#include "sa/Pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>

#include "sa/Dsp.hpp"
#include "sa/Udp.hpp"

namespace sa {

using namespace std::chrono_literals;

namespace {

constexpr std::size_t kChunkSamples = 4096;  // most samples taken from one ring per turn

void validateBand(float loHz, float hiHz) {
    if (!(loHz >= 0.0f && loHz < hiHz)) {
        throw std::invalid_argument("Envelope band must satisfy 0 <= low < high");
    }
}

}  // namespace

// Analysis state of one channel, owned by the DSP thread.
struct Pipeline::ChannelAnalysis {
    ChannelAnalysis(std::uint16_t channelId, const PipelineConfig& config)
        : id(channelId),
          analyzer(config.fftSize),
          envelope(config.fftSize),
          windowLen(std::max(config.fftSize, config.waveformSamples)),
          nextFrameEnd(config.fftSize) {
        history.reserve(windowLen * 2 + kChunkSamples);
    }

    void resetFatigue() {
        rainflow.reset();
        closedCycles.clear();
        fatigueSeconds = 0.0;
    }

    std::uint16_t id;
    SpectrumAnalyzer analyzer;
    EnvelopeAnalyzer envelope;
    std::size_t windowLen;
    std::vector<float> history;  // the last history.size() samples of the stream
    std::uint64_t streamEnd = 0;  // samples received so far
    std::uint64_t nextFrameEnd;   // stream position where the next frame ends
    std::uint64_t frameIndex = 0;

    RainflowCounter rainflow;
    RainflowMatrix closedCycles;
    double fatigueSeconds = 0.0;
};

Pipeline::Pipeline(PipelineConfig config) : config_(config) {
    if (config_.hopSize == 0 || config_.hopSize > config_.fftSize) {
        throw std::invalid_argument("hopSize must be in (0, fftSize]");
    }
    if (config_.frameHistory == 0) {
        throw std::invalid_argument("frameHistory must be at least 1");
    }
    SpectrumAnalyzer validate(config_.fftSize);  // throws for a bad FFT size
    setEnvelopeBand(config_.envelopeBandLoHz, config_.envelopeBandHiHz);
}

void Pipeline::setEnvelopeBand(float loHz, float hiHz) {
    validateBand(loHz, hiHz);
    std::lock_guard lock(bandMutex_);
    envelopeBandLoHz_ = loHz;
    envelopeBandHiHz_ = hiHz;
}

Pipeline::~Pipeline() { stop(); }

void Pipeline::start() {
    if (receiver_) {
        throw std::logic_error("Pipeline can only be started once; create a new instance");
    }
    receiver_ = std::make_unique<UdpReceiver>(config_.port);
    running_ = true;
    dspThread_ = std::jthread([this](std::stop_token st) { dspLoop(st); });
    receiveThread_ = std::jthread([this](std::stop_token st) { receiveLoop(st); });
}

void Pipeline::stop() {
    receiveThread_.request_stop();
    dspThread_.request_stop();
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    if (dspThread_.joinable()) {
        dspThread_.join();
    }
    running_ = false;
}

std::uint16_t Pipeline::port() const { return receiver_ ? receiver_->port() : config_.port; }

Pipeline::Channel* Pipeline::channel(std::uint16_t id) const {
    return id < kMaxChannels ? published_[id].load(std::memory_order_acquire) : nullptr;
}

Pipeline::Channel& Pipeline::channelForPacket(std::uint16_t id) {
    if (!ownedChannels_[id]) {
        ownedChannels_[id] = std::make_unique<Channel>(config_.ringCapacity);
        // Release: the DSP and UI threads see a fully constructed channel.
        published_[id].store(ownedChannels_[id].get(), std::memory_order_release);
    }
    return *ownedChannels_[id];
}

std::vector<std::uint16_t> Pipeline::channels() const {
    std::vector<std::uint16_t> ids;
    for (std::uint16_t id = 0; id < kMaxChannels; ++id) {
        if (channel(id)) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::shared_ptr<const AnalysisFrame> Pipeline::latestFrame(std::uint16_t channel) const {
    if (channel >= kMaxChannels) {
        return nullptr;
    }
    std::lock_guard lock(frameMutex_);
    const auto& frames = recentFrames_[channel];
    return frames.empty() ? nullptr : frames.back();
}

std::vector<std::shared_ptr<const AnalysisFrame>> Pipeline::framesSince(
    std::uint16_t channel, std::uint64_t afterIndex) const {
    if (channel >= kMaxChannels) {
        return {};
    }
    std::lock_guard lock(frameMutex_);
    const auto& frames = recentFrames_[channel];
    const auto first = std::find_if(frames.begin(), frames.end(),
                                    [afterIndex](const auto& f) { return f->index > afterIndex; });
    return {first, frames.end()};
}

PipelineStats Pipeline::stats() const {
    PipelineStats s;
    s.packetsMalformed = packetsMalformed_.load();
    for (std::uint16_t id = 0; id < kMaxChannels; ++id) {
        const Channel* ch = channel(id);
        if (!ch) {
            continue;
        }
        ChannelStats c;
        c.channel = id;
        c.sampleRate = ch->sampleRate.load();
        c.packetsReceived = ch->packetsReceived.load();
        c.packetsLost = ch->packetsLost.load();
        c.samplesDropped = ch->samplesDropped.load();
        c.framesComputed = ch->framesComputed.load();
        c.bufferFill = ch->ring.size();

        s.packetsReceived += c.packetsReceived;
        s.packetsLost += c.packetsLost;
        s.samplesDropped += c.samplesDropped;
        s.framesComputed += c.framesComputed;
        s.bufferFill += c.bufferFill;
        s.channels.push_back(c);
    }
    return s;
}

std::string Pipeline::lastError() const {
    std::lock_guard lock(errorMutex_);
    return lastError_;
}

void Pipeline::setError(const std::string& message) {
    std::lock_guard lock(errorMutex_);
    lastError_ = message;
}

void Pipeline::receiveLoop(std::stop_token stop) {
    std::vector<std::byte> datagram(65536);
    std::vector<float> samples;
    samples.reserve(kMaxSamplesPerPacket);
    PacketHeader header;

    try {
        while (!stop.stop_requested()) {
            const std::size_t n = receiver_->receive(datagram, 100ms);
            if (n == 0) {
                continue;
            }
            if (!parsePacket(std::span(datagram.data(), n), header, samples)) {
                ++packetsMalformed_;
                continue;
            }
            Channel& ch = channelForPacket(header.channel);

            if (ch.haveSequence && header.sequence != ch.expectedSequence) {
                // Forward gap = lost packets. A large "negative" jump means the
                // sender restarted, which is not a loss.
                const std::uint32_t gap = header.sequence - ch.expectedSequence;
                if (gap < 0x8000'0000u) {
                    ch.packetsLost += gap;
                }
            }
            ch.haveSequence = true;
            ch.expectedSequence = header.sequence + 1;

            ch.sampleRate.store(header.sampleRate, std::memory_order_relaxed);
            const std::size_t pushed = ch.ring.push(samples);
            ch.samplesDropped += samples.size() - pushed;
            ++ch.packetsReceived;
        }
    } catch (const std::exception& e) {
        setError(std::string("Receive thread stopped: ") + e.what());
        running_ = false;
    }
}

void Pipeline::dspLoop(std::stop_token stop) {
    std::array<std::unique_ptr<ChannelAnalysis>, kMaxChannels> analyses;
    std::vector<float> chunk(kChunkSamples);

    while (!stop.stop_requested()) {
        const bool resetFatigue = fatigueResetRequested_.exchange(false);
        float bandLo = 0.0f;
        float bandHi = 0.0f;
        {
            std::lock_guard lock(bandMutex_);
            bandLo = envelopeBandLoHz_;
            bandHi = envelopeBandHiHz_;
        }

        // Round-robin, one chunk per channel per turn, so a busy channel cannot
        // starve the others.
        bool anyData = false;
        for (std::uint16_t id = 0; id < kMaxChannels; ++id) {
            Channel* ch = channel(id);
            if (!ch) {
                continue;
            }
            auto& analysis = analyses[id];
            if (!analysis) {
                analysis = std::make_unique<ChannelAnalysis>(id, config_);
            }
            if (resetFatigue) {
                analysis->resetFatigue();
            }
            const std::size_t n = ch->ring.pop(chunk);
            if (n > 0) {
                anyData = true;
                analyse(*analysis, *ch, std::span<const float>(chunk.data(), n), bandLo, bandHi);
            }
        }
        if (!anyData) {
            // Polling keeps the producer path lock- and syscall-free.
            // 1 ms is far below one packet period at typical rates.
            std::this_thread::sleep_for(1ms);
        }
    }
}

void Pipeline::analyse(ChannelAnalysis& a, Channel& ch, std::span<const float> chunk,
                       float bandLo, float bandHi) {
    a.history.insert(a.history.end(), chunk.begin(), chunk.end());
    a.streamEnd += chunk.size();
    const std::uint32_t fs = ch.sampleRate.load(std::memory_order_relaxed);

    // Fatigue needs every sample, not just the analysed windows.
    a.rainflow.push(chunk, [&a](const RainflowCycle& c) { a.closedCycles.add(c.range, c.mean, c.count); });
    if (fs != 0) {
        a.fatigueSeconds += static_cast<double>(chunk.size()) / fs;
    }
    // Built at most once per chunk; frames from the same chunk share it.
    std::optional<RainflowMatrix> fatigueSnapshot;

    // Frames end exactly every hopSize samples (a regular STFT), even when one
    // chunk carries several hops' worth of new data. The spectrogram relies on it.
    while (a.nextFrameEnd <= a.streamEnd) {
        const auto behind = static_cast<std::size_t>(a.streamEnd - a.nextFrameEnd);
        a.nextFrameEnd += config_.hopSize;
        if (fs == 0 || behind + config_.fftSize > a.history.size()) {
            continue;
        }
        const std::size_t end = a.history.size() - behind;

        const std::span<const float> fftBlock(a.history.data() + end - config_.fftSize,
                                              config_.fftSize);
        const std::size_t waveLen = std::min(config_.waveformSamples, end);
        const std::span<const float> waveBlock(a.history.data() + end - waveLen, waveLen);

        auto frame = std::make_shared<AnalysisFrame>();
        frame->channel = a.id;
        frame->index = ++a.frameIndex;
        frame->sampleRate = fs;
        frame->binHz = static_cast<float>(fs) / static_cast<float>(config_.fftSize);
        frame->spectrum = a.analyzer.compute(fftBlock);
        frame->waveform.assign(waveBlock.begin(), waveBlock.end());
        frame->rms = rms(fftBlock);
        frame->peak = peakAbs(fftBlock);
        frame->dominantHz = dominantFrequency(frame->spectrum, frame->binHz);

        frame->envelopeSpectrum = a.envelope.compute(fftBlock, frame->binHz, bandLo, bandHi);
        frame->envelopeBandLoHz = bandLo;
        frame->envelopeBandHiHz = bandHi;
        // The envelope cannot fluctuate faster than the band is wide.
        const std::size_t envelopeBins =
            std::min(frame->envelopeSpectrum.size(),
                     static_cast<std::size_t>((bandHi - bandLo) / frame->binHz) + 1);
        // Bins 0 and 1 are the Hann main lobe around DC: any slow drift of the
        // envelope (block edges, level changes) lands there. Searching from bin 2
        // keeps that from posing as a line; both helpers skip their first bin.
        const auto envelopeRange =
            std::span<const float>(frame->envelopeSpectrum).first(envelopeBins).subspan(1);
        const float peakHz = dominantFrequency(envelopeRange, frame->binHz);
        frame->envelopePeakHz = peakHz > 0.0f ? peakHz + frame->binHz : 0.0f;
        frame->envelopePeakProminenceDb = peakProminenceDb(envelopeRange);

        if (!fatigueSnapshot) {
            fatigueSnapshot = a.closedCycles;
            for (const RainflowCycle& c : a.rainflow.residue()) {
                fatigueSnapshot->add(c.range, c.mean, c.count);
            }
        }
        frame->rainflow = *fatigueSnapshot;
        frame->fatigueSeconds = a.fatigueSeconds;

        {
            std::lock_guard lock(frameMutex_);
            auto& frames = recentFrames_[a.id];
            frames.push_back(std::move(frame));
            if (frames.size() > config_.frameHistory) {
                frames.pop_front();
            }
        }
        ++ch.framesComputed;
    }

    // Trim only after analysis: the frames above may have needed older samples.
    if (a.history.size() > 2 * a.windowLen) {
        a.history.erase(a.history.begin(),
                        a.history.end() - static_cast<std::ptrdiff_t>(a.windowLen));
    }
}

}  // namespace sa

#include "sa/Pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>

#include "sa/Dsp.hpp"
#include "sa/Protocol.hpp"
#include "sa/Udp.hpp"

namespace sa {

using namespace std::chrono_literals;

namespace {

void validateBand(float loHz, float hiHz) {
    if (!(loHz >= 0.0f && loHz < hiHz)) {
        throw std::invalid_argument("Envelope band must satisfy 0 <= low < high");
    }
}

}  // namespace

Pipeline::Pipeline(PipelineConfig config)
    : config_(config), ring_(config.ringCapacity) {
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

std::shared_ptr<const AnalysisFrame> Pipeline::latestFrame() const {
    std::lock_guard lock(frameMutex_);
    return recentFrames_.empty() ? nullptr : recentFrames_.back();
}

std::vector<std::shared_ptr<const AnalysisFrame>> Pipeline::framesSince(
    std::uint64_t afterIndex) const {
    std::lock_guard lock(frameMutex_);
    const auto first = std::find_if(recentFrames_.begin(), recentFrames_.end(),
                                    [afterIndex](const auto& f) { return f->index > afterIndex; });
    return {first, recentFrames_.end()};
}

PipelineStats Pipeline::stats() const {
    PipelineStats s;
    s.packetsReceived = packetsReceived_.load();
    s.packetsLost = packetsLost_.load();
    s.packetsMalformed = packetsMalformed_.load();
    s.samplesDropped = samplesDropped_.load();
    s.framesComputed = framesComputed_.load();
    s.bufferFill = ring_.size();
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
    bool haveSequence = false;
    std::uint32_t expectedSequence = 0;

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

            if (haveSequence && header.sequence != expectedSequence) {
                // Forward gap = lost packets. A large "negative" jump means the
                // sender restarted, which is not a loss.
                const std::uint32_t gap = header.sequence - expectedSequence;
                if (gap < 0x8000'0000u) {
                    packetsLost_ += gap;
                }
            }
            haveSequence = true;
            expectedSequence = header.sequence + 1;

            sampleRate_.store(header.sampleRate, std::memory_order_relaxed);
            const std::size_t pushed = ring_.push(samples);
            samplesDropped_ += samples.size() - pushed;
            ++packetsReceived_;
        }
    } catch (const std::exception& e) {
        setError(std::string("Receive thread stopped: ") + e.what());
        running_ = false;
    }
}

void Pipeline::dspLoop(std::stop_token stop) {
    SpectrumAnalyzer analyzer(config_.fftSize);
    EnvelopeAnalyzer envelope(config_.fftSize);
    const std::size_t windowLen = std::max(config_.fftSize, config_.waveformSamples);

    std::vector<float> chunk(4096);
    std::vector<float> history;  // the last history.size() samples of the stream
    history.reserve(windowLen * 2 + chunk.size());
    std::uint64_t streamEnd = 0;                   // samples received so far
    std::uint64_t nextFrameEnd = config_.fftSize;  // stream position where the next frame ends
    std::uint64_t frameIndex = 0;

    RainflowCounter rainflow;
    RainflowMatrix closedCycles;
    double fatigueSeconds = 0.0;
    const RainflowCounter::CycleSink countCycle = [&closedCycles](const RainflowCycle& c) {
        closedCycles.add(c.range, c.mean, c.count);
    };

    while (!stop.stop_requested()) {
        const std::size_t n = ring_.pop(chunk);
        if (n == 0) {
            // Polling keeps the producer path lock- and syscall-free.
            // 1 ms is far below one packet period at typical rates.
            std::this_thread::sleep_for(1ms);
            continue;
        }

        history.insert(history.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        streamEnd += n;

        // Frames end exactly every hopSize samples (a regular STFT), even when one
        // chunk carries several hops' worth of new data. The spectrogram relies on it.
        const std::uint32_t fs = sampleRate_.load(std::memory_order_relaxed);
        float bandLo = 0.0f;
        float bandHi = 0.0f;
        {
            std::lock_guard lock(bandMutex_);
            bandLo = envelopeBandLoHz_;
            bandHi = envelopeBandHiHz_;
        }

        // Fatigue needs every sample, not just the analysed windows.
        if (fatigueResetRequested_.exchange(false)) {
            rainflow.reset();
            closedCycles.clear();
            fatigueSeconds = 0.0;
        }
        rainflow.push(std::span<const float>(chunk.data(), n), countCycle);
        if (fs != 0) {
            fatigueSeconds += static_cast<double>(n) / fs;
        }
        // Built at most once per chunk; frames from the same chunk share it.
        std::optional<RainflowMatrix> fatigueSnapshot;

        while (nextFrameEnd <= streamEnd) {
            const auto behind = static_cast<std::size_t>(streamEnd - nextFrameEnd);
            nextFrameEnd += config_.hopSize;
            if (fs == 0 || behind + config_.fftSize > history.size()) {
                continue;
            }
            const std::size_t end = history.size() - behind;

            const std::span<const float> fftBlock(history.data() + end - config_.fftSize,
                                                  config_.fftSize);
            const std::size_t waveLen = std::min(config_.waveformSamples, end);
            const std::span<const float> waveBlock(history.data() + end - waveLen, waveLen);

            auto frame = std::make_shared<AnalysisFrame>();
            frame->index = ++frameIndex;
            frame->sampleRate = fs;
            frame->binHz = static_cast<float>(fs) / static_cast<float>(config_.fftSize);
            frame->spectrum = analyzer.compute(fftBlock);
            frame->waveform.assign(waveBlock.begin(), waveBlock.end());
            frame->rms = rms(fftBlock);
            frame->peak = peakAbs(fftBlock);
            frame->dominantHz = dominantFrequency(frame->spectrum, frame->binHz);

            frame->envelopeSpectrum = envelope.compute(fftBlock, frame->binHz, bandLo, bandHi);
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
                fatigueSnapshot = closedCycles;
                for (const RainflowCycle& c : rainflow.residue()) {
                    fatigueSnapshot->add(c.range, c.mean, c.count);
                }
            }
            frame->rainflow = *fatigueSnapshot;
            frame->fatigueSeconds = fatigueSeconds;

            {
                std::lock_guard lock(frameMutex_);
                recentFrames_.push_back(std::move(frame));
                if (recentFrames_.size() > config_.frameHistory) {
                    recentFrames_.pop_front();
                }
            }
            ++framesComputed_;
        }

        // Trim only after analysis: the frames above may have needed older samples.
        if (history.size() > 2 * windowLen) {
            history.erase(history.begin(),
                          history.end() - static_cast<std::ptrdiff_t>(windowLen));
        }
    }
}

}  // namespace sa

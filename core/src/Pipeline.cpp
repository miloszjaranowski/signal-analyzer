#include "sa/Pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <span>
#include <stdexcept>

#include "sa/Dsp.hpp"
#include "sa/Protocol.hpp"
#include "sa/Udp.hpp"

namespace sa {

using namespace std::chrono_literals;

Pipeline::Pipeline(PipelineConfig config)
    : config_(config), ring_(config.ringCapacity) {
    if (config_.hopSize == 0 || config_.hopSize > config_.fftSize) {
        throw std::invalid_argument("hopSize must be in (0, fftSize]");
    }
    SpectrumAnalyzer validate(config_.fftSize);  // throws for a bad FFT size
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
    return latestFrame_;
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
    const std::size_t windowLen = std::max(config_.fftSize, config_.waveformSamples);

    std::vector<float> chunk(4096);
    std::vector<float> history;
    history.reserve(windowLen * 2 + chunk.size());
    std::size_t newSamples = 0;
    std::uint64_t frameIndex = 0;

    while (!stop.stop_requested()) {
        const std::size_t n = ring_.pop(chunk);
        if (n == 0) {
            // Polling keeps the producer path lock- and syscall-free.
            // 1 ms is far below one packet period at typical rates.
            std::this_thread::sleep_for(1ms);
            continue;
        }

        history.insert(history.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        newSamples += n;
        if (history.size() > 2 * windowLen) {
            history.erase(history.begin(),
                          history.end() - static_cast<std::ptrdiff_t>(windowLen));
        }

        const std::uint32_t fs = sampleRate_.load(std::memory_order_relaxed);
        if (history.size() < config_.fftSize || newSamples < config_.hopSize || fs == 0) {
            continue;
        }
        newSamples = 0;

        const std::span<const float> fftBlock(history.data() + history.size() - config_.fftSize,
                                              config_.fftSize);
        const std::size_t waveLen = std::min(config_.waveformSamples, history.size());
        const std::span<const float> waveBlock(history.data() + history.size() - waveLen, waveLen);

        auto frame = std::make_shared<AnalysisFrame>();
        frame->index = ++frameIndex;
        frame->sampleRate = fs;
        frame->binHz = static_cast<float>(fs) / static_cast<float>(config_.fftSize);
        frame->spectrum = analyzer.compute(fftBlock);
        frame->waveform.assign(waveBlock.begin(), waveBlock.end());
        frame->rms = rms(fftBlock);
        frame->peak = peakAbs(fftBlock);
        frame->dominantHz = dominantFrequency(frame->spectrum, frame->binHz);

        {
            std::lock_guard lock(frameMutex_);
            latestFrame_ = std::move(frame);
        }
        ++framesComputed_;
    }
}

}  // namespace sa

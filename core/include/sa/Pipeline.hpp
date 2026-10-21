#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "sa/SpscRingBuffer.hpp"

namespace sa {

class UdpReceiver;

struct PipelineConfig {
    std::uint16_t port = 5005;
    std::size_t fftSize = 4096;              // power of two
    std::size_t hopSize = 1024;              // new samples between analyses
    std::size_t waveformSamples = 2048;      // samples kept for the time plot
    std::size_t ringCapacity = 1u << 18;     // ~26 s at 10 kHz
};

// One analysis result. Immutable once published; shared with the UI by shared_ptr.
struct AnalysisFrame {
    std::uint64_t index = 0;
    std::uint32_t sampleRate = 0;
    float binHz = 0.0f;
    float rms = 0.0f;
    float peak = 0.0f;
    float dominantHz = 0.0f;
    std::vector<float> waveform;
    std::vector<float> spectrum;  // single-sided amplitude, fftSize/2 + 1 bins
};

struct PipelineStats {
    std::uint64_t packetsReceived = 0;
    std::uint64_t packetsLost = 0;       // gaps in sequence numbers
    std::uint64_t packetsMalformed = 0;
    std::uint64_t samplesDropped = 0;    // ring buffer full (consumer too slow)
    std::uint64_t framesComputed = 0;
    std::size_t bufferFill = 0;
};

// Two worker threads connected by a lock-free SPSC ring buffer:
//
//   [UDP] -> receive thread -> SpscRingBuffer<float> -> DSP thread -> latest AnalysisFrame
//
// The receive thread only parses and copies, so it is never blocked by DSP work.
// The UI (any thread) polls latestFrame(); the swap is a short mutex-guarded
// shared_ptr exchange, so readers never see a half-written frame.
class Pipeline {
public:
    explicit Pipeline(PipelineConfig config);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void start();  // throws std::runtime_error if the socket cannot be opened
    void stop();   // idempotent; joins both threads
    bool running() const { return running_.load(); }

    std::uint16_t port() const;
    const PipelineConfig& config() const { return config_; }

    std::shared_ptr<const AnalysisFrame> latestFrame() const;
    PipelineStats stats() const;
    std::string lastError() const;

private:
    void receiveLoop(std::stop_token stop);
    void dspLoop(std::stop_token stop);
    void setError(const std::string& message);

    PipelineConfig config_;
    SpscRingBuffer<float> ring_;
    std::unique_ptr<UdpReceiver> receiver_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> sampleRate_{0};

    std::atomic<std::uint64_t> packetsReceived_{0};
    std::atomic<std::uint64_t> packetsLost_{0};
    std::atomic<std::uint64_t> packetsMalformed_{0};
    std::atomic<std::uint64_t> samplesDropped_{0};
    std::atomic<std::uint64_t> framesComputed_{0};

    mutable std::mutex frameMutex_;
    std::shared_ptr<const AnalysisFrame> latestFrame_;

    mutable std::mutex errorMutex_;
    std::string lastError_;

    // Declared last: destroyed (and joined) first.
    std::jthread receiveThread_;
    std::jthread dspThread_;
};

}  // namespace sa

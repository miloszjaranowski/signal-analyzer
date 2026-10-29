#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "sa/Fatigue.hpp"
#include "sa/Protocol.hpp"
#include "sa/SpscRingBuffer.hpp"

namespace sa {

class UdpReceiver;

struct PipelineConfig {
    std::uint16_t port = 5005;
    std::size_t fftSize = 4096;              // power of two
    std::size_t hopSize = 1024;              // new samples between analyses
    std::size_t waveformSamples = 2048;      // samples kept for the time plot
    std::size_t ringCapacity = 1u << 18;     // per channel; ~26 s at 10 kHz
    std::size_t frameHistory = 64;           // recent frames per channel kept for framesSince()
    float envelopeBandLoHz = 1000.0f;        // band-pass before envelope detection,
    float envelopeBandHiHz = 4000.0f;        // ideally around a structural resonance
};

// One analysis result of one channel. Immutable once published; shared with the
// UI by shared_ptr.
struct AnalysisFrame {
    std::uint16_t channel = 0;
    std::uint64_t index = 0;  // per channel, starting at 1
    std::uint32_t sampleRate = 0;
    float binHz = 0.0f;
    float rms = 0.0f;
    float peak = 0.0f;
    float dominantHz = 0.0f;
    std::vector<float> waveform;
    std::vector<float> spectrum;  // single-sided amplitude, fftSize/2 + 1 bins

    std::vector<float> envelopeSpectrum;  // amplitude spectrum of the band envelope, same bins
    float envelopeBandLoHz = 0.0f;        // band the envelope was taken from
    float envelopeBandHiHz = 0.0f;
    float envelopePeakHz = 0.0f;          // strongest envelope component, e.g. a fault rate
    float envelopePeakProminenceDb = 0.0f;  // how far it stands above the envelope noise

    // Rainflow matrix of every sample since start (or resetFatigue()), including
    // the current residue as half cycles, and the signal time it covers.
    RainflowMatrix rainflow;
    double fatigueSeconds = 0.0;
};

struct ChannelStats {
    std::uint16_t channel = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t packetsReceived = 0;
    std::uint64_t packetsLost = 0;       // gaps in this channel's sequence numbers
    std::uint64_t samplesDropped = 0;    // ring buffer full (consumer too slow)
    std::uint64_t framesComputed = 0;
    std::size_t bufferFill = 0;
};

struct PipelineStats {
    // Totals over all channels.
    std::uint64_t packetsReceived = 0;
    std::uint64_t packetsLost = 0;
    std::uint64_t packetsMalformed = 0;  // not attributable to a channel
    std::uint64_t samplesDropped = 0;
    std::uint64_t framesComputed = 0;
    std::size_t bufferFill = 0;

    std::vector<ChannelStats> channels;  // channels seen so far, ascending
};

// Two worker threads; every channel has its own lock-free SPSC ring buffer:
//
//   [UDP] -> receive thread -+-> ring (ch 0) -+-> DSP thread -> recent AnalysisFrames per channel
//                            +-> ring (ch 1) -+
//                            +-> ...         -+
//
// The receive thread only parses, checks sequence numbers and copies, so it is
// never blocked by DSP work. A channel appears with its first valid packet. The
// DSP thread serves the channels round-robin, a chunk at a time. The UI (any
// thread) polls latestFrame() or framesSince(); publishing is a short
// mutex-guarded push of a shared_ptr, so readers never see a half-written frame.
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

    // Thread-safe; applies to all channels from their next frame. Throws
    // std::invalid_argument unless 0 <= loHz < hiHz.
    void setEnvelopeBand(float loHz, float hiHz);

    // Thread-safe; restarts rainflow counting on all channels.
    void resetFatigue() { fatigueResetRequested_ = true; }

    // Channels that have sent at least one valid packet, ascending.
    std::vector<std::uint16_t> channels() const;

    std::shared_ptr<const AnalysisFrame> latestFrame(std::uint16_t channel = 0) const;
    // Frames of `channel` with index > afterIndex, oldest first (at most
    // config().frameHistory). Lets a consumer that polls slower than the DSP rate
    // still see every frame.
    std::vector<std::shared_ptr<const AnalysisFrame>> framesSince(std::uint16_t channel,
                                                                  std::uint64_t afterIndex) const;
    PipelineStats stats() const;
    std::string lastError() const;

private:
    // Input side of one channel. Created by the receive thread on the channel's
    // first packet, published through published_ and kept until destruction.
    struct Channel {
        explicit Channel(std::size_t ringCapacity) : ring(ringCapacity) {}

        SpscRingBuffer<float> ring;  // receive thread -> DSP thread
        std::atomic<std::uint32_t> sampleRate{0};
        std::atomic<std::uint64_t> packetsReceived{0};
        std::atomic<std::uint64_t> packetsLost{0};
        std::atomic<std::uint64_t> samplesDropped{0};
        std::atomic<std::uint64_t> framesComputed{0};  // written by the DSP thread
        bool haveSequence = false;                     // receive thread only
        std::uint32_t expectedSequence = 0;
    };
    struct ChannelAnalysis;  // DSP-thread state of one channel, see Pipeline.cpp

    void receiveLoop(std::stop_token stop);
    void dspLoop(std::stop_token stop);
    void analyse(ChannelAnalysis& analysis, Channel& channel, std::span<const float> chunk,
                 float bandLo, float bandHi);
    Channel& channelForPacket(std::uint16_t id);  // receive thread only
    Channel* channel(std::uint16_t id) const;     // nullptr until the first packet
    void setError(const std::string& message);

    PipelineConfig config_;
    std::unique_ptr<UdpReceiver> receiver_;

    std::array<std::unique_ptr<Channel>, kMaxChannels> ownedChannels_;  // receive thread writes
    std::array<std::atomic<Channel*>, kMaxChannels> published_{};       // everyone reads

    std::atomic<bool> running_{false};
    std::atomic<bool> fatigueResetRequested_{false};
    std::atomic<std::uint64_t> packetsMalformed_{0};

    mutable std::mutex frameMutex_;
    std::array<std::deque<std::shared_ptr<const AnalysisFrame>>, kMaxChannels> recentFrames_;

    std::mutex bandMutex_;
    float envelopeBandLoHz_ = 0.0f;
    float envelopeBandHiHz_ = 0.0f;

    mutable std::mutex errorMutex_;
    std::string lastError_;

    // Declared last: destroyed (and joined) first.
    std::jthread receiveThread_;
    std::jthread dspThread_;
};

}  // namespace sa

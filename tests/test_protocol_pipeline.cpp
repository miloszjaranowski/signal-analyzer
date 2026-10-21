#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>

#include "sa/Pipeline.hpp"
#include "sa/Protocol.hpp"
#include "sa/Udp.hpp"

using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kFs = 8000;
constexpr std::size_t kBlock = 256;
constexpr double kFreq = 437.5;  // exactly on a bin for fs/1024 = 7.8125 Hz

// Sends `packets` datagrams of a unit sine to the pipeline, skipping the sequence
// numbers in `lost`, then waits until the pipeline has received the rest.
void streamSine(sa::Pipeline& pipeline, std::uint32_t packets,
                std::initializer_list<std::uint32_t> lost = {}) {
    sa::UdpSender sender("127.0.0.1", pipeline.port());
    std::vector<float> samples(kBlock);
    std::uint64_t sampleIndex = 0;

    for (std::uint32_t seq = 0; seq < packets; ++seq) {
        for (std::size_t i = 0; i < kBlock; ++i, ++sampleIndex) {
            samples[i] = static_cast<float>(
                std::sin(2.0 * std::numbers::pi * kFreq * double(sampleIndex) / kFs));
        }
        if (std::find(lost.begin(), lost.end(), seq) != lost.end()) {
            continue;
        }
        sa::PacketHeader h;
        h.sequence = seq;
        h.sampleRate = kFs;
        sender.send(sa::serializePacket(h, samples));
        std::this_thread::sleep_for(1ms);  // loopback is fast; avoid overrunning the socket
    }

    const std::uint64_t expected = packets - lost.size();
    for (int i = 0; i < 200 && pipeline.stats().packetsReceived < expected; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    std::this_thread::sleep_for(50ms);  // let the DSP thread drain the ring buffer
}

}  // namespace

TEST(Protocol, RoundTrip) {
    sa::PacketHeader h;
    h.sequence = 42;
    h.sampleRate = 10'000;
    const std::vector<float> samples{0.5f, -1.0f, 3.25f};

    const auto bytes = sa::serializePacket(h, samples);
    ASSERT_EQ(bytes.size(), sa::kHeaderSize + 3 * sizeof(float));

    sa::PacketHeader parsed;
    std::vector<float> out;
    ASSERT_TRUE(sa::parsePacket(bytes, parsed, out));
    EXPECT_EQ(parsed.sequence, 42u);
    EXPECT_EQ(parsed.sampleRate, 10'000u);
    EXPECT_EQ(parsed.sampleCount, 3u);
    EXPECT_EQ(out, samples);
}

TEST(Protocol, RejectsMalformed) {
    sa::PacketHeader h;
    h.sampleRate = 1000;
    const std::vector<float> samples(4, 1.0f);
    sa::PacketHeader parsed;
    std::vector<float> out;

    auto truncated = sa::serializePacket(h, samples);
    truncated.pop_back();
    EXPECT_FALSE(sa::parsePacket(truncated, parsed, out));

    auto badMagic = sa::serializePacket(h, samples);
    badMagic[0] = std::byte{0};
    EXPECT_FALSE(sa::parsePacket(badMagic, parsed, out));

    sa::PacketHeader zeroRate;
    EXPECT_FALSE(sa::parsePacket(sa::serializePacket(zeroRate, samples), parsed, out));

    EXPECT_FALSE(sa::parsePacket({}, parsed, out));
}

// End-to-end over loopback: real sockets, real threads.
TEST(Pipeline, AnalysesSineAndCountsLostPackets) {
    sa::PipelineConfig cfg;
    cfg.port = 0;  // ephemeral
    cfg.fftSize = 1024;
    cfg.hopSize = 256;
    sa::Pipeline pipeline(cfg);
    pipeline.start();

    streamSine(pipeline, 40, {20, 21});  // simulate two lost datagrams
    const auto frame = pipeline.latestFrame();
    const auto stats = pipeline.stats();
    pipeline.stop();

    EXPECT_EQ(stats.packetsReceived, 38u);
    EXPECT_EQ(stats.packetsLost, 2u);
    EXPECT_EQ(stats.samplesDropped, 0u);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->sampleRate, kFs);
    EXPECT_NEAR(frame->dominantHz, kFreq, 1.0);
    EXPECT_NEAR(frame->rms, 1.0 / std::sqrt(2.0), 0.02);
    EXPECT_TRUE(pipeline.lastError().empty());
}

TEST(Pipeline, RejectsInvalidEnvelopeBand) {
    sa::PipelineConfig cfg;
    cfg.envelopeBandLoHz = 3000.0f;
    cfg.envelopeBandHiHz = 2000.0f;
    EXPECT_THROW(sa::Pipeline{cfg}, std::invalid_argument);

    sa::Pipeline pipeline(sa::PipelineConfig{});
    EXPECT_THROW(pipeline.setEnvelopeBand(-1.0f, 100.0f), std::invalid_argument);
    EXPECT_THROW(pipeline.setEnvelopeBand(500.0f, 500.0f), std::invalid_argument);
    EXPECT_NO_THROW(pipeline.setEnvelopeBand(0.0f, 500.0f));
}

TEST(Pipeline, FramesSinceReturnsRecentFramesInOrder) {
    sa::PipelineConfig cfg;
    cfg.port = 0;
    cfg.fftSize = 1024;
    cfg.hopSize = 384;  // not a multiple of the packet size
    cfg.frameHistory = 4;
    sa::Pipeline pipeline(cfg);
    pipeline.start();

    streamSine(pipeline, 40);
    pipeline.stop();  // frames stay readable after stop; no more are published

    // One frame per hop, however the samples arrive: 40 * 256 = 10240 samples,
    // frames end at 1024, 1408, ..., 10240.
    const auto stats = pipeline.stats();
    ASSERT_EQ(stats.framesComputed, (40u * kBlock - cfg.fftSize) / cfg.hopSize + 1);

    const auto all = pipeline.framesSince(0);
    ASSERT_EQ(all.size(), cfg.frameHistory);  // older frames were evicted
    for (std::size_t i = 1; i < all.size(); ++i) {
        EXPECT_EQ(all[i]->index, all[i - 1]->index + 1);
    }
    EXPECT_EQ(all.back()->index, stats.framesComputed);
    EXPECT_EQ(all.back(), pipeline.latestFrame());

    const auto tail = pipeline.framesSince(all[1]->index);
    ASSERT_EQ(tail.size(), 2u);
    EXPECT_EQ(tail.front(), all[2]);
    EXPECT_TRUE(pipeline.framesSince(all.back()->index).empty());
}

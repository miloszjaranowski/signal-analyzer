#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "sa/Pipeline.hpp"
#include "sa/Protocol.hpp"
#include "sa/Udp.hpp"

using namespace std::chrono_literals;

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

    sa::UdpSender sender("127.0.0.1", pipeline.port());
    constexpr std::uint32_t fs = 8000;
    constexpr std::size_t block = 256;
    constexpr double freq = 437.5;  // exactly on a bin for fs/1024 = 7.8125 Hz
    std::vector<float> samples(block);
    std::uint64_t sampleIndex = 0;

    for (std::uint32_t seq = 0; seq < 40; ++seq) {
        for (std::size_t i = 0; i < block; ++i, ++sampleIndex) {
            samples[i] = static_cast<float>(
                std::sin(2.0 * std::numbers::pi * freq * double(sampleIndex) / fs));
        }
        if (seq == 20 || seq == 21) {
            continue;  // simulate two lost datagrams
        }
        sa::PacketHeader h;
        h.sequence = seq;
        h.sampleRate = fs;
        sender.send(sa::serializePacket(h, samples));
        std::this_thread::sleep_for(1ms);  // loopback is fast; avoid overrunning the socket
    }

    std::shared_ptr<const sa::AnalysisFrame> frame;
    for (int i = 0; i < 200 && pipeline.stats().packetsReceived < 38; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    std::this_thread::sleep_for(50ms);
    frame = pipeline.latestFrame();
    const auto stats = pipeline.stats();
    pipeline.stop();

    EXPECT_EQ(stats.packetsReceived, 38u);
    EXPECT_EQ(stats.packetsLost, 2u);
    EXPECT_EQ(stats.samplesDropped, 0u);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->sampleRate, fs);
    EXPECT_NEAR(frame->dominantHz, freq, 1.0);
    EXPECT_NEAR(frame->rms, 1.0 / std::sqrt(2.0), 0.02);
    EXPECT_TRUE(pipeline.lastError().empty());
}

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "sa/Capture.hpp"
#include "sa/Pipeline.hpp"
#include "sa/Protocol.hpp"
#include "sa/Udp.hpp"

using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kFs = 8000;
constexpr std::size_t kBlock = 256;
constexpr double kFreq = 437.5;  // on a bin for fs/1024

// A unique file in the temp directory, removed when the test ends.
class TempFile {
public:
    explicit TempFile(const std::string& name) {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("sa_test_" + name + "_" + std::to_string(rd()) + ".sacap");
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (int v : values) {
        out.push_back(std::byte(v));
    }
    return out;
}

std::vector<std::byte> sinePacket(std::uint16_t channel, std::uint32_t seq, double freq, double amplitude) {
    std::vector<float> samples(kBlock);
    for (std::size_t i = 0; i < kBlock; ++i) {
        const double t = double(seq * kBlock + i) / kFs;
        samples[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * freq * t));
    }
    sa::PacketHeader h;
    h.sequence = seq;
    h.sampleRate = kFs;
    h.channel = channel;
    return sa::serializePacket(h, samples);
}

// Waits for a replay to finish by itself; false on timeout.
bool waitUntilStopped(const sa::Pipeline& pipeline, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (pipeline.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    return !pipeline.running();
}

}  // namespace

TEST(Capture, RoundTripsRecords) {
    TempFile file("roundtrip");
    const std::vector<std::vector<std::byte>> datagrams{bytes({1, 2, 3}), {}, bytes({255, 0, 7, 9})};
    {
        sa::CaptureWriter writer(file.path());
        for (std::size_t i = 0; i < datagrams.size(); ++i) {
            ASSERT_TRUE(writer.write(1000 * i, datagrams[i]));
        }
        EXPECT_EQ(writer.records(), 3u);
    }

    sa::CaptureReader reader(file.path());
    sa::CaptureRecord rec;
    for (std::size_t i = 0; i < datagrams.size(); ++i) {
        ASSERT_TRUE(reader.next(rec));
        EXPECT_EQ(rec.timestampUs, 1000 * i);
        EXPECT_EQ(rec.datagram, datagrams[i]);
    }
    EXPECT_FALSE(reader.next(rec));
    EXPECT_DOUBLE_EQ(reader.progress(), 1.0);
}

TEST(Capture, RejectsForeignFilesAndStopsAtTruncatedRecord) {
    TempFile foreign("foreign");
    std::ofstream(foreign.path(), std::ios::binary) << "definitely not a capture";
    EXPECT_THROW(sa::CaptureReader{foreign.path()}, std::runtime_error);
    EXPECT_THROW(sa::CaptureReader{foreign.path().string() + ".missing"}, std::runtime_error);

    TempFile truncated("truncated");
    {
        sa::CaptureWriter writer(truncated.path());
        writer.write(1, bytes({1, 2, 3, 4}));
        writer.write(2, bytes({5, 6, 7, 8}));
    }
    std::filesystem::resize_file(truncated.path(), std::filesystem::file_size(truncated.path()) - 2);

    sa::CaptureReader reader(truncated.path());
    sa::CaptureRecord rec;
    EXPECT_TRUE(reader.next(rec));
    EXPECT_FALSE(reader.next(rec));  // the cut-off record is dropped, not misread
}

// Replay without pacing is deterministic: no network, and the receive thread waits
// for room in the ring instead of dropping samples.
TEST(Pipeline, ReplaysCaptureExactly) {
    TempFile file("replay");
    {
        sa::CaptureWriter writer(file.path());
        for (std::uint32_t seq = 0; seq < 40; ++seq) {
            const std::uint64_t t = seq * 32'000;  // 256 samples at 8 kHz = 32 ms
            writer.write(t, sinePacket(0, seq, kFreq, 1.0));
            if (seq != 10) {
                writer.write(t, sinePacket(3, seq, 2.0 * kFreq, 0.5));  // one lost on channel 3
            }
        }
        writer.write(40 * 32'000, bytes({0xde, 0xad}));  // malformed
    }

    sa::PipelineConfig cfg;
    cfg.fftSize = 1024;
    cfg.hopSize = 256;
    cfg.ringCapacity = 1024;  // small on purpose: replay must wait, not drop
    cfg.replayFile = file.str();
    cfg.replaySpeed = 0.0;
    sa::Pipeline pipeline(cfg);
    pipeline.start();
    ASSERT_TRUE(waitUntilStopped(pipeline, 10s));
    pipeline.stop();

    const auto stats = pipeline.stats();
    EXPECT_TRUE(pipeline.lastError().empty());
    EXPECT_EQ(stats.packetsMalformed, 1u);
    EXPECT_EQ(stats.samplesDropped, 0u);
    ASSERT_EQ(stats.channels.size(), 2u);
    EXPECT_EQ(stats.channels[0].packetsReceived, 40u);
    EXPECT_EQ(stats.channels[1].channel, 3u);
    EXPECT_EQ(stats.channels[1].packetsLost, 1u);
    // One frame per hop: (samples - fftSize) / hop + 1.
    EXPECT_EQ(stats.channels[0].framesComputed, (40u * kBlock - 1024u) / 256u + 1u);
    EXPECT_EQ(stats.channels[1].framesComputed, (39u * kBlock - 1024u) / 256u + 1u);
    EXPECT_DOUBLE_EQ(pipeline.replayProgress(), 1.0);

    const auto ch3 = pipeline.latestFrame(3);
    ASSERT_TRUE(ch3);
    EXPECT_NEAR(ch3->dominantHz, 2.0 * kFreq, 1.0);
    EXPECT_NEAR(ch3->rms, 0.5 / std::sqrt(2.0), 0.02);
}

TEST(Pipeline, ReplayFollowsRecordedTiming) {
    TempFile file("timing");
    {
        sa::CaptureWriter writer(file.path());
        writer.write(5'000'000, sinePacket(0, 0, kFreq, 1.0));  // offsets are relative to the first
        writer.write(5'300'000, sinePacket(0, 1, kFreq, 1.0));
    }
    sa::PipelineConfig cfg;
    cfg.replayFile = file.str();
    cfg.replaySpeed = 1.0;
    sa::Pipeline pipeline(cfg);

    const auto start = std::chrono::steady_clock::now();
    pipeline.start();
    ASSERT_TRUE(waitUntilStopped(pipeline, 5s));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 280ms);
    EXPECT_LT(elapsed, 3s);
}

TEST(Pipeline, RecordsEverythingItReceivesAndReplaysIt) {
    TempFile file("record");
    std::uint64_t recorded = 0;
    {
        sa::PipelineConfig cfg;
        cfg.port = 0;
        cfg.fftSize = 1024;
        cfg.hopSize = 256;
        sa::Pipeline live(cfg);
        live.start();
        live.startRecording(file.str());

        sa::UdpSender sender("127.0.0.1", live.port());
        for (std::uint32_t seq = 0; seq < 12; ++seq) {
            sender.send(sinePacket(1, seq, kFreq, 1.0));
            std::this_thread::sleep_for(1ms);
        }
        sender.send(bytes({1, 2, 3}));  // malformed, still recorded
        for (int i = 0; i < 200 && live.recordingStatus().packets < 13; ++i) {
            std::this_thread::sleep_for(10ms);
        }
        const auto status = live.recordingStatus();
        EXPECT_TRUE(status.active);
        EXPECT_TRUE(status.error.empty());
        live.stopRecording();
        const auto finalStatus = live.recordingStatus();
        EXPECT_FALSE(finalStatus.active);
        EXPECT_GE(finalStatus.packets, status.packets);  // totals survive the stop
        recorded = finalStatus.packets;
    }
    ASSERT_EQ(recorded, 13u);

    sa::PipelineConfig cfg;
    cfg.fftSize = 1024;
    cfg.hopSize = 256;
    cfg.replayFile = file.str();
    cfg.replaySpeed = 0.0;
    sa::Pipeline replay(cfg);
    replay.start();
    ASSERT_TRUE(waitUntilStopped(replay, 10s));
    const auto stats = replay.stats();
    EXPECT_EQ(stats.packetsMalformed, 1u);
    ASSERT_EQ(stats.channels.size(), 1u);
    EXPECT_EQ(stats.channels[0].channel, 1u);
    EXPECT_EQ(stats.channels[0].packetsReceived, 12u);
    EXPECT_EQ(stats.channels[0].framesComputed, (12u * kBlock - 1024u) / 256u + 1u);
}

TEST(Pipeline, RejectsMissingReplayFile) {
    sa::PipelineConfig cfg;
    cfg.replayFile = "does/not/exist.sacap";
    sa::Pipeline pipeline(cfg);
    EXPECT_THROW(pipeline.start(), std::runtime_error);
}

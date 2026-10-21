#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sa {

static_assert(std::endian::native == std::endian::little,
              "Wire format is little-endian; big-endian hosts are not supported yet.");

// Wire format of one UDP datagram (all fields little-endian):
//
//   offset  size  field
//   0       4     magic        0x31504153 ("SAP1")
//   4       4     sequence     incremented by 1 per packet
//   8       4     sampleRate   [Hz]
//   12      2     sampleCount  number of float32 samples that follow
//   14      2     channel      reserved for multi-channel, 0 for now
//   16      4*N   samples      IEEE-754 float32
inline constexpr std::uint32_t kMagic = 0x31504153;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kMaxSamplesPerPacket = 4096;

struct PacketHeader {
    std::uint32_t magic = kMagic;
    std::uint32_t sequence = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t sampleCount = 0;
    std::uint16_t channel = 0;
};

// Parses a datagram. Reuses `samples` capacity so the hot path does not allocate.
// Returns false for anything malformed (bad magic, wrong size, zero sample rate).
bool parsePacket(std::span<const std::byte> data, PacketHeader& header,
                 std::vector<float>& samples);

std::vector<std::byte> serializePacket(const PacketHeader& header,
                                       std::span<const float> samples);

}  // namespace sa

#pragma once

#include <cstdint>
#include <cstring>

// SAP1 packet, identical to core/include/sa/Protocol.hpp (little-endian, like the
// ESP32 and the PC):
//
//   0  uint32 magic 0x31504153 ("SAP1")
//   4  uint32 sequence (per channel)
//   8  uint32 sample rate [Hz]
//  12  uint16 sample count N
//  14  uint16 channel
//  16  float32 samples[N]
namespace sap1 {

constexpr uint32_t kMagic = 0x31504153;
constexpr size_t kHeaderSize = 16;

constexpr size_t packetSize(uint16_t count) { return kHeaderSize + 4u * count; }

// Writes one packet into `out` (at least packetSize(count) bytes); returns its size.
inline size_t build(uint8_t* out, uint32_t sequence, uint32_t sampleRate, uint16_t channel,
                    const float* samples, uint16_t count) {
    std::memcpy(out + 0, &kMagic, 4);
    std::memcpy(out + 4, &sequence, 4);
    std::memcpy(out + 8, &sampleRate, 4);
    std::memcpy(out + 12, &count, 2);
    std::memcpy(out + 14, &channel, 2);
    std::memcpy(out + kHeaderSize, samples, 4u * count);
    return packetSize(count);
}

}  // namespace sap1

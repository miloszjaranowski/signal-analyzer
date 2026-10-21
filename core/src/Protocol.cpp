#include "sa/Protocol.hpp"

#include <cstring>

namespace sa {

namespace {

template <typename T>
T readAt(std::span<const std::byte> data, std::size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void writeAt(std::vector<std::byte>& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

}  // namespace

bool parsePacket(std::span<const std::byte> data, PacketHeader& header,
                 std::vector<float>& samples) {
    if (data.size() < kHeaderSize) {
        return false;
    }

    header.magic = readAt<std::uint32_t>(data, 0);
    header.sequence = readAt<std::uint32_t>(data, 4);
    header.sampleRate = readAt<std::uint32_t>(data, 8);
    header.sampleCount = readAt<std::uint16_t>(data, 12);
    header.channel = readAt<std::uint16_t>(data, 14);

    if (header.magic != kMagic || header.sampleRate == 0 ||
        header.sampleCount > kMaxSamplesPerPacket) {
        return false;
    }

    const std::size_t payload = static_cast<std::size_t>(header.sampleCount) * sizeof(float);
    if (data.size() != kHeaderSize + payload) {
        return false;
    }

    samples.resize(header.sampleCount);
    std::memcpy(samples.data(), data.data() + kHeaderSize, payload);
    return true;
}

std::vector<std::byte> serializePacket(const PacketHeader& header,
                                       std::span<const float> samples) {
    std::vector<std::byte> out(kHeaderSize + samples.size() * sizeof(float));
    writeAt(out, 0, header.magic);
    writeAt(out, 4, header.sequence);
    writeAt(out, 8, header.sampleRate);
    writeAt(out, 12, static_cast<std::uint16_t>(samples.size()));
    writeAt(out, 14, header.channel);
    if (!samples.empty()) {
        std::memcpy(out.data() + kHeaderSize, samples.data(), samples.size() * sizeof(float));
    }
    return out;
}

}  // namespace sa

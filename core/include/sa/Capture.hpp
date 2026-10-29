#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace sa {

// Capture file (.sacap): every received datagram, byte for byte, with its arrival
// time. Lossless and independent of the channel layout: replaying a capture feeds
// the pipeline exactly the packets it saw, lost and malformed ones included.
// All integers little-endian (see Protocol.hpp):
//
//   file header   8 bytes   magic "SACAP001"
//   record        8 bytes   arrival time [us] since the recording started
//                 4 bytes   datagram length L
//                 L bytes   datagram (normally an SAP1 packet)
//
// The datagram format is documented separately, so the file stays readable even
// if a datagram is not valid SAP1. tools/sacap.py converts captures to CSV / HDF5.
inline constexpr std::array<char, 8> kCaptureMagic{'S', 'A', 'C', 'A', 'P', '0', '0', '1'};
inline constexpr std::uint32_t kMaxCaptureDatagram = 65536;

class CaptureWriter {
public:
    // Throws std::runtime_error if the file cannot be created.
    explicit CaptureWriter(const std::filesystem::path& path);

    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    // Buffered: normally just a copy into memory. Returns false once a write has
    // failed (e.g. disk full); the file then ends at the last complete record.
    bool write(std::uint64_t timestampUs, std::span<const std::byte> datagram);
    void flush();

    std::uint64_t records() const { return records_; }
    std::uint64_t bytes() const { return bytes_; }

private:
    std::vector<char> buffer_;  // stream buffer, declared before the stream that uses it
    std::ofstream out_;
    std::uint64_t records_ = 0;
    std::uint64_t bytes_ = 0;
};

struct CaptureRecord {
    std::uint64_t timestampUs = 0;
    std::vector<std::byte> datagram;
};

class CaptureReader {
public:
    // Throws std::runtime_error if the file is missing or not a capture.
    explicit CaptureReader(const std::filesystem::path& path);

    // Reads the next record into `record`, reusing its buffer. Returns false at the
    // end of the file; a truncated last record (e.g. after a crash) also ends it.
    bool next(CaptureRecord& record);

    double progress() const;  // 0..1, by bytes read

private:
    std::ifstream in_;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
};

}  // namespace sa

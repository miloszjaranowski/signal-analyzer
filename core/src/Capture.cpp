#include "sa/Capture.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "sa/Protocol.hpp"  // little-endian host assertion

namespace sa {

namespace {

constexpr std::size_t kRecordHeader = 8 + 4;

}  // namespace

CaptureWriter::CaptureWriter(const std::filesystem::path& path) : buffer_(1u << 20) {
    // A large stream buffer turns a record write into a memcpy most of the time,
    // so recording does not slow the receive thread down.
    out_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        throw std::runtime_error("Cannot create capture file " + path.string());
    }
    out_.write(kCaptureMagic.data(), static_cast<std::streamsize>(kCaptureMagic.size()));
    bytes_ = kCaptureMagic.size();
}

bool CaptureWriter::write(std::uint64_t timestampUs, std::span<const std::byte> datagram) {
    if (!out_ || datagram.size() > kMaxCaptureDatagram) {
        return false;
    }
    char header[kRecordHeader];
    const auto length = static_cast<std::uint32_t>(datagram.size());
    std::memcpy(header, &timestampUs, 8);
    std::memcpy(header + 8, &length, 4);
    out_.write(header, sizeof header);
    out_.write(reinterpret_cast<const char*>(datagram.data()), static_cast<std::streamsize>(length));
    if (!out_) {
        return false;
    }
    ++records_;
    bytes_ += kRecordHeader + length;
    return true;
}

void CaptureWriter::flush() { out_.flush(); }

CaptureReader::CaptureReader(const std::filesystem::path& path)
    : in_(path, std::ios::binary) {
    if (!in_) {
        throw std::runtime_error("Cannot open capture file " + path.string());
    }
    std::error_code ec;
    size_ = std::filesystem::file_size(path, ec);
    std::array<char, kCaptureMagic.size()> magic{};
    in_.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in_ || magic != kCaptureMagic) {
        throw std::runtime_error(path.string() + " is not a capture file (.sacap)");
    }
    position_ = magic.size();
}

bool CaptureReader::next(CaptureRecord& record) {
    char header[kRecordHeader];
    if (!in_.read(header, sizeof header)) {
        return false;
    }
    std::uint32_t length = 0;
    std::memcpy(&record.timestampUs, header, 8);
    std::memcpy(&length, header + 8, 4);
    if (length > kMaxCaptureDatagram) {
        return false;  // corrupt length: stop rather than allocate garbage
    }
    record.datagram.resize(length);
    if (!in_.read(reinterpret_cast<char*>(record.datagram.data()), static_cast<std::streamsize>(length))) {
        return false;
    }
    position_ += kRecordHeader + length;
    return true;
}

double CaptureReader::progress() const {
    return size_ > 0 ? std::min(1.0, static_cast<double>(position_) / static_cast<double>(size_)) : 1.0;
}

}  // namespace sa

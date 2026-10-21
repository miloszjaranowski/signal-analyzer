#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sa {

// Thin RAII wrappers over BSD sockets / Winsock. Platform headers stay in the .cpp.

class UdpReceiver {
public:
    // Binds to 0.0.0.0:port. Port 0 picks a free ephemeral port (handy in tests).
    // Throws std::runtime_error on failure.
    explicit UdpReceiver(std::uint16_t port);
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Waits up to `timeout` for one datagram. Returns its size, or 0 on timeout.
    // The timeout lets the owning thread notice a stop request.
    std::size_t receive(std::span<std::byte> buffer, std::chrono::milliseconds timeout);

    std::uint16_t port() const { return port_; }

private:
    std::uintptr_t socket_;
    std::uint16_t port_ = 0;
};

class UdpSender {
public:
    UdpSender(const std::string& host, std::uint16_t port);
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    void send(std::span<const std::byte> datagram);

private:
    std::uintptr_t socket_;
};

}  // namespace sa

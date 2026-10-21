#include "sa/Udp.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sa {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
const NativeSocket kInvalidSocket = INVALID_SOCKET;

// Winsock is reference counted by the OS, so pairing Startup/Cleanup per
// socket object is safe and keeps initialisation local.
void platformInit() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}
void platformCleanup() { WSACleanup(); }
void closeNative(NativeSocket s) { closesocket(s); }
std::string lastErrorText() { return "winsock error " + std::to_string(WSAGetLastError()); }
#else
using NativeSocket = int;
const NativeSocket kInvalidSocket = -1;

void platformInit() {}
void platformCleanup() {}
void closeNative(NativeSocket s) { ::close(s); }
std::string lastErrorText() { return std::strerror(errno); }
#endif

NativeSocket native(std::uintptr_t s) { return static_cast<NativeSocket>(s); }
std::uintptr_t handle(NativeSocket s) { return static_cast<std::uintptr_t>(s); }

NativeSocket openUdpSocket() {
    platformInit();
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) {
        const std::string err = lastErrorText();
        platformCleanup();
        throw std::runtime_error("socket() failed: " + err);
    }
    return s;
}

[[noreturn]] void failAndClose(NativeSocket s, const std::string& what) {
    const std::string err = lastErrorText();
    closeNative(s);
    platformCleanup();
    throw std::runtime_error(what + ": " + err);
}

}  // namespace

UdpReceiver::UdpReceiver(std::uint16_t port) {
    const NativeSocket s = openUdpSocket();

    // A larger kernel buffer absorbs bursts while the receive thread is descheduled.
    int rcvBuf = 4 * 1024 * 1024;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        failAndClose(s, "bind() to port " + std::to_string(port) + " failed");
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        failAndClose(s, "getsockname() failed");
    }
    port_ = ntohs(bound.sin_port);
    socket_ = handle(s);
}

UdpReceiver::~UdpReceiver() {
    closeNative(native(socket_));
    platformCleanup();
}

std::size_t UdpReceiver::receive(std::span<std::byte> buffer, std::chrono::milliseconds timeout) {
    const NativeSocket s = native(socket_);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(s, &readSet);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

    // First argument is ignored on Windows.
    const int ready = ::select(static_cast<int>(s) + 1, &readSet, nullptr, nullptr, &tv);
    if (ready == 0) {
        return 0;
    }
    if (ready < 0) {
#ifndef _WIN32
        if (errno == EINTR) {
            return 0;
        }
#endif
        throw std::runtime_error("select() failed: " + lastErrorText());
    }

#ifdef _WIN32
    const int n = ::recv(s, reinterpret_cast<char*>(buffer.data()),
                         static_cast<int>(buffer.size()), 0);
    if (n == SOCKET_ERROR) {
        // WSAEMSGSIZE: datagram larger than buffer -> treat as malformed by caller.
        if (WSAGetLastError() == WSAEMSGSIZE) {
            return buffer.size();
        }
        throw std::runtime_error("recv() failed: " + lastErrorText());
    }
#else
    const ssize_t n = ::recv(s, buffer.data(), buffer.size(), 0);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return 0;
        }
        throw std::runtime_error("recv() failed: " + lastErrorText());
    }
#endif
    return static_cast<std::size_t>(n);
}

UdpSender::UdpSender(const std::string& host, std::uint16_t port) {
    const NativeSocket s = openUdpSocket();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        closeNative(s);
        platformCleanup();
        throw std::runtime_error("Invalid IPv4 address: " + host);
    }
    // connect() on UDP just fixes the destination, so send() can be used.
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        failAndClose(s, "connect() failed");
    }
    socket_ = handle(s);
}

UdpSender::~UdpSender() {
    closeNative(native(socket_));
    platformCleanup();
}

void UdpSender::send(std::span<const std::byte> datagram) {
#ifdef _WIN32
    const int n = ::send(native(socket_), reinterpret_cast<const char*>(datagram.data()),
                         static_cast<int>(datagram.size()), 0);
#else
    const ssize_t n = ::send(native(socket_), datagram.data(), datagram.size(), 0);
#endif
    if (n < 0) {
        throw std::runtime_error("send() failed: " + lastErrorText());
    }
}

}  // namespace sa

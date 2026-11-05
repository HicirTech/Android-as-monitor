#include "stream_connection.h"

#include <ws2tcpip.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace aum {

// Small enough that a saturated USB link blocks send() quickly, which stalls the encoder and drops frames before
// encoding instead of letting stale frames queue up in front of the phone.
static constexpr int kSendBufferBytes = 512 * 1024;
static constexpr std::uint32_t kMaxPayloadBytes = 64 * 1024 * 1024;

StreamConnection::StreamConnection(SOCKET socket) : socket_(socket) {
    const BOOL noDelay = TRUE;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&kSendBufferBytes), sizeof(kSendBufferBytes));
}

StreamConnection::~StreamConnection() {
    Close();
}

void StreamConnection::Close() {
    const SOCKET socket = socket_;
    socket_ = INVALID_SOCKET;
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

bool StreamConnection::Send(PacketType type, std::uint8_t flags, std::uint64_t timestamp, const void* data, std::uint32_t size) {
    PacketHeader header{};
    header.type = static_cast<std::uint8_t>(type);
    header.flags = flags;
    header.length = size;
    header.timestamp = timestamp;

    WSABUF buffers[2];
    buffers[0].buf = reinterpret_cast<char*>(&header);
    buffers[0].len = sizeof(header);
    buffers[1].buf = const_cast<char*>(static_cast<const char*>(data));
    buffers[1].len = size;

    const std::lock_guard lock(sendMutex_);
    DWORD sent = 0;
    if (WSASend(socket_, buffers, size > 0 ? 2 : 1, &sent, 0, nullptr, nullptr) != 0) {
        return false;
    }
    return sent == sizeof(header) + size;
}

bool StreamConnection::ReadExactly(void* target, std::size_t size) {
    auto* cursor = static_cast<char*>(target);
    std::size_t remaining = size;
    while (remaining > 0) {
        const int received = recv(socket_, cursor, static_cast<int>(remaining), 0);
        if (received <= 0) {
            return false;
        }
        cursor += received;
        remaining -= static_cast<std::size_t>(received);
    }
    return true;
}

bool StreamConnection::Read(Packet& packet) {
    PacketHeader header{};
    if (!ReadExactly(&header, sizeof(header)) || header.length > kMaxPayloadBytes) {
        return false;
    }
    packet.type = static_cast<PacketType>(header.type);
    packet.flags = header.flags;
    packet.timestamp = header.timestamp;
    packet.payload.resize(header.length);
    return header.length == 0 || ReadExactly(packet.payload.data(), header.length);
}

StreamListener::StreamListener(std::uint16_t port) {
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        throw std::runtime_error("socket failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(socket_, 1) != 0) {
        throw std::runtime_error("cannot listen on 127.0.0.1:" + std::to_string(port) + ", is another companion running?");
    }
}

StreamListener::~StreamListener() {
    closesocket(socket_);
}

SOCKET StreamListener::Accept(int timeoutMs) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket_, &readable);
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) {
        return INVALID_SOCKET;
    }
    return accept(socket_, nullptr, nullptr);
}

}  // namespace aum

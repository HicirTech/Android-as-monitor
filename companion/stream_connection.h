#pragma once

#include "../shared/stream_protocol.h"

#include <winsock2.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace aum {

struct Packet {
    PacketType type{};
    std::uint8_t flags = 0;
    std::uint64_t timestamp = 0;
    std::vector<std::uint8_t> payload;
};

class StreamConnection {
public:
    explicit StreamConnection(SOCKET socket);
    ~StreamConnection();

    StreamConnection(const StreamConnection&) = delete;
    StreamConnection& operator=(const StreamConnection&) = delete;

    // Thread safe; returns false once the link is gone.
    bool Send(PacketType type, std::uint8_t flags, std::uint64_t timestamp, const void* data, std::uint32_t size);
    bool Read(Packet& packet);
    void Close();

private:
    bool ReadExactly(void* target, std::size_t size);

    SOCKET socket_;
    std::mutex sendMutex_;
};

class StreamListener {
public:
    explicit StreamListener(std::uint16_t port);
    ~StreamListener();

    StreamListener(const StreamListener&) = delete;
    StreamListener& operator=(const StreamListener&) = delete;

    SOCKET Accept(int timeoutMs);

private:
    SOCKET socket_;
};

}  // namespace aum

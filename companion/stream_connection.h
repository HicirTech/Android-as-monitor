#pragma once

#include "../shared/stream_protocol.h"

#include <winsock2.h>

#include <cstdint>
#include <mutex>
#include <string>
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

// The PC's end of a Wi-Fi link, where the phone listens. Retries because the phone listens only a moment after its app starts.
// Returns INVALID_SOCKET once timeoutMs has passed.
SOCKET ConnectStream(const std::string& ipv4Address, std::uint16_t port, int timeoutMs);

}  // namespace aum

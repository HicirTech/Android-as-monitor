#include "../stream_connection.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace aum;

static int failures = 0;

static void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static constexpr const char* kLoopback = "127.0.0.1";
static constexpr int kPatientMs = 20000;
static constexpr int kImpatientMs = 100;
// Longer than Windows keeps retrying a refused connect by itself, so that only ConnectStream's own retries can succeed.
static constexpr DWORD kPhoneListensAfterMs = 3000;

// A fixed number could clash with a companion that is streaming.
static std::uint16_t FreeLoopbackPort() {
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    int length = sizeof(address);
    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    closesocket(probe);
    return ntohs(address.sin_port);
}

static void ConnectsOnceThePhoneListens() {
    const std::uint16_t port = FreeLoopbackPort();
    SOCKET accepted = INVALID_SOCKET;
    std::thread phone([&] {
        Sleep(kPhoneListensAfterMs);
        StreamListener listener(port);
        accepted = listener.Accept(kPatientMs);
    });
    const SOCKET connected = ConnectStream(kLoopback, port, kPatientMs);
    phone.join();
    Check(connected != INVALID_SOCKET, "connects once the phone listens");
    Check(accepted != INVALID_SOCKET, "and the phone sees the connection");
    if (connected == INVALID_SOCKET || accepted == INVALID_SOCKET) {
        return;
    }

    StreamConnection pc(connected);
    StreamConnection phoneEnd(accepted);
    constexpr std::uint64_t kTimestamp = 42;
    Check(pc.Send(PacketType::Ping, 0, kTimestamp, nullptr, 0), "the stream can send on it");
    Packet packet;
    Check(phoneEnd.Read(packet) && packet.type == PacketType::Ping && packet.timestamp == kTimestamp, "and the packet arrives whole");
}

static void GivesUpWhenNothingListens() {
    Check(ConnectStream(kLoopback, FreeLoopbackPort(), kImpatientMs) == INVALID_SOCKET, "gives up once the time has passed");
}

static void RefusesWhatIsNoAddress() {
    bool refused = false;
    try {
        ConnectStream("tablet", FreeLoopbackPort(), kImpatientMs);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Check(refused, "says so at once when given no IPv4 address");
}

int main() {
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::puts("WSAStartup failed");
        return 1;
    }
    ConnectsOnceThePhoneListens();
    GivesUpWhenNothingListens();
    RefusesWhatIsNoAddress();
    std::printf(failures == 0 ? "stream connection tests passed\n" : "%d stream connection checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}

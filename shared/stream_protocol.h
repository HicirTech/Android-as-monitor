#pragma once

#include <cstdint>

namespace aum {

inline constexpr std::uint16_t kStreamPort = 27183;

// The phone's end of the adb tunnel is a Unix socket, not TCP. adbd leaves Nagle's algorithm on for a socket it accepts from
// `adb reverse tcp:`, so with sound and small frames on one link, frames wait for delayed ACKs and arrive in 40 ms bursts.
inline constexpr const char* kStreamSocketName = "android_usb_monitor";

// One connection carries picture, sound and control together, the way HDMI carries audio inside the video signal,
// so both share one link and one clock. Mirrored in android/.../Wire.kt.
enum class PacketType : std::uint8_t {
    Hello = 1,            // phone -> PC: key=value panel description
    Start = 2,            // PC -> phone: key=value stream parameters
    Ready = 3,            // phone -> PC: decoder and audio track are running
    VideoFrame = 4,       // PC -> phone: timestamp = frame id, payload = Annex B access unit
    AudioPcm = 5,         // PC -> phone: timestamp = PC nanoseconds, payload = interleaved float32
    Ping = 6,             // PC -> phone: timestamp = PC nanoseconds
    Pong = 7,             // phone -> PC: timestamp = echoed PC nanoseconds, payload = phone nanoseconds (int64)
    FrameStats = 8,       // phone -> PC: timestamp = frame id, payload = FrameStatsPayload
    KeyframeRequest = 9,  // phone -> PC
};

inline constexpr std::uint8_t kFlagKeyframe = 0x01;

#pragma pack(push, 1)
struct PacketHeader {
    std::uint8_t type;
    std::uint8_t flags;
    std::uint16_t reserved;
    std::uint32_t length;
    std::uint64_t timestamp;
};

struct FrameStatsPayload {
    std::int64_t receivedNs;
    std::int64_t queuedNs;
    std::int64_t decodedNs;
    std::int64_t presentNs;  // The vsync the phone gave this frame, or 0 when it skipped the frame to keep latency down.
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 16);
static_assert(sizeof(FrameStatsPayload) == 32);

}  // namespace aum

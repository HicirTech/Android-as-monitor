#pragma once

#include <cstdint>
#include <cstdio>

namespace aum {

inline constexpr const wchar_t* kControlPipeName = L"\\\\.\\pipe\\AndroidUsbMonitor";
inline constexpr const wchar_t* kFrameHeaderName = L"Global\\AndroidUsbMonitor.FrameHeader";
inline constexpr const wchar_t* kFrameReadyEventName = L"Global\\AndroidUsbMonitor.FrameReady";

// The driver host runs as LocalService in session 0, so everything it shares must grant the logged-on user access.
inline constexpr const wchar_t* kSharedObjectSddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GA;;;IU)";

inline constexpr std::uint32_t kFrameSlotCount = 3;
inline constexpr std::uint32_t kMaxEdidBytes = 512;
inline constexpr std::uint32_t kFrameTapMagic = 0x4D554146;
inline constexpr std::uint32_t kFrameTapAbiVersion = 1;

enum class ControlCommand : std::uint32_t {
    Plug = 1,
    Unplug = 2,
};

struct ControlRequest {
    std::uint32_t sequence;
    ControlCommand command;
    GUID containerId;
    LUID preferredRenderAdapter;
    std::uint32_t edidSize;
    std::uint8_t edid[kMaxEdidBytes];
};

struct ControlResponse {
    std::uint32_t sequence;
    std::int32_t status;
    LUID osAdapterLuid;
    std::uint32_t osTargetId;
};

struct FrameSlotInfo {
    std::uint64_t sequence;
    std::uint64_t acquireQpc;
    std::uint64_t presentQpc;
    std::uint32_t presentationFrame;
    std::uint32_t reserved;
};

struct FrameTapHeader {
    std::uint32_t magic;
    std::uint32_t abiVersion;
    // Zero while the textures are being replaced; readers reopen the textures whenever it changes.
    volatile LONG generation;
    LUID renderAdapter;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t dxgiFormat;
    volatile LONG latestSlot;
    volatile LONG64 published;
    FrameSlotInfo slots[kFrameSlotCount];
};

inline void FormatFrameTextureName(wchar_t (&name)[80], LONG generation, std::uint32_t slot) {
    // The generation keeps names unique: a reader may still hold the previous swap chain's textures open.
    swprintf_s(name, L"Global\\AndroidUsbMonitor.Frame.%ld.%u", generation, slot);
}

}  // namespace aum

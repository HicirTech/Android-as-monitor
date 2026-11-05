#pragma once

#include "panel_spec.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aum {

struct ActiveDisplay {
    LUID adapter;
    std::uint32_t targetId;
    std::wstring gdiName;
    std::wstring friendlyName;
    std::wstring devicePath;
    RECT desktopRect;
    std::uint32_t vSyncNumerator;
    std::uint32_t vSyncDenominator;
};

struct DisplayMode {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t frequencyHz;

    bool operator==(const DisplayMode&) const = default;
};

std::vector<ActiveDisplay> ListActiveDisplays();
std::vector<DisplayMode> ListDisplayModes(const std::wstring& gdiName);
std::vector<std::uint8_t> ReadEdidFromRegistry(const std::wstring& devicePath);
std::uint32_t RawDpiOf(const ActiveDisplay& display);

// The monitor exists exactly as long as this object: the driver unplugs it when the pipe closes,
// so even a crash of this process leaves no blank screen behind.
class VirtualMonitor {
public:
    VirtualMonitor(const std::vector<std::uint8_t>& edid, const GUID& containerId, LUID preferredRenderAdapter);
    ~VirtualMonitor();

    VirtualMonitor(const VirtualMonitor&) = delete;
    VirtualMonitor& operator=(const VirtualMonitor&) = delete;

    std::optional<ActiveDisplay> WaitUntilActive(int timeoutMs) const;

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    LUID adapter_{};
    std::uint32_t targetId_ = 0;
    std::uint32_t nextSequence_ = 1;
};

}  // namespace aum

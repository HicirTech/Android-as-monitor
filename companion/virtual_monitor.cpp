#include "virtual_monitor.h"

#include "clock.h"
#include "win_error.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <stdexcept>
#include <string>

#include "../shared/driver_protocol.h"

namespace aum {

static constexpr int kPipeConnectTimeoutMs = 5000;
static constexpr DWORD kPipeBusyWaitMs = 500;
static constexpr int kExtendDesktopAfterMs = 2000;

std::vector<ActiveDisplay> ListActiveDisplays() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    while (result == ERROR_INSUFFICIENT_BUFFER) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            throw std::runtime_error("GetDisplayConfigBufferSizes failed");
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        paths.resize(pathCount);
        modes.resize(modeCount);
    }
    if (result != ERROR_SUCCESS) {
        throw std::runtime_error("QueryDisplayConfig failed");
    }

    std::vector<ActiveDisplay> displays;
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        ActiveDisplay display{};
        display.adapter = path.targetInfo.adapterId;
        display.targetId = path.targetInfo.id;
        display.vSyncNumerator = path.targetInfo.refreshRate.Numerator;
        display.vSyncDenominator = path.targetInfo.refreshRate.Denominator;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
            display.gdiName = sourceName.viewGdiDeviceName;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS) {
            display.friendlyName = targetName.monitorFriendlyDeviceName;
            display.devicePath = targetName.monitorDevicePath;
        }

        const UINT32 sourceMode = path.sourceInfo.modeInfoIdx;
        if (sourceMode != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && sourceMode < modes.size() && modes[sourceMode].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const DISPLAYCONFIG_SOURCE_MODE& source = modes[sourceMode].sourceMode;
            display.desktopRect = {source.position.x, source.position.y, source.position.x + static_cast<LONG>(source.width), source.position.y + static_cast<LONG>(source.height)};
        }
        displays.push_back(display);
    }
    return displays;
}

std::vector<DisplayMode> ListDisplayModes(const std::wstring& gdiName) {
    std::vector<DisplayMode> modes;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(gdiName.c_str(), index, &mode); ++index) {
        const DisplayMode candidate{mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency};
        if (std::find(modes.begin(), modes.end(), candidate) == modes.end()) {
            modes.push_back(candidate);
        }
    }
    return modes;
}

std::vector<std::uint8_t> ReadEdidFromRegistry(const std::wstring& devicePath) {
    // \\?\DISPLAY#AUM1F2B#7&2b3c&0&UID4352#{guid} maps to Enum\DISPLAY\AUM1F2B\7&2b3c&0&UID4352.
    const std::size_t hardwareStart = devicePath.find(L'#');
    const std::size_t instanceStart = hardwareStart == std::wstring::npos ? std::wstring::npos : devicePath.find(L'#', hardwareStart + 1);
    const std::size_t instanceEnd = instanceStart == std::wstring::npos ? std::wstring::npos : devicePath.find(L'#', instanceStart + 1);
    if (instanceEnd == std::wstring::npos) {
        return {};
    }
    const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + devicePath.substr(hardwareStart + 1, instanceStart - hardwareStart - 1) + L"\\" +
                             devicePath.substr(instanceStart + 1, instanceEnd - instanceStart - 1) + L"\\Device Parameters";
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::vector<std::uint8_t> edid(size);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, edid.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    edid.resize(size);
    return edid;
}

std::uint32_t RawDpiOf(const ActiveDisplay& display) {
    const POINT center{(display.desktopRect.left + display.desktopRect.right) / 2, (display.desktopRect.top + display.desktopRect.bottom) / 2};
    const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
    UINT dpiX = 0;
    UINT dpiY = 0;
    if (!monitor || FAILED(GetDpiForMonitor(monitor, MDT_RAW_DPI, &dpiX, &dpiY))) {
        return 0;
    }
    return dpiX;
}

VirtualMonitor::VirtualMonitor(const std::vector<std::uint8_t>& edid, const GUID& containerId, LUID preferredRenderAdapter) {
    if (edid.size() > kMaxEdidBytes) {
        throw std::runtime_error("EDID is too large for the driver protocol");
    }

    const std::int64_t deadline = NowNs() + std::int64_t{kPipeConnectTimeoutMs} * 1'000'000;
    for (;;) {
        pipe_ = CreateFileW(kControlPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        if (NowNs() >= deadline) {
            if (error == ERROR_FILE_NOT_FOUND) {
                throw std::runtime_error("the display driver is not running: install it with tools\\install-driver.ps1, or use --source pattern");
            }
            throw std::runtime_error("cannot open the driver control pipe, error " + std::to_string(error));
        }
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(kControlPipeName, kPipeBusyWaitMs);
        } else {
            Sleep(100);
        }
    }
    DWORD readMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe_, &readMode, nullptr, nullptr)) {
        throw std::runtime_error("cannot switch the control pipe to message mode");
    }

    ControlRequest request{};
    request.sequence = nextSequence_++;
    request.command = ControlCommand::Plug;
    request.containerId = containerId;
    request.preferredRenderAdapter = preferredRenderAdapter;
    request.edidSize = static_cast<std::uint32_t>(edid.size());
    std::copy(edid.begin(), edid.end(), request.edid);

    ControlResponse response{};
    DWORD received = 0;
    if (!TransactNamedPipe(pipe_, &request, sizeof(request), &response, sizeof(response), &received, nullptr) || received != sizeof(response)) {
        throw std::runtime_error("the driver did not answer the plug request, error " + std::to_string(GetLastError()));
    }
    if (response.sequence != request.sequence || response.status != 0) {
        char status[32];
        sprintf_s(status, "0x%08lX", static_cast<unsigned long>(response.status));
        throw std::runtime_error(std::string("the driver refused the monitor: ") + status);
    }
    adapter_ = response.osAdapterLuid;
    targetId_ = response.osTargetId;
}

VirtualMonitor::~VirtualMonitor() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
    }
}

std::optional<ActiveDisplay> VirtualMonitor::WaitUntilActive(int timeoutMs) const {
    const std::int64_t started = NowNs();
    bool askedForExtend = false;
    while ((NowNs() - started) / 1'000'000 < timeoutMs) {
        for (const ActiveDisplay& display : ListActiveDisplays()) {
            if (display.adapter.LowPart == adapter_.LowPart && display.adapter.HighPart == adapter_.HighPart && display.targetId == targetId_) {
                return display;
            }
        }
        // Windows normally extends onto a new monitor by itself; "PC screen only" leaves it dark, as Win+P would fix.
        if (!askedForExtend && (NowNs() - started) / 1'000'000 > kExtendDesktopAfterMs) {
            askedForExtend = true;
            SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        }
        Sleep(100);
    }
    return std::nullopt;
}

}  // namespace aum

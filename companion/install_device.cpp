#include "commands.h"
#include "win_error.h"

#include <swdevice.h>

#include <cstdio>
#include <stdexcept>

namespace aum {

struct CreationResult {
    HANDLE done;
    HRESULT status;
};

static VOID WINAPI OnCreated(HSWDEVICE, HRESULT status, PVOID context, PCWSTR instanceId) {
    auto* result = static_cast<CreationResult*>(context);
    result->status = status;
    std::printf("device %s: 0x%08lX\n", instanceId ? Narrow(instanceId).c_str() : "(none)", static_cast<unsigned long>(status));
    SetEvent(result->done);
}

// Needs elevation. The device outlives this process and comes back after a reboot; it stays an idle display adapter
// with no monitor until the companion plugs one in.
int RunInstallDevice() {
    static constexpr DWORD kInstallTimeoutMs = 60000;

    SW_DEVICE_CREATE_INFO info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = L"AndroidUsbMonitor";
    info.pszzHardwareIds = L"AndroidUsbMonitor\0\0";
    info.pszzCompatibleIds = L"AndroidUsbMonitor\0\0";
    info.pszDeviceDescription = L"Android USB Monitor Adapter";
    info.CapabilityFlags = SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;

    CreationResult result{CreateEventW(nullptr, TRUE, FALSE, nullptr), E_PENDING};
    HSWDEVICE device = nullptr;
    ThrowIfFailed(SwDeviceCreate(L"AndroidUsbMonitor", L"HTREE\\ROOT\\0", &info, 0, nullptr, OnCreated, &result, &device), "SwDeviceCreate");
    if (WaitForSingleObject(result.done, kInstallTimeoutMs) != WAIT_OBJECT_0) {
        throw std::runtime_error("the device did not finish installing in time");
    }
    ThrowIfFailed(result.status, "device creation");
    ThrowIfFailed(SwDeviceSetLifetime(device, SWDeviceLifetimeParentPresent), "SwDeviceSetLifetime");
    SwDeviceClose(device);
    CloseHandle(result.done);
    return 0;
}

}  // namespace aum

#include <initguid.h>

#include "commands.h"
#include "win_error.h"

#include <setupapi.h>
#include <devguid.h>

namespace aum {

// Needs elevation. A root-enumerated device stays across reboots, where a software device would not come back; it is an idle
// display adapter with no monitor until the companion plugs one in. Installing the driver package is what binds a driver to it.
int RunInstallDevice() {
    static constexpr wchar_t kDeviceName[] = L"AndroidUsbMonitor";
    // A multi-string: the literal's own terminator is the second of the two it must end with.
    static constexpr wchar_t kHardwareIds[] = L"AndroidUsbMonitor\0";

    const HDEVINFO devices = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (devices == INVALID_HANDLE_VALUE) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "SetupDiCreateDeviceInfoList");
    }
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    const BOOL created =
        SetupDiCreateDeviceInfoW(devices, kDeviceName, &GUID_DEVCLASS_DISPLAY, L"Android USB Monitor Adapter", nullptr, DICD_GENERATE_ID, &device) &&
        SetupDiSetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(kHardwareIds), sizeof(kHardwareIds)) &&
        SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices, &device);
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(devices);
    if (!created) {
        ThrowIfFailed(HRESULT_FROM_WIN32(error), "creating the root device");
    }
    return 0;
}

}  // namespace aum

#include "audio_capture.h"
#include "commands.h"
#include "virtual_monitor.h"
#include "win_error.h"

#include "../shared/edid_timing.h"

#include <cstdio>

namespace aum {

// Shows what Windows believes about every active display, using the same queries the native mode check relies on.
int RunInspectDisplays() {
    for (const ActiveDisplay& display : ListActiveDisplays()) {
        std::printf("%s  \"%s\"  %ldx%ld at %u/%u Hz, raw DPI %u\n", Narrow(display.gdiName).c_str(), Narrow(display.friendlyName).c_str(),
                    display.desktopRect.right - display.desktopRect.left, display.desktopRect.bottom - display.desktopRect.top, display.vSyncNumerator,
                    display.vSyncDenominator, RawDpiOf(display));

        const auto modes = ListDisplayModes(display.gdiName);
        std::printf("  %zu distinct modes\n", modes.size());

        const auto edid = ReadEdidFromRegistry(display.devicePath);
        const auto timing = ParseNativeTiming(edid.data(), edid.size());
        if (timing) {
            const Rational refresh = RefreshRate(*timing);
            std::printf("  EDID %zu bytes: %ux%u at %.3f Hz, %ux%u mm\n", edid.size(), timing->hActive, timing->vActive, refresh.value(), timing->widthMm, timing->heightMm);
        } else {
            std::printf("  EDID %zu bytes: no usable detailed timing\n", edid.size());
        }
    }
    std::puts("playback endpoints:");
    for (const std::wstring& name : ListRenderEndpoints()) {
        std::printf("  %s\n", Narrow(name).c_str());
    }
    return 0;
}

}  // namespace aum

#include "commands.h"
#include "win_error.h"

#include <winsock2.h>
#include <mfapi.h>
#include <objbase.h>

#include <cstdio>
#include <exception>
#include <string>

static void PrintUsage() {
    std::puts(
        "aum-companion run            [--source virtual|dda|pattern] [--seconds N] [--codec auto|hevc|h264] [--bitrate-mbps N]\n"
        "                             [--audio-endpoint NAME|tone|off] [--render-adapter NAME] [--pattern bands|scroll] [--decoder NAME] [--gpu-hold N]\n"
        "                             [--serial S] [--adb PATH] [--csv PATH]\n"
        "aum-companion encoder-bench  [--adapter NAME] [--codec hevc|h264] [--width W] [--height H] [--fps F] [--seconds N]\n"
        "                             [--bitrate-mbps N] [--pattern bands|scroll] [--dump FILE] [--keyframe-at-second N] [--gpu-hold N] [--dda-output \\\\.\\DISPLAYn]\n"
        "aum-companion inspect-displays\n"
        "aum-companion install-device (elevated; tools\\install-driver.ps1 calls this)");
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }
    const std::wstring command = argv[1];
    try {
        aum::ThrowIfFailed(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        aum::ThrowIfFailed(MFStartup(MF_VERSION), "MFStartup");
        WSADATA winsock;
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        const aum::Options options(argc, argv, 2);
        if (command == L"run") {
            return aum::RunMonitor(options);
        }
        if (command == L"encoder-bench") {
            return aum::RunEncoderBench(options);
        }
        if (command == L"inspect-displays") {
            return aum::RunInspectDisplays();
        }
        if (command == L"install-device") {
            return aum::RunInstallDevice();
        }
        PrintUsage();
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}

#include "adb.h"
#include "audio_capture.h"
#include "clock.h"
#include "commands.h"
#include "dda_source.h"
#include "edid.h"
#include "gpu.h"
#include "gpu_clock_holder.h"
#include "latency_stats.h"
#include "motion_window.h"
#include "panel_rate_source.h"
#include "panel_spec.h"
#include "pattern_source.h"
#include "phone_cadence.h"
#include "phone_layer.h"
#include "stream_connection.h"
#include "tap_source.h"
#include "tone_source.h"
#include "video_pipeline.h"
#include "virtual_monitor.h"
#include "win_error.h"

#include "../shared/stream_protocol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>

namespace aum {

namespace {

constexpr const char* kAppPackage = "dev.androidusbmonitor";
constexpr int kPhoneConnectTimeoutMs = 20000;
constexpr int kMonitorActiveTimeoutMs = 10000;
constexpr int kSwapChainTimeoutMs = 5000;
constexpr int kAudioEndpointSettleMs = 3000;
constexpr int kPingIntervalMs = 200;
constexpr int kPhoneLayerCheckAfterSeconds = 3;
constexpr DWORD kLayerSearchIntervalMs = 200;
constexpr DWORD kScreenSettleSeconds = 3;
constexpr double kRefreshTolerance = 0.01;
constexpr double kDpiTolerance = 0.03;
constexpr double kRequiredFpsRatio = 0.99;
// Composing an indirect display at the panel's own rate, Windows itself loses frames: on one PC about 80 fps for 2 s out of every
// 12.5 s, with no encoding or link involved. At twice that rate, one frame per panel period always reaches the panel.
constexpr int kDefaultComposeMultiple = 2;

HANDLE stopEvent = nullptr;

BOOL WINAPI OnConsoleControl(DWORD) {
    SetEvent(stopEvent);
    return TRUE;
}

void PrintCheck(bool passed, const std::string& what) {
    std::printf("  %s %s\n", passed ? "PASS" : "FAIL", what.c_str());
}

// Requirement 4: Windows must see exactly one mode, the panel's resolution and size at the rate it composes the monitor.
bool VerifyNativeMode(const ActiveDisplay& display, const PanelSpec& spec, const std::vector<std::uint8_t>& sentEdid) {
    std::puts("native mode check (what Windows now reports):");
    bool allPassed = true;
    const auto check = [&](bool passed, const std::string& what) {
        PrintCheck(passed, what);
        allPassed = allPassed && passed;
    };

    const auto modes = ListDisplayModes(display.gdiName);
    std::string modeList;
    for (const DisplayMode& mode : modes) {
        modeList += std::to_string(mode.width) + "x" + std::to_string(mode.height) + "@" + std::to_string(mode.frequencyHz) + " ";
    }
    const std::uint32_t wholeHz = static_cast<std::uint32_t>(std::lround(spec.refreshHz));
    const bool onlyNative = modes.size() == 1 && modes[0].width == spec.width && modes[0].height == spec.height &&
                            std::abs(static_cast<int>(modes[0].frequencyHz) - static_cast<int>(wholeHz)) <= 1;
    check(onlyNative, "only one mode is offered: " + modeList);

    const double windowsHz = display.vSyncDenominator ? static_cast<double>(display.vSyncNumerator) / display.vSyncDenominator : 0;
    char refreshText[128];
    sprintf_s(refreshText, "refresh rate: composing at %.5f Hz, Windows %u/%u = %.5f Hz", spec.refreshHz, display.vSyncNumerator, display.vSyncDenominator, windowsHz);
    check(std::abs(windowsHz - spec.refreshHz) < kRefreshTolerance, refreshText);

    const auto storedEdid = ReadEdidFromRegistry(display.devicePath);
    check(storedEdid == sentEdid, "EDID stored by Windows is byte-identical (" + std::to_string(storedEdid.size()) + " bytes)");

    const std::uint32_t rawDpi = RawDpiOf(display);
    const double expectedDpi = spec.width / (spec.widthMm / 25.4);
    char dpiText[160];
    sprintf_s(dpiText, "physical size: phone %.1f x %.1f mm = %.0f DPI, Windows raw DPI %u", spec.widthMm, spec.heightMm, expectedDpi, rawDpi);
    if (rawDpi == 0) {
        // The EDID carries the size and Windows stores it, but for some panels it does not use it: it assumes 96 DPI instead.
        std::printf("  WARN %s: Windows ignores the size of this panel\n", dpiText);
    } else {
        check(std::abs(rawDpi - expectedDpi) / expectedDpi < kDpiTolerance, dpiText);
    }

    PrintCheck(Narrow(display.friendlyName) == spec.model.substr(0, 13), "Windows calls it \"" + Narrow(display.friendlyName) + "\"");
    return allPassed;
}

void ReportEndpointChange(const std::vector<std::wstring>& before) {
    Sleep(kAudioEndpointSettleMs);
    const auto after = ListRenderEndpoints();
    bool appeared = false;
    for (const std::wstring& name : after) {
        if (std::find(before.begin(), before.end(), name) == before.end()) {
            std::printf("  new playback endpoint with the monitor: %s\n", Narrow(name).c_str());
            appeared = true;
        }
    }
    if (!appeared) {
        std::puts("  no playback endpoint appeared with the monitor, even though its EDID declares stereo LPCM speakers");
    }
}

// What decides 1:1 on the panel. A screenshot cannot: it recomposes through the GPU, which resamples cropped YUV buffers.
void ReportPhoneLayer(const Adb& adb) {
    const auto layer = FindVideoLayer(adb.RunOrEmpty("shell dumpsys SurfaceFlinger"));
    if (!layer) {
        std::puts("  phone compositor: video layer not found in dumpsys");
        return;
    }
    char text[200];
    sprintf_s(text, "phone compositor maps pixels 1:1: %s %s, source %.0fx%.0f, display frame %.0fx%.0f", layer->composition.c_str(), layer->transform.c_str(), layer->sourceWidth,
              layer->sourceHeight, layer->displayWidth, layer->displayHeight);
    PrintCheck(layer->MapsPixelsOneToOne(), text);
}

// The only real measure of what reached the panel. It costs the phone a dumpsys now and then, so only test runs do it.
void SamplePresentTimes(const Adb& adb, PhoneCadence& cadence) {
    // The app's launch and the screen rotating to landscape have nothing to do with how the stream runs.
    if (WaitForSingleObject(stopEvent, kScreenSettleSeconds * 1000) != WAIT_TIMEOUT) {
        return;
    }
    std::string layer;
    unsigned long intervalMs = 0;
    while (intervalMs == 0) {
        if (WaitForSingleObject(stopEvent, kLayerSearchIntervalMs) != WAIT_TIMEOUT) {
            return;
        }
        if (layer.empty()) {
            layer = FindVideoLayerName(adb.RunOrEmpty("shell dumpsys SurfaceFlinger --list"));
            continue;
        }
        cadence.AddLatencyDump(adb.RunOrEmpty("shell \"dumpsys SurfaceFlinger --latency '" + layer + "'\""));
        intervalMs = cadence.ReadIntervalMs();
    }
    // One shell for the whole run. Starting a shell makes adbd fork, which stalls the picture on the same link every time.
    char sleepSeconds[32];
    sprintf_s(sleepSeconds, "%.3f", static_cast<double>(intervalMs) / 1000.0);
    adb.Stream("shell \"while true; do echo " + std::string(kLatencyDumpMark) + "; dumpsys SurfaceFlinger --latency '" + layer + "'; sleep " + sleepSeconds + "; done\"", stopEvent,
               [&](std::string_view piece) { cadence.AddLoopOutput(piece); });
}

void ReportOnScreen(const PhoneCadence& cadence, std::vector<double> captureToScreen, double reportedHz, Rational refresh) {
    const auto screen = cadence.Summarize();
    if (!screen) {
        std::puts("  frame rate on screen: not measured, SurfaceFlinger gave no present times");
        return;
    }
    std::printf("on screen: %.1f fps over %zu frames, %.1f%% of vsyncs showed no new frame, longest gap %.1f ms, %.1f%% of frames missed their vsync\n", screen->framesPerSecond,
                screen->framesOnScreen, screen->repeatedVsyncShare * 100, screen->longestGapMs, screen->lateShare * 100);
    if (!captureToScreen.empty()) {
        std::sort(captureToScreen.begin(), captureToScreen.end());
        std::printf("capture to on screen: p50 %.1f ms, p95 %.1f ms\n", captureToScreen[captureToScreen.size() / 2], captureToScreen[captureToScreen.size() * 95 / 100]);
    }
    std::printf("panel: runs at %.3f Hz, Android reports %.5f Hz\n", screen->panelHz, reportedHz);
    PrintCheck(screen->framesPerSecond >= refresh.value() * kRequiredFpsRatio, "frame rate on screen: " + std::to_string(screen->framesPerSecond) + " of " + std::to_string(refresh.value()) + " fps");
}

VideoCodec ChooseCodec(const Options& options, const PanelSpec& spec) {
    const std::wstring requested = options.Text(L"codec", L"auto");
    if (requested == L"hevc") {
        return VideoCodec::Hevc;
    }
    if (requested == L"h264") {
        return VideoCodec::H264;
    }
    if (!spec.hevcCoversNative && spec.avcCoversNative) {
        return VideoCodec::H264;
    }
    if (!spec.hevcCoversNative) {
        std::puts("warning: the phone reports no decoder rated for the native mode; trying HEVC anyway");
    }
    return VideoCodec::Hevc;
}

std::string ReadHello(StreamConnection& connection) {
    Packet packet;
    if (!connection.Read(packet) || packet.type != PacketType::Hello) {
        throw std::runtime_error("the phone did not send its panel description");
    }
    return {packet.payload.begin(), packet.payload.end()};
}

void WaitForReady(StreamConnection& connection) {
    Packet packet;
    if (!connection.Read(packet) || packet.type != PacketType::Ready) {
        throw std::runtime_error("the phone app stopped before its decoder was running: keep the screen on and unlocked; adb logcat -s AndroidUsbMonitor says more");
    }
}

LUID PreferredRenderAdapter(const Options& options) {
    const std::wstring name = options.Text(L"render-adapter");
    if (name.empty()) {
        // The frames stay on the GPU that renders them, so this choice also decides the encoder.
        return PreferredEncodingAdapter();
    }
    const auto found = FindAdapter(name);
    if (!found) {
        throw std::runtime_error("no GPU matching " + Narrow(name));
    }
    return *found;
}

}  // namespace

int RunMonitor(const Options& options) {
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);

    const Adb adb(Adb::Locate(options.Text(L"adb")), Narrow(options.Text(L"serial")));
    const std::string port = std::to_string(kStreamPort);
    const std::string wifiAddress = Narrow(options.Text(L"connect"));
    const bool overWifi = !wifiAddress.empty();
    // Over Wi-Fi these checks reach the phone through adb on the same radio, and they stalled the picture for seconds at a time.
    const bool phoneChecks = !overWifi || options.Text(L"phone-checks") == L"on";
    if (!phoneChecks) {
        std::puts("phone checks off, they stall the picture over Wi-Fi; --phone-checks on runs them");
    }

    // A copy left running from an earlier session would otherwise connect first and then be killed by the restart.
    adb.Run(std::string("shell am force-stop ") + kAppPackage);
    std::optional<StreamListener> listener;
    if (!overWifi) {
        listener.emplace(kStreamPort);
        adb.Run("reverse localabstract:" + std::string(kStreamSocketName) + " tcp:" + port);
    }
    adb.Run(std::string("shell am start -n ") + kAppPackage + "/.MainActivity" + (overWifi ? " --ez wifi true" : ""));

    std::puts("waiting for the phone app...");
    const SOCKET socket = overWifi ? ConnectStream(wifiAddress, kStreamPort, kPhoneConnectTimeoutMs) : listener->Accept(kPhoneConnectTimeoutMs);
    if (socket == INVALID_SOCKET) {
        throw std::runtime_error(overWifi ? "cannot reach the phone app at " + wifiAddress + "; is the address right and the screen unlocked?"
                                          : "the phone app did not connect; is it installed and the screen unlocked?");
    }
    StreamConnection connection(socket);

    const std::string hello = ReadHello(connection);
    const PanelSpec spec = ParsePanelSpec(hello);
    std::printf("phone: %s %s, panel %ux%u at %.5f Hz, %.1f x %.1f mm\n", spec.manufacturer.c_str(), spec.model.c_str(), spec.width, spec.height, spec.refreshHz, spec.widthMm, spec.heightMm);
    std::printf("decoders: hevc %s (native mode %s), avc %s (native mode %s)\n", spec.hevcDecoder.c_str(), spec.hevcCoversNative ? "rated" : "not rated",
                spec.avcDecoder.c_str(), spec.avcCoversNative ? "rated" : "not rated");
    std::printf("reported: xdpi %s, ydpi %s, densityDpi %s, modes %s\n", spec.raw.count("xdpi") ? spec.raw.at("xdpi").c_str() : "?", spec.raw.count("ydpi") ? spec.raw.at("ydpi").c_str() : "?",
                spec.raw.count("densityDpi") ? spec.raw.at("densityDpi").c_str() : "?", spec.raw.count("supportedModes") ? spec.raw.at("supportedModes").c_str() : "?");
    const std::string usbSpeed = overWifi ? std::string{} : adb.RunOrEmpty("shell cat /sys/class/udc/*/current_speed");
    if (!usbSpeed.empty()) {
        std::printf("USB link: %s", usbSpeed.c_str());
    }
    if (spec.surfaceWidth != spec.width || spec.surfaceHeight != spec.height) {
        throw std::runtime_error("the app surface is " + std::to_string(spec.surfaceWidth) + "x" + std::to_string(spec.surfaceHeight) +
                                 ", not the native mode: pixels would not map 1:1");
    }

    const int composeMultiple = static_cast<int>(options.Number(L"compose-multiple", kDefaultComposeMultiple));
    if (composeMultiple < 1) {
        throw std::runtime_error("--compose-multiple must be at least 1");
    }
    PanelSpec composedSpec = spec;
    composedSpec.refreshHz = spec.refreshHz * composeMultiple;
    const std::vector<std::uint8_t> edid = BuildEdid(composedSpec);
    const Rational refresh = SnapRefreshRate(spec.refreshHz);

    const std::wstring sourceName = options.Text(L"source", L"virtual");
    const std::wstring audioEndpoint = options.Text(L"audio-endpoint");
    const auto endpointsBefore = ListRenderEndpoints();

    std::unique_ptr<VirtualMonitor> monitor;
    std::optional<ActiveDisplay> display;
    std::optional<LUID> captureAdapter;
    if (sourceName == L"pattern") {
        captureAdapter = PreferredRenderAdapter(options);
    } else {
        monitor = std::make_unique<VirtualMonitor>(edid, ContainerIdFor(spec.deviceId), PreferredRenderAdapter(options));
        display = monitor->WaitUntilActive(kMonitorActiveTimeoutMs);
        if (!display) {
            throw std::runtime_error("Windows did not activate the monitor; press Win+P and choose Extend");
        }
        VerifyNativeMode(*display, composedSpec, edid);
        ReportEndpointChange(endpointsBefore);
        if (sourceName != L"dda") {
            captureAdapter = TapSource::WaitForRenderAdapter(kSwapChainTimeoutMs);
        }
        if (!captureAdapter) {
            const auto location = DdaSource::Locate(display->gdiName);
            if (!location) {
                throw std::runtime_error("neither the driver frame tap nor Desktop Duplication can reach the virtual monitor: read the AndroidUsbMonitor events in the Application log");
            }
            captureAdapter = location->adapter;
        }
    }
    std::printf("capture GPU: %s\n", Narrow(DescribeAdapter(*captureAdapter)).c_str());

    GpuDevice gpu = CreateGpuDevice(*captureAdapter);
    const GpuClockHolder clockHolder(*captureAdapter, static_cast<int>(options.Number(L"gpu-hold", DefaultClockHoldCopies(*captureAdapter))));
    std::unique_ptr<FrameSource> source;
    if (sourceName == L"pattern") {
        const PatternKind pattern = options.Text(L"pattern", L"bands") == L"scroll" ? PatternKind::ScrollingDetail : PatternKind::PixelBands;
        source = std::make_unique<PatternSource>(gpu, spec.width, spec.height, refresh, pattern);
    } else if (sourceName != L"dda") {
        // Sharing textures from the session 0 driver host is what can fail on an unseen machine; a picture beats a stop.
        try {
            source = std::make_unique<TapSource>(gpu, spec.width, spec.height);
            std::puts("frame source: driver swap chain tap");
        } catch (const std::runtime_error& error) {
            std::printf("warning: driver frame tap unavailable (%s); using Desktop Duplication, which adds latency and shows no pointer\n", error.what());
        }
    }
    if (!source) {
        source = std::make_unique<DdaSource>(gpu, display->gdiName);
        std::puts("frame source: Desktop Duplication");
    }
    if (monitor && composeMultiple > 1) {
        source = std::make_unique<PanelRateSource>(std::move(source), refresh);
        std::printf("Windows composes at %.3f Hz, the phone gets one frame per panel period\n", composedSpec.refreshHz);
    }

    FILE* csv = nullptr;
    const std::wstring csvPath = options.Text(L"csv");
    if (!csvPath.empty() && _wfopen_s(&csv, csvPath.c_str(), L"w") != 0) {
        csv = nullptr;
    }
    LatencyStats stats(csv);

    EncoderSettings settings;
    settings.codec = ChooseCodec(options, spec);
    settings.width = spec.width;
    settings.height = spec.height;
    settings.refresh = refresh;
    settings.bitrateBps = static_cast<std::uint32_t>(options.Number(L"bitrate-mbps", 100) * 1'000'000);

    std::atomic<bool> linkLost{false};
    const auto onLinkLost = [&] {
        linkLost = true;
        SetEvent(stopEvent);
    };

    VideoPipeline pipeline(gpu, *source, settings, stats, [&](const EncodedFrame& frame) {
        const bool sent = connection.Send(PacketType::VideoFrame, frame.isKeyframe ? kFlagKeyframe : 0, frame.frameId, frame.data, frame.size);
        if (!sent) {
            onLinkLost();
        }
        return sent;
    });
    std::printf("encoder: %s, %s, %u Mbps\n", Narrow(pipeline.EncoderName()).c_str(), MimeType(settings.codec), settings.bitrateBps / 1'000'000);

    const auto sendAudio = [&](const float* samples, std::uint32_t frames) {
        stats.OnAudio(samples, frames);
        if (!connection.Send(PacketType::AudioPcm, 0, static_cast<std::uint64_t>(NowNs()), samples, frames * 2 * sizeof(float))) {
            onLinkLost();
        }
    };
    std::unique_ptr<AudioCapture> audioCapture;
    std::unique_ptr<ToneSource> tone;
    std::uint32_t audioSampleRate = 0;
    if (audioEndpoint == L"tone") {
        tone = std::make_unique<ToneSource>(sendAudio);
        audioSampleRate = ToneSource::kSampleRate;
        std::puts("audio source: 440 Hz test tone");
    } else if (audioEndpoint != L"off") {
        audioCapture = std::make_unique<AudioCapture>(audioEndpoint, sendAudio);
        audioSampleRate = audioCapture->SampleRate();
        std::printf("audio source: %s, %u Hz stereo\n", Narrow(audioCapture->EndpointName()).c_str(), audioSampleRate);
    }

    std::string start = "codec=" + std::string(MimeType(settings.codec)) + "\nwidth=" + std::to_string(spec.width) + "\nheight=" + std::to_string(spec.height) +
                        "\nrefreshHz=" + std::to_string(refresh.value()) + "\n";
    if (audioSampleRate > 0) {
        start += "sampleRate=" + std::to_string(audioSampleRate) + "\nchannels=2\n";
    }
    const std::wstring decoder = options.Text(L"decoder");
    if (!decoder.empty()) {
        start += "decoder=" + Narrow(decoder) + "\n";
    }
    if (!connection.Send(PacketType::Start, 0, 0, start.data(), static_cast<std::uint32_t>(start.size()))) {
        throw std::runtime_error("lost the phone while starting");
    }
    WaitForReady(connection);

    std::thread reader([&] {
        Packet packet;
        while (connection.Read(packet)) {
            const std::int64_t receivedNs = NowNs();
            if (packet.type == PacketType::Pong && packet.payload.size() == sizeof(std::int64_t)) {
                stats.OnPong(static_cast<std::int64_t>(packet.timestamp), *reinterpret_cast<const std::int64_t*>(packet.payload.data()), receivedNs);
            } else if (packet.type == PacketType::FrameStats && packet.payload.size() == sizeof(FrameStatsPayload)) {
                stats.OnPhoneStats(packet.timestamp, *reinterpret_cast<const FrameStatsPayload*>(packet.payload.data()));
            } else if (packet.type == PacketType::KeyframeRequest) {
                pipeline.RequestKeyframe();
            }
        }
        onLinkLost();
    });

    std::thread pinger([&] {
        while (WaitForSingleObject(stopEvent, kPingIntervalMs) == WAIT_TIMEOUT) {
            connection.Send(PacketType::Ping, 0, static_cast<std::uint64_t>(NowNs()), nullptr, 0);
        }
    });

    const double seconds = options.Number(L"seconds", 0);
    std::thread reporter([&] {
        const std::int64_t startedNs = NowNs();
        int elapsedSeconds = 0;
        while (WaitForSingleObject(stopEvent, 1000) == WAIT_TIMEOUT) {
            stats.PrintSecond(refresh.value());
            if (++elapsedSeconds == kPhoneLayerCheckAfterSeconds && phoneChecks) {
                ReportPhoneLayer(adb);
            }
            if (seconds > 0 && static_cast<double>(NowNs() - startedNs) / 1e9 >= seconds) {
                SetEvent(stopEvent);
            }
        }
    });

    PhoneCadence cadence;
    std::thread presentSampler;
    if (seconds > 0 && phoneChecks) {
        stats.KeepCaptureTimes();
        presentSampler = std::thread([&] { SamplePresentTimes(adb, cadence); });
    }

    // Fixed-length runs are the frame rate test, so they need a desktop that changes every frame.
    std::unique_ptr<MotionWindow> motion;
    if (seconds > 0 && display) {
        motion = std::make_unique<MotionWindow>(*captureAdapter, display->desktopRect);
    }
    if (audioCapture) {
        audioCapture->Start();
    }
    if (tone) {
        tone->Start();
    }

    pipeline.Run(stopEvent);

    motion.reset();
    audioCapture.reset();
    tone.reset();
    SetEvent(stopEvent);
    connection.Close();
    if (presentSampler.joinable()) {
        presentSampler.join();
    }
    reporter.join();
    pinger.join();
    reader.join();

    const RunSummary summary = stats.Summarize();
    std::printf("captured %.1f fps, encoded %.1f fps, decoded %.1f fps, %.1f Mbps, dropped before encode %llu, skipped by the phone %llu\n", summary.capturedFps, summary.encodedFps,
                summary.decodedFps, summary.megabitsPerSecond, summary.droppedBeforeEncode, summary.skippedByPhone);
    stats.PrintEndToEnd();
    if (seconds > 0 && phoneChecks) {
        ReportOnScreen(cadence, cadence.CaptureToScreenMs(stats.CapturedBySchedule()), spec.refreshHz, refresh);
    }
    if (csv) {
        std::fclose(csv);
    }
    if (linkLost && seconds <= 0) {
        std::puts("the phone disconnected");
    }
    return 0;
}

}  // namespace aum

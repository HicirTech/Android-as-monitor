#include "clock.h"
#include "commands.h"
#include "dda_source.h"
#include "edid.h"
#include "gpu.h"
#include "gpu_clock_holder.h"
#include "latency_stats.h"
#include "pattern_source.h"
#include "video_pipeline.h"
#include "win_error.h"

#include <cstdio>
#include <memory>
#include <thread>

namespace aum {

// Measures capture to encoded bitstream at the target load, with no phone or driver involved.
int RunEncoderBench(const Options& options) {
    const std::wstring adapterName = options.Text(L"adapter");
    const std::wstring ddaOutput = options.Text(L"dda-output");

    std::uint32_t width = static_cast<std::uint32_t>(options.Number(L"width", 2400));
    std::uint32_t height = static_cast<std::uint32_t>(options.Number(L"height", 1080));
    const Rational refresh = SnapRefreshRate(options.Number(L"fps", 120));
    const double seconds = options.Number(L"seconds", 5);

    LUID adapter{};
    if (!ddaOutput.empty()) {
        const auto location = DdaSource::Locate(ddaOutput);
        if (!location) {
            throw std::runtime_error("no display output named " + Narrow(ddaOutput));
        }
        adapter = location->adapter;
        width = location->width;
        height = location->height;
    } else if (!adapterName.empty()) {
        const auto found = FindAdapter(adapterName);
        if (!found) {
            throw std::runtime_error("no GPU matching " + Narrow(adapterName));
        }
        adapter = *found;
    } else {
        adapter = PreferredEncodingAdapter();
    }

    GpuDevice gpu = CreateGpuDevice(adapter);
    const GpuClockHolder clockHolder(adapter, static_cast<int>(options.Number(L"gpu-hold", DefaultClockHoldCopies(adapter))));
    std::unique_ptr<FrameSource> source;
    if (!ddaOutput.empty()) {
        source = std::make_unique<DdaSource>(gpu, ddaOutput);
    } else {
        const PatternKind pattern = options.Text(L"pattern", L"bands") == L"scroll" ? PatternKind::ScrollingDetail : PatternKind::PixelBands;
        source = std::make_unique<PatternSource>(gpu, width, height, refresh, pattern);
    }

    EncoderSettings settings;
    settings.codec = options.Text(L"codec", L"hevc") == L"h264" ? VideoCodec::H264 : VideoCodec::Hevc;
    settings.width = width;
    settings.height = height;
    settings.refresh = refresh;
    settings.bitrateBps = static_cast<std::uint32_t>(options.Number(L"bitrate-mbps", 100) * 1'000'000);

    FILE* dump = nullptr;
    const std::wstring dumpPath = options.Text(L"dump");
    if (!dumpPath.empty() && _wfopen_s(&dump, dumpPath.c_str(), L"wb") != 0) {
        throw std::runtime_error("cannot open the dump file");
    }

    LatencyStats stats(nullptr);
    VideoPipeline pipeline(gpu, *source, settings, stats, [&](const EncodedFrame& frame) {
        if (dump) {
            std::fwrite(frame.data, 1, frame.size, dump);
            if (frame.frameId < 3) {
                std::printf("frame %llu: %u bytes, keyframe flag %d, starts %02X %02X %02X %02X %02X %02X\n", frame.frameId, frame.size, frame.isKeyframe ? 1 : 0, frame.data[0],
                            frame.data[1], frame.data[2], frame.data[3], frame.data[4], frame.data[5]);
            }
        }
        return true;
    });

    std::printf("GPU %s, encoder %s, %ux%u at %.3f Hz, %s, %u Mbps\n", Narrow(DescribeAdapter(adapter)).c_str(), Narrow(pipeline.EncoderName()).c_str(),
                width, height, refresh.value(), MimeType(settings.codec), settings.bitrateBps / 1'000'000);

    const HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const int keyframeAtSecond = static_cast<int>(options.Number(L"keyframe-at-second", 0));
    std::thread timer([&] {
        const std::int64_t endNs = NowNs() + static_cast<std::int64_t>(seconds * 1e9);
        int second = 0;
        while (NowNs() < endNs) {
            Sleep(1000);
            stats.PrintSecond(refresh.value());
            if (++second == keyframeAtSecond) {
                pipeline.RequestKeyframe();
            }
        }
        SetEvent(stop);
    });
    pipeline.Run(stop);
    timer.join();

    const RunSummary summary = stats.Summarize();
    std::printf("captured %.1f fps, encoded %.1f fps, %.1f Mbps, dropped before encode %llu\n", summary.capturedFps, summary.encodedFps,
                summary.megabitsPerSecond, summary.droppedBeforeEncode);
    CloseHandle(stop);
    if (dump) {
        std::fclose(dump);
    }
    return 0;
}

}  // namespace aum

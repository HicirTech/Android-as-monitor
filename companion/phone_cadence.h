#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aum {

struct PhoneCadenceSummary {
    std::size_t framesOnScreen = 0;
    double framesPerSecond = 0;
    // From the present times themselves. No panel runs at exactly the rate its device reports.
    double panelHz = 0;
    // Vsyncs that showed no new frame.
    double repeatedVsyncShare = 0;
    double longestGapMs = 0;
    // Frames that reached the panel after the vsync they were scheduled for.
    double lateShare = 0;
};

// Printed before each dump when a shell loop keeps reading them.
inline constexpr std::string_view kLatencyDumpMark = "---latency---";

// What really reached the panel, from `dumpsys SurfaceFlinger --latency`. Nothing the app sees can tell: the decoder's render
// callback only echoes back the timestamp the frame was released with.
class PhoneCadence {
public:
    // Reads overlap, and SurfaceFlinger keeps only a short history, so a gap counts only between frames of one read.
    void AddLatencyDump(const std::string& dump);

    // Output of a shell loop, in pieces of any size, with kLatencyDumpMark before each dump.
    void AddLoopOutput(std::string_view piece);

    std::optional<PhoneCadenceSummary> Summarize() const;

    // Capture to panel, for every frame that reached it and has a capture time. The key is the vsync a frame was scheduled for
    // and the value when it was captured, both on the phone's clock. The dump's ready column cannot serve: what it holds differs
    // between devices, and on some it is the time the compositor took the buffer.
    std::vector<double> CaptureToScreenMs(const std::map<std::int64_t, std::int64_t>& capturedBySchedule) const;

    // How long until the next read still overlaps this one, at whatever rate this panel runs. 0 until a read has returned frames.
    unsigned long ReadIntervalMs() const;

private:
    struct Frame {
        std::int64_t desiredNs = 0;
        std::int64_t previousPresentNs = 0;
    };

    std::map<std::int64_t, Frame> frames_;
    std::int64_t reportedPeriodNs_ = 0;
    std::int64_t lastReadSpanNs_ = 0;
    std::string loopOutput_;
};

}  // namespace aum

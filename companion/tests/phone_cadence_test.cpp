#include "../phone_cadence.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace aum;

static int failures = 0;

static void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static constexpr std::int64_t kStart = 147415191337406;
static const std::string kPending = "9223372036854775807";

// `dumpsys SurfaceFlinger --latency`: the refresh period, then desired, present and ready times per frame.
static std::string Dump(std::int64_t reportedPeriod, const std::vector<std::int64_t>& presents, std::int64_t desiredLead) {
    std::string text = std::to_string(reportedPeriod) + "\n";
    for (const std::int64_t present : presents) {
        text += std::to_string(present - desiredLead) + "\t" + std::to_string(present) + "\t" + std::to_string(present - desiredLead) + "\n";
    }
    return text;
}

static std::vector<std::int64_t> OnVsyncs(std::int64_t period, const std::vector<int>& vsyncs) {
    std::vector<std::int64_t> presents;
    for (const int vsync : vsyncs) {
        presents.push_back(kStart + vsync * period);
    }
    return presents;
}

static void CountsWhatReachedThePanelAtAnyRate() {
    for (const std::int64_t period : {16666666LL, 11111111LL, 8333333LL, 6944444LL}) {
        PhoneCadence cadence;
        cadence.AddLatencyDump(Dump(period, OnVsyncs(period, {0, 1, 2, 4, 5}), period / 2));
        const auto summary = cadence.Summarize();
        Check(summary.has_value(), "a read with frames gives a summary");
        Check(summary->framesOnScreen == 5, "five frames reached the panel");
        Check(std::abs(summary->framesPerSecond - 4e9 / static_cast<double>(5 * period)) < 0.01, "four gaps over five vsyncs");
        Check(std::abs(summary->repeatedVsyncShare - 0.2) < 1e-9, "one vsync of five showed no new frame");
        Check(std::abs(summary->longestGapMs - static_cast<double>(2 * period) / 1e6) < 1e-3, "the longest gap is two vsyncs");
    }
}

static void OverlappingReadsCountOnce() {
    const std::int64_t period = 8333333;
    PhoneCadence once;
    once.AddLatencyDump(Dump(period, OnVsyncs(period, {0, 1, 2, 3, 4, 5}), period / 2));
    PhoneCadence twice;
    twice.AddLatencyDump(Dump(period, OnVsyncs(period, {0, 1, 2, 3, 4}), period / 2));
    twice.AddLatencyDump(Dump(period, OnVsyncs(period, {2, 3, 4, 5}), period / 2));
    Check(twice.Summarize()->framesOnScreen == once.Summarize()->framesOnScreen, "a frame seen in two reads is one frame");
    Check(std::abs(twice.Summarize()->framesPerSecond - once.Summarize()->framesPerSecond) < 1e-6, "and one gap");
}

static void AHoleBetweenReadsIsNotAGap() {
    const std::int64_t period = 8333333;
    PhoneCadence cadence;
    cadence.AddLatencyDump(Dump(period, OnVsyncs(period, {0, 1, 2}), period / 2));
    cadence.AddLatencyDump(Dump(period, OnVsyncs(period, {100, 101, 102}), period / 2));
    const auto summary = cadence.Summarize();
    Check(summary->repeatedVsyncShare == 0, "the vsyncs between two reads were not seen to repeat");
    Check(std::abs(summary->longestGapMs - static_cast<double>(period) / 1e6) < 1e-3, "and are no gap");
}

static void MeasuresThePanelRatherThanBelievingIt() {
    // Each panel runs a little off what its device reports, as the Fold 7 does at 120.047 Hz against 120.00001.
    for (const auto& [reported, real] : std::vector<std::pair<std::int64_t, std::int64_t>>{{8333333, 8329900}, {8333333, 8336000}, {16666666, 16683000}}) {
        PhoneCadence cadence;
        std::vector<int> vsyncs;
        for (int vsync = 0; vsync < 2000; ++vsync) {
            if (vsync % 37 != 0) {
                vsyncs.push_back(vsync);
            }
        }
        cadence.AddLatencyDump(Dump(reported, OnVsyncs(real, vsyncs), real / 2));
        const double expectedHz = 1e9 / static_cast<double>(real);
        Check(std::abs(cadence.Summarize()->panelHz - expectedHz) < expectedHz * 2e-6, "the fitted rate is the real one, not the reported one");
    }
}

static void RowsWithoutAPresentTimeAreIgnored() {
    const std::int64_t period = 8333333;
    PhoneCadence cadence;
    const std::string first = std::to_string(kStart);
    const std::string second = std::to_string(kStart + period);
    cadence.AddLatencyDump(std::to_string(period) + "\n0\t0\t0\n" + first + "\t" + first + "\t" + first + "\n" + second + "\t" + second + "\t" + second + "\n" + second + "\t" + kPending + "\t" + second + "\n");
    Check(cadence.Summarize()->framesOnScreen == 2, "rows with no fence and rows still pending are no frames");
}

static void ReplacedBuffersShareOneFrame() {
    const std::int64_t period = 8333333;
    const std::string present = std::to_string(kStart + period);
    const std::string previous = std::to_string(kStart);
    PhoneCadence cadence;
    // The first of the two buffers for this vsync was scheduled two vsyncs early: only the last one counts as what was shown.
    cadence.AddLatencyDump(std::to_string(period) + "\n" + previous + "\t" + previous + "\t0\n" + std::to_string(kStart + period - 2 * period) + "\t" + present + "\t0\n" + present + "\t" + present + "\t0\n");
    const auto summary = cadence.Summarize();
    Check(summary->framesOnScreen == 2, "one present time is one frame on the panel");
    Check(std::abs(summary->longestGapMs - static_cast<double>(period) / 1e6) < 1e-3, "and the gap to it is from the frame before");
    Check(summary->lateShare == 0, "judged by the buffer that was shown");
}

static void FramesPastTheirVsyncCountAsLate() {
    const std::int64_t period = 8333333;
    std::string text = std::to_string(period) + "\n";
    for (int vsync = 0; vsync < 4; ++vsync) {
        const std::int64_t present = kStart + vsync * period;
        // Released half a period before its vsync, so one vsync late shows as one and a half periods.
        const std::int64_t lead = vsync == 3 ? period + period / 2 : period / 2;
        text += std::to_string(present - lead) + "\t" + std::to_string(present) + "\t0\n";
    }
    PhoneCadence cadence;
    cadence.AddLatencyDump(text);
    Check(std::abs(cadence.Summarize()->lateShare - 0.25) < 1e-9, "one frame of four missed its vsync");
}

static void CaptureToScreenCountsFromTheRealPresent() {
    for (const std::int64_t period : {16666666LL, 8333333LL, 8268065LL}) {
        const std::int64_t half = period / 2;
        const std::int64_t captureLead = 25000000;
        std::map<std::int64_t, std::int64_t> capturedBySchedule;
        std::string text = std::to_string(period) + "\n";
        // Frame 0 on time. Frame 1 scheduled for vsync 1 but shown on vsync 2. Frame 2, for vsync 3, has no capture time.
        // Frame 3, for vsync 4, was replaced by frame 4 before it reached the panel, on vsync 5. Frame 5, the newest, has no
        // capture time either, and nothing after it to take one from.
        capturedBySchedule[kStart] = kStart - captureLead;
        text += std::to_string(kStart - half) + "\t" + std::to_string(kStart) + "\t0\n";
        capturedBySchedule[kStart + period] = kStart + period - captureLead;
        text += std::to_string(kStart + period - half) + "\t" + std::to_string(kStart + 2 * period) + "\t0\n";
        text += std::to_string(kStart + 3 * period - half) + "\t" + std::to_string(kStart + 3 * period) + "\t0\n";
        capturedBySchedule[kStart + 4 * period] = kStart + 4 * period - captureLead;
        text += std::to_string(kStart + 4 * period - half) + "\t" + std::to_string(kStart + 5 * period) + "\t0\n";
        capturedBySchedule[kStart + 5 * period] = kStart + 5 * period - captureLead;
        text += std::to_string(kStart + 5 * period - half) + "\t" + std::to_string(kStart + 5 * period) + "\t0\n";
        text += std::to_string(kStart + 6 * period - half) + "\t" + std::to_string(kStart + 6 * period) + "\t0\n";

        PhoneCadence cadence;
        cadence.AddLatencyDump(text);
        const std::vector<double> latencies = cadence.CaptureToScreenMs(capturedBySchedule);
        const double onTime = static_cast<double>(captureLead) / 1e6;
        Check(latencies.size() == 3, "only frames that reached the panel and have a capture time count");
        Check(std::abs(latencies[0] - onTime) < 1e-3, "a frame on its vsync took what was scheduled");
        Check(std::abs(latencies[1] - (onTime + static_cast<double>(period) / 1e6)) < 1e-3, "a late frame took a vsync longer");
        Check(std::abs(latencies[2] - onTime) < 1e-3, "the frame shown on a vsync is the last one given to it");
    }
}

static void CaptureToScreenToleratesThePeriodTheAppUsed() {
    // The app halves the period it was offered, here the real one, while the dump reports the nominal one.
    const std::int64_t reported = 8333333;
    const std::int64_t offered = 8268065;
    std::map<std::int64_t, std::int64_t> capturedBySchedule;
    std::string text = std::to_string(reported) + "\n";
    for (int vsync = 0; vsync < 5; ++vsync) {
        const std::int64_t scheduled = kStart + vsync * offered;
        capturedBySchedule[scheduled] = scheduled - 20000000;
        text += std::to_string(scheduled - offered / 2) + "\t" + std::to_string(scheduled) + "\t0\n";
    }
    PhoneCadence cadence;
    cadence.AddLatencyDump(text);
    const std::vector<double> latencies = cadence.CaptureToScreenMs(capturedBySchedule);
    Check(latencies.size() == 5, "every frame finds its own capture time");
    Check(!latencies.empty() && std::abs(latencies.back() - 20.0) < 1e-3, "and no neighbour's");
}

static void TheNextReadOverlapsThisOneAtAnyRate() {
    for (const std::int64_t period : {16666666LL, 8333333LL, 6944444LL}) {
        std::vector<int> vsyncs;
        for (int vsync = 0; vsync < 127; ++vsync) {
            vsyncs.push_back(vsync);
        }
        PhoneCadence cadence;
        Check(cadence.ReadIntervalMs() == 0, "no interval before the first read");
        cadence.AddLatencyDump(Dump(period, OnVsyncs(period, vsyncs), period / 2));
        const double historyMs = static_cast<double>(126 * period) / 1e6;
        Check(cadence.ReadIntervalMs() > 0 && cadence.ReadIntervalMs() < historyMs * 0.75, "reads come well inside the history this panel's rate gives");
    }
}

static void ALoopsOutputCountsLikeSeparateReadsHoweverItIsCut() {
    const std::int64_t period = 8333333;
    const std::string mark(kLatencyDumpMark);
    const std::string output = mark + "\n" + Dump(period, OnVsyncs(period, {0, 1, 2}), period / 2) + mark + "\n" + Dump(period, OnVsyncs(period, {2, 3, 5}), period / 2) + mark + "\n";

    PhoneCadence whole;
    whole.AddLoopOutput(output);
    Check(whole.Summarize().has_value() && whole.Summarize()->framesOnScreen == 5, "two complete dumps give their frames");
    Check(std::abs(whole.Summarize()->repeatedVsyncShare - 0.2) < 1e-9, "and their gaps: one vsync of five repeated");

    PhoneCadence cut;
    for (std::size_t at = 0; at < output.size(); at += 7) {
        cut.AddLoopOutput(std::string_view(output).substr(at, 7));
    }
    Check(cut.Summarize().has_value() && cut.Summarize()->framesOnScreen == 5, "a dump cut into pieces is the same dump");

    PhoneCadence unfinished;
    unfinished.AddLoopOutput(mark + "\n" + Dump(period, OnVsyncs(period, {0, 1, 2}), period / 2));
    Check(!unfinished.Summarize().has_value(), "a dump is only read once the next one has begun, because until then it may be incomplete");
}

static void NothingToReadGivesNoSummary() {
    PhoneCadence cadence;
    Check(!cadence.Summarize().has_value(), "no reads");
    cadence.AddLatencyDump("");
    cadence.AddLatencyDump("Permission Denial\n");
    Check(!cadence.Summarize().has_value(), "no usable reads");
}

int main() {
    CountsWhatReachedThePanelAtAnyRate();
    OverlappingReadsCountOnce();
    AHoleBetweenReadsIsNotAGap();
    MeasuresThePanelRatherThanBelievingIt();
    RowsWithoutAPresentTimeAreIgnored();
    ReplacedBuffersShareOneFrame();
    FramesPastTheirVsyncCountAsLate();
    CaptureToScreenCountsFromTheRealPresent();
    CaptureToScreenToleratesThePeriodTheAppUsed();
    TheNextReadOverlapsThisOneAtAnyRate();
    ALoopsOutputCountsLikeSeparateReadsHoweverItIsCut();
    NothingToReadGivesNoSummary();
    std::printf(failures == 0 ? "phone cadence tests passed\n" : "%d phone cadence checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}

#include "../panel_rate_picker.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace aum;

static int failures = 0;

static void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

// Rates a panel may really run at, including ones that are no round number.
static const std::int64_t kPanelPeriodsNs[] = {16'666'666, 11'111'111, 8'333'333, 8'330'000, 6'944'444, 4'166'666};
static constexpr std::int64_t kStartNs = 1'000'000'000'000;
static constexpr int kPeriods = 1000;

static std::vector<std::int64_t> PickedFrom(std::int64_t periodNs, const std::vector<std::int64_t>& capturesNs) {
    PanelRatePicker picker(periodNs);
    std::vector<std::int64_t> picked;
    for (const std::int64_t capturedNs : capturesNs) {
        if (picker.Pick(capturedNs)) {
            picked.push_back(capturedNs);
        }
    }
    return picked;
}

static void TwiceTheRateGivesOneFramePerPeriod() {
    for (const std::int64_t period : kPanelPeriodsNs) {
        std::vector<std::int64_t> captures;
        for (int tick = 0; tick < 2 * kPeriods; ++tick) {
            captures.push_back(kStartNs + tick * period / 2);
        }
        const auto picked = PickedFrom(period, captures);
        const std::string label = "period " + std::to_string(period);
        Check(picked.size() == kPeriods, label + " one frame per period, got " + std::to_string(picked.size()));
        bool evenlySpaced = true;
        for (std::size_t i = 1; i < picked.size(); ++i) {
            evenlySpaced = evenlySpaced && picked[i] - picked[i - 1] == period;
        }
        Check(evenlySpaced, label + " and they stay one period apart");
    }
}

static void ALostTickCostsNoFrame() {
    for (const std::int64_t period : kPanelPeriodsNs) {
        for (const int lostEvery : {3, 7, 11}) {
            std::vector<std::int64_t> captures;
            for (int tick = 0; tick < 2 * kPeriods; ++tick) {
                if (tick % lostEvery != 0) {
                    captures.push_back(kStartNs + tick * period / 2);
                }
            }
            const auto picked = PickedFrom(period, captures);
            const std::string label = "period " + std::to_string(period) + ", every " + std::to_string(lostEvery) + "th tick lost";
            Check(picked.size() >= kPeriods - 1 && picked.size() <= kPeriods, label + " still gives one frame per period, got " + std::to_string(picked.size()));
        }
    }
}

static void AJitteringSourceKeepsOneFramePerPeriod() {
    for (const std::int64_t period : kPanelPeriodsNs) {
        std::vector<std::int64_t> captures;
        const std::int64_t jitter = period / 20;
        for (int tick = 0; tick < 2 * kPeriods; ++tick) {
            // A fixed pattern, so that the test has no randomness in it.
            const std::int64_t offset = (tick * 7 % 5 - 2) * jitter / 2;
            captures.push_back(kStartNs + tick * period / 2 + offset);
        }
        const auto picked = PickedFrom(period, captures);
        Check(picked.size() >= kPeriods - 1 && picked.size() <= kPeriods + 1, "period " + std::to_string(period) + " with jitter, got " + std::to_string(picked.size()));
    }
}

static void ASourceAtThePanelRateLosesNothing() {
    for (const std::int64_t period : kPanelPeriodsNs) {
        std::vector<std::int64_t> captures;
        for (int frame = 0; frame < kPeriods; ++frame) {
            captures.push_back(kStartNs + frame * period);
        }
        Check(PickedFrom(period, captures).size() == captures.size(), "period " + std::to_string(period) + " every frame of a source at the panel's rate passes");
    }
}

static void TheFirstFrameIsSentAtOnce() {
    PanelRatePicker picker(8'333'333);
    Check(picker.Pick(kStartNs), "nothing holds back the first frame");
}

static void AnIdleDesktopIsNotFollowedByABurst() {
    const std::int64_t period = 8'333'333;
    PanelRatePicker picker(period);
    picker.Pick(kStartNs);
    const std::int64_t afterIdleNs = kStartNs + 1000 * period;
    Check(picker.Pick(afterIdleNs), "the first frame after an idle desktop is sent at once");
    Check(!picker.Pick(afterIdleNs + period / 2), "and the next tick is not sent to catch up");
    Check(picker.Pick(afterIdleNs + period), "the one a period later is");
}

int main() {
    TwiceTheRateGivesOneFramePerPeriod();
    ALostTickCostsNoFrame();
    AJitteringSourceKeepsOneFramePerPeriod();
    ASourceAtThePanelRateLosesNothing();
    TheFirstFrameIsSentAtOnce();
    AnIdleDesktopIsNotFollowedByABurst();
    std::printf(failures == 0 ? "panel rate picker tests passed\n" : "%d panel rate picker checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}

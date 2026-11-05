#include "../pending_frame_slot.h"

#include <cstdio>

using namespace aum;

static int failures = 0;

static void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static void TheNewestFrameReplacesTheWaitingOne() {
    PendingFrameSlot slot;
    Check(!slot.Put({nullptr, 1, false}), "the first frame replaces nothing");
    Check(slot.Put({nullptr, 2, false}), "the second frame reports the replacement");
    const auto frame = slot.Take();
    Check(frame.has_value() && frame->frameId == 2, "the newest frame is the one encoded");
}

static void AKeyframeRequestSurvivesBeingReplaced() {
    PendingFrameSlot slot;
    slot.Put({nullptr, 1, true});
    slot.Put({nullptr, 2, false});
    const auto frame = slot.Take();
    Check(frame.has_value() && frame->forceKeyframe, "the replacing frame inherits the keyframe request");
}

static void ANormalFrameIsNotForcedToAKeyframe() {
    PendingFrameSlot slot;
    slot.Put({nullptr, 1, false});
    slot.Put({nullptr, 2, false});
    Check(!slot.Take()->forceKeyframe, "no keyframe is requested out of nowhere");
}

static void TakingEmptiesTheSlot() {
    PendingFrameSlot slot;
    Check(!slot.Take().has_value(), "an empty slot gives nothing");
    slot.Put({nullptr, 1, true});
    slot.Take();
    Check(!slot.Take().has_value(), "a frame is encoded once");
    Check(!slot.Put({nullptr, 2, false}), "nothing is left to replace after a take");
    Check(!slot.Take()->forceKeyframe, "a served keyframe request does not carry over to later frames");
}

int main() {
    TheNewestFrameReplacesTheWaitingOne();
    AKeyframeRequestSurvivesBeingReplaced();
    ANormalFrameIsNotForcedToAKeyframe();
    TakingEmptiesTheSlot();
    std::printf(failures == 0 ? "pending frame slot tests passed\n" : "%d pending frame slot checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}

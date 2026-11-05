package dev.androidusbmonitor

// Time from releasing a buffer until the compositor has it. A frame closer to a deadline than this goes to the next vsync.
private const val RELEASE_MARGIN_NS = 500_000L
private const val MAX_VSYNCS_AHEAD = 1L
// How many frames in a row must have room for an earlier vsync before one is skipped to take it.
private const val FRAMES_TO_JUDGE_ROOM = 16

/**
 * Gives every decoded frame its own vsync, the earliest one it can still reach.
 *
 * The PC's frame clock and the panel never run at quite the same rate, and no device's rate is exactly what it reports, so the
 * phase between them keeps sliding. While it slides past the compositor's deadline, arrival jitter would otherwise put two frames
 * on one vsync and none on the vsync before: one frame lost and one repeated, many times a second. With a vsync per frame the
 * panel slips about once per cycle of the phase. Everything here comes from the vsyncs the system offers and from how frames
 * actually arrive; no rate or jitter is assumed.
 */
class VsyncSlots {
    private var lastPresentNs = SKIP
    private var framesJudged = 0
    private var leastRoomNs = Long.MAX_VALUE
    private var mostRoomNs = Long.MIN_VALUE

    /**
     * Places a frame decoded at [nowNs], given the soonest vsync on offer: its [presentNs], its [deadlineNs] and the [periodNs]
     * between vsyncs. Returns the present time of the frame's vsync, or [SKIP] when showing it would only add latency.
     */
    fun place(nowNs: Long, presentNs: Long, deadlineNs: Long, periodNs: Long): Long {
        val releasedNs = nowNs + RELEASE_MARGIN_NS
        val vsyncsToReachable = maxOf(0L, Math.floorDiv(releasedNs - deadlineNs - 1, periodNs) + 1)
        val earliestNs = presentNs + vsyncsToReachable * periodNs

        var targetNs = earliestNs
        if (lastPresentNs != SKIP) {
            // Half a period of tolerance: each new offer re-predicts the same vsync a few microseconds differently.
            val vsyncsToNext = Math.floorDiv(lastPresentNs + periodNs / 2 - presentNs, periodNs) + 1
            targetNs = maxOf(earliestNs, presentNs + vsyncsToNext * periodNs)
        }

        val vsyncsAhead = (targetNs - earliestNs) / periodNs
        if (vsyncsAhead > MAX_VSYNCS_AHEAD) {
            return SKIP
        }
        if (vsyncsAhead == 0L) {
            restartJudgingRoom()
        } else if (hasRoomForTheEarlierVsync(deadlineNs + vsyncsToReachable * periodNs - releasedNs)) {
            return SKIP
        }
        lastPresentNs = targetNs
        return targetNs
    }

    /**
     * A frame running a vsync late could have had the earlier one. Going back costs a skipped frame, so it is only worth it once
     * even the latest of the last frames clears that vsync's deadline by half of how much they jitter. Closer than that they
     * would soon miss it again, and again.
     */
    private fun hasRoomForTheEarlierVsync(roomNs: Long): Boolean {
        leastRoomNs = minOf(leastRoomNs, roomNs)
        mostRoomNs = maxOf(mostRoomNs, roomNs)
        if (++framesJudged < FRAMES_TO_JUDGE_ROOM) {
            return false
        }
        val hasRoom = leastRoomNs * 2 > mostRoomNs - leastRoomNs
        restartJudgingRoom()
        return hasRoom
    }

    private fun restartJudgingRoom() {
        framesJudged = 0
        leastRoomNs = Long.MAX_VALUE
        mostRoomNs = Long.MIN_VALUE
    }

    companion object {
        const val SKIP = 0L
    }
}

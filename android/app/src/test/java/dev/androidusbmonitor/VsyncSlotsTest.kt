package dev.androidusbmonitor

import java.util.Random
import kotlin.system.exitProcess

private const val NS_PER_SECOND = 1_000_000_000.0
private const val PREDICTION_NOISE_NS = 50_000L
private const val JITTER_NS = 1_000_000L

// Rates a phone may really run at, including ones that are no round number, so that nothing can pass by assuming one.
private val PANEL_RATES_HZ = listOf(60.0, 90.0, 97.3, 120.047, 120.0, 144.0, 240.0)

private var failures = 0

private fun check(condition: Boolean, what: String) {
    if (!condition) {
        println("FAIL $what")
        failures++
    }
}

private fun periodOf(rateHz: Double) = (NS_PER_SECOND / rateHz).toLong()

/** A panel whose compositor deadline sits a little over a vsync before each present, as on real phones. */
private class Panel(val periodNs: Long) {
    val deadlineLeadNs = periodNs + 1_000_000
    private val random = Random(7)

    fun place(slots: VsyncSlots, nowNs: Long): Long {
        val lastEventNs = nowNs - Math.floorMod(nowNs, periodNs)
        val soonest = Math.floorDiv(lastEventNs + deadlineLeadNs, periodNs) + 1
        val noise = (random.nextDouble() * 2 - 1) * PREDICTION_NOISE_NS
        val presentNs = soonest * periodNs + noise.toLong()
        return slots.place(nowNs, presentNs, soonest * periodNs - deadlineLeadNs, periodNs)
    }

    fun deadlineOf(presentNs: Long) = presentNs - deadlineLeadNs

    /** The earliest vsync a frame at [nowNs] can reach, on the panel's exact grid. */
    fun earliestPresentNs(nowNs: Long): Long = (Math.floorDiv(nowNs + deadlineLeadNs + 1_000_000 - 1, periodNs) + 1) * periodNs
}

private class Placed(val nowNs: Long, val presentNs: Long)

/** Frames at a steady rate with jitter. */
private fun run(panel: Panel, frameRateHz: Double, seconds: Double, jitterNs: Long): List<Placed> {
    val random = Random(11)
    val slots = VsyncSlots()
    val framePeriod = NS_PER_SECOND / frameRateHz
    val startNs = 1_000_000_000_000L
    var previousNs = startNs
    return (0 until (seconds * frameRateHz).toInt()).map { index ->
        val jitter = ((random.nextDouble() * 2 - 1) * jitterNs).toLong()
        val nowNs = maxOf(previousNs, startNs + (index * framePeriod).toLong() + jitter)
        previousNs = nowNs
        Placed(nowNs, panel.place(slots, nowNs))
    }
}

private fun shown(placed: List<Placed>) = placed.filter { it.presentNs != VsyncSlots.SKIP }

private fun repeatedVsyncs(placed: List<Placed>, periodNs: Long) =
    shown(placed).zipWithNext().sumOf { (a, b) -> maxOf(0L, Math.round((b.presentNs - a.presentNs).toDouble() / periodNs) - 1) }

/** The same frames released straight away: each reaches its earliest vsync, and of two on one vsync only the newer shows. */
private fun slipsWithoutSlots(panel: Panel, placed: List<Placed>): Long {
    val vsyncs = placed.map { panel.earliestPresentNs(it.nowNs) }
    val distinct = vsyncs.distinct()
    val repeated = distinct.zipWithNext().sumOf { (a, b) -> (b - a) / panel.periodNs - 1 }
    return (vsyncs.size - distinct.size) + repeated
}

private fun everyFrameGetsItsOwnVsync() {
    for (rate in PANEL_RATES_HZ) {
        val panel = Panel(periodOf(rate))
        val frames = shown(run(panel, rate * 1.0004, 30.0, JITTER_NS))
        check(frames.zipWithNext().all { (a, b) -> b.presentNs - a.presentNs > panel.periodNs / 2 }, "at $rate Hz no two frames share a vsync")
    }
}

private fun noFrameIsPlacedPastItsDeadline() {
    for (rate in PANEL_RATES_HZ) {
        val panel = Panel(periodOf(rate))
        val frames = shown(run(panel, rate * 0.9996, 30.0, JITTER_NS))
        check(frames.all { panel.deadlineOf(it.presentNs) + PREDICTION_NOISE_NS >= it.nowNs }, "at $rate Hz every frame can still make its vsync")
    }
}

private fun aFrameTooCloseToADeadlineWaitsForTheNextVsync() {
    for (rate in PANEL_RATES_HZ) {
        val periodNs = periodOf(rate)
        val presentNs = 1_000_000_000_000L
        val deadlineNs = presentNs - periodNs
        check(VsyncSlots().place(deadlineNs - periodNs / 2, presentNs, deadlineNs, periodNs) == presentNs, "at $rate Hz half a vsync ahead of the deadline makes it")
        check(VsyncSlots().place(deadlineNs - 1_000, presentNs, deadlineNs, periodNs) == presentNs + periodNs, "at $rate Hz a microsecond ahead of it does not")
        check(VsyncSlots().place(deadlineNs + 2 * periodNs, presentNs, deadlineNs, periodNs) == presentNs + 3 * periodNs, "at $rate Hz an old offer is carried forward")
    }
}

private fun aSlidingPhaseCostsFewSlips() {
    for (rate in PANEL_RATES_HZ) {
        for (driftPpm in listOf(-400.0, 400.0, -50.0, 50.0)) {
            val panel = Panel(periodOf(rate))
            val placed = run(panel, rate * (1 + driftPpm / 1e6), 120.0, JITTER_NS)
            val slips = repeatedVsyncs(placed, panel.periodNs) + placed.count { it.presentNs == VsyncSlots.SKIP }
            val without = slipsWithoutSlots(panel, placed)
            check(slips * 4 <= without, "at $rate Hz and $driftPpm ppm: $slips slips, against $without without slots")
        }
    }
}

private fun aBurstNeverBuildsABacklog() {
    for (rate in PANEL_RATES_HZ) {
        val panel = Panel(periodOf(rate))
        val slots = VsyncSlots()
        val startNs = 1_000_000_000_000L
        val placed = (0 until 8).map { Placed(startNs + it, panel.place(slots, startNs + it)) }
        val frames = shown(placed)
        val earliest = panel.earliestPresentNs(startNs)
        check(frames.all { it.presentNs - earliest <= panel.periodNs + PREDICTION_NOISE_NS }, "at $rate Hz a burst stays within one vsync of the earliest")
        check(placed.count { it.presentNs == VsyncSlots.SKIP } == 6, "at $rate Hz the rest of a burst is skipped")
    }
}

private fun catchesUpWhenFramesKeepRunningAhead() {
    for (rate in PANEL_RATES_HZ) {
        val panel = Panel(periodOf(rate))
        val slots = VsyncSlots()
        val startNs = 1_000_000_000_000L
        // Two frames at once put the second a vsync behind; then frames keep the panel's own pace, so nothing would ever win it back.
        val placed = (0 until 100).map { index ->
            val nowNs = startNs + maxOf(0, index - 1) * panel.periodNs
            Placed(nowNs, panel.place(slots, nowNs))
        }
        check(placed.count { it.presentNs == VsyncSlots.SKIP } == 1, "at $rate Hz one skip wins the latency back")
        val last = placed.last()
        check(last.presentNs - panel.earliestPresentNs(last.nowNs) < panel.periodNs / 2, "at $rate Hz frames end up on their earliest vsync")
    }
}

private fun staysCloseToTheEarliestVsync() {
    for (rate in PANEL_RATES_HZ) {
        for (driftPpm in listOf(-400.0, 400.0)) {
            val panel = Panel(periodOf(rate))
            val frames = shown(run(panel, rate * (1 + driftPpm / 1e6), 120.0, JITTER_NS))
            val meanBehindNs = frames.map { it.presentNs - panel.earliestPresentNs(it.nowNs) }.average()
            // Holding back is only for the stretch of the phase where frames jitter across a deadline.
            val jitterShareOfPeriod = 2.0 * JITTER_NS / panel.periodNs
            check(meanBehindNs <= panel.periodNs * minOf(1.0, jitterShareOfPeriod * 2), "at $rate Hz and $driftPpm ppm frames run ${meanBehindNs / 1e6} ms behind their earliest vsync")
        }
    }
}

private fun anIsolatedLateFrameIsWonBackQuickly() {
    for (rate in PANEL_RATES_HZ) {
        val panel = Panel(periodOf(rate))
        val slots = VsyncSlots()
        val startNs = 1_000_000_000_000L
        val placed = (0 until 200).map { index ->
            val lateNs = if (index == 50) panel.periodNs else 0L
            val nowNs = startNs + index * panel.periodNs + lateNs
            Placed(nowNs, panel.place(slots, nowNs))
        }
        val behind = placed.count { it.presentNs != VsyncSlots.SKIP && it.presentNs - panel.earliestPresentNs(it.nowNs) > panel.periodNs / 2 }
        check(behind <= 20, "at $rate Hz one late frame keeps $behind frames behind")
    }
}

private fun holdsBackWhileFramesRunCloseToADeadline() {
    for (rate in PANEL_RATES_HZ) {
        val periodNs = periodOf(rate)
        val slots = VsyncSlots()
        val presentNs = 1_000_000_000_000L
        val deadlineNs = presentNs - periodNs
        val random = Random(3)
        // Frames that just make a deadline, by a little less than they jitter, after one that missed it.
        slots.place(deadlineNs, presentNs, deadlineNs, periodNs)
        val placed = (1 until 2000).map { index ->
            val clearanceNs = (random.nextDouble() * JITTER_NS).toLong()
            val deadline = deadlineNs + index * periodNs
            slots.place(deadline - 500_000 - clearanceNs, presentNs + index * periodNs, deadline, periodNs)
        }
        check(placed.none { it == VsyncSlots.SKIP }, "at $rate Hz nothing is skipped to chase a vsync frames would soon miss again")
    }
}

private fun aFrameThatMissesTheEarlierVsyncRestartsTheJudgement() {
    for (rate in PANEL_RATES_HZ) {
        val periodNs = periodOf(rate)
        val firstPresentNs = 1_000_000_000_000L
        val presentOf = { vsync: Int -> firstPresentNs + vsync * periodNs }
        val deadlineOf = { vsync: Int -> presentOf(vsync) - periodNs }
        val slots = VsyncSlots()
        slots.place(deadlineOf(0) - periodNs / 2, presentOf(0), deadlineOf(0), periodNs)
        // Most frames have plenty of room for the earlier vsync, but every so often one misses it: going back would not last.
        val placed = (1 until 400).map { vsync ->
            val fromDeadlineNs = if (vsync % 15 == 0) 1_000L else -periodNs / 2
            slots.place(deadlineOf(vsync - 1) + fromDeadlineNs, presentOf(vsync - 1), deadlineOf(vsync - 1), periodNs)
        }
        check(placed.none { it == VsyncSlots.SKIP }, "at $rate Hz a miss every few frames keeps it from going back")
    }
}

private fun aLateFrameCostsOneSlip() {
    for (rate in PANEL_RATES_HZ) {
        val panel = Panel(periodOf(rate))
        val slots = VsyncSlots()
        val startNs = 1_000_000_000_000L
        val placed = (0 until 200).map { index ->
            val lateNs = if (index == 50) panel.periodNs else 0L
            val nowNs = startNs + index * panel.periodNs + lateNs
            Placed(nowNs, panel.place(slots, nowNs))
        }
        check(repeatedVsyncs(placed, panel.periodNs) == 1L, "at $rate Hz one late frame repeats one vsync")
        check(placed.count { it.presentNs == VsyncSlots.SKIP } <= 1, "at $rate Hz one late frame loses at most one frame")
    }
}

fun main() {
    everyFrameGetsItsOwnVsync()
    noFrameIsPlacedPastItsDeadline()
    aFrameTooCloseToADeadlineWaitsForTheNextVsync()
    aSlidingPhaseCostsFewSlips()
    aBurstNeverBuildsABacklog()
    catchesUpWhenFramesKeepRunningAhead()
    staysCloseToTheEarliestVsync()
    anIsolatedLateFrameIsWonBackQuickly()
    holdsBackWhileFramesRunCloseToADeadline()
    aFrameThatMissesTheEarlierVsyncRestartsTheJudgement()
    aLateFrameCostsOneSlip()
    if (failures > 0) {
        println("$failures vsync slot checks failed")
        exitProcess(1)
    }
    println("vsync slot tests passed")
}

package dev.androidusbmonitor

import android.annotation.TargetApi
import android.os.Build
import android.os.Handler
import android.util.Log
import android.view.Choreographer

private const val TAG = "AndroidUsbMonitor"

/**
 * Follows the vsyncs the system predicts for this panel. Everything runs on the thread of [handler], which needs no locks.
 * Android 13 and newer: MainActivity turns older devices away, apart from a TV that plays tunneled and has no use for this.
 */
@TargetApi(Build.VERSION_CODES.TIRAMISU)
class VsyncPacer(private val handler: Handler, private val presentationDeadlineNs: Long) : Choreographer.VsyncCallback {
    private val slots = VsyncSlots()
    private var choreographer: Choreographer? = null
    private var presentNs = 0L
    private var deadlineNs = 0L
    private var periodNs = 0L

    init {
        handler.post { choreographer = Choreographer.getInstance().also { it.postVsyncCallback(this) } }
    }

    override fun onVsync(data: Choreographer.FrameData) {
        val timelines = data.frameTimelines
        val soonest = timelines[0]
        val soonestPresentNs = soonest.expectedPresentationTimeNanos
        val newPeriodNs = when {
            timelines.size > 1 -> timelines[1].expectedPresentationTimeNanos - soonestPresentNs
            presentNs != 0L -> soonestPresentNs - presentNs
            else -> 0L
        }
        if (periodNs == 0L && newPeriodNs > 0) {
            Log.i(TAG, "vsync period $newPeriodNs ns, deadline ${soonestPresentNs - soonest.deadlineNanos} ns before present, display says $presentationDeadlineNs, ${timelines.size} timelines")
        }
        periodNs = newPeriodNs
        presentNs = soonestPresentNs
        // The timeline's deadline is the one for app drawing. Some panels need a buffer earlier than that: the display says how much.
        deadlineNs = minOf(soonest.deadlineNanos, soonestPresentNs - presentationDeadlineNs)
        choreographer?.postVsyncCallback(this)
    }

    /** The present time of the vsync for a frame decoded at [nowNs], or [VsyncSlots.SKIP]. */
    fun schedule(nowNs: Long): Long =
        if (periodNs > 0) slots.place(nowNs, presentNs, deadlineNs, periodNs) else VsyncSlots.SKIP

    /** Half a period early, so the compositor picks exactly that vsync even if the prediction moves a little. */
    fun releaseTimestampNs(scheduledPresentNs: Long) = scheduledPresentNs - periodNs / 2

    fun stop() {
        handler.post {
            choreographer?.removeVsyncCallback(this)
            choreographer = null
        }
    }
}

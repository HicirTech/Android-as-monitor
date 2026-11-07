package dev.androidusbmonitor

import android.os.Build

// The panel runs at 120 Hz, but this TV's decoder path tops out near 1000 Mpixel/s: 119.7 fps at the 3840x2176 NVENC codes for 2160 lines.
// At 120 fps its queue only ever grows.
private const val G05_4K_REFRESH_HZ = 119f

/** A TV whose Android lays out and reports a lower resolution than its panel, so that only this table knows the panel. */
data class TvPanel(val width: Int, val height: Int, val refreshHz: Float) {
    companion object {
        /** Null on every other device, which gets what Android reports. */
        val current: TvPanel? = when (Build.PRODUCT) {
            "G05_4K_GB" -> TvPanel(3840, 2160, G05_4K_REFRESH_HZ)
            else -> null
        }
    }
}

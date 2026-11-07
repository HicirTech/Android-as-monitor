package dev.androidusbmonitor

import android.content.Context
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build
import android.provider.Settings
import android.util.DisplayMetrics
import android.view.Display
import java.security.MessageDigest
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

private const val MM_PER_INCH = 25.4

data class DecoderSupport(val name: String, val coversNative: Boolean, val lowLatency: Boolean)

/** The panel as the PC must see it, measured from the mode the display is actually running. */
data class PanelSpec(
    val panelWidth: Int,
    val panelHeight: Int,
    val refreshHz: Float,
    val widthMm: Double,
    val heightMm: Double,
    val xdpi: Float,
    val ydpi: Float,
    val densityDpi: Int,
    val rotation: Int,
    val supportedModes: String,
)

fun bestNativeMode(display: Display): Display.Mode =
    display.supportedModes.maxWith(compareBy({ it.physicalWidth * it.physicalHeight }, { it.refreshRate }))

fun measurePanel(display: Display): PanelSpec {
    val mode = display.mode
    val metrics = DisplayMetrics()
    @Suppress("DEPRECATION")
    display.getRealMetrics(metrics)

    // Mode sizes and the DPI values both describe the panel's natural axes, whatever the current rotation is.
    val naturalWidthMm = mode.physicalWidth / metrics.xdpi * MM_PER_INCH
    val naturalHeightMm = mode.physicalHeight / metrics.ydpi * MM_PER_INCH
    val isNaturallyLandscape = mode.physicalWidth >= mode.physicalHeight

    val reported = PanelSpec(
        panelWidth = max(mode.physicalWidth, mode.physicalHeight),
        panelHeight = min(mode.physicalWidth, mode.physicalHeight),
        refreshHz = mode.refreshRate,
        widthMm = if (isNaturallyLandscape) naturalWidthMm else naturalHeightMm,
        heightMm = if (isNaturallyLandscape) naturalHeightMm else naturalWidthMm,
        xdpi = metrics.xdpi,
        ydpi = metrics.ydpi,
        densityDpi = metrics.densityDpi,
        rotation = display.rotation,
        supportedModes = display.supportedModes.joinToString(" ") { "${it.physicalWidth}x${it.physicalHeight}@${it.refreshRate}" },
    )
    return TvPanel.current?.let {
        reported.copy(
            panelWidth = it.width,
            panelHeight = it.height,
            refreshHz = it.refreshHz,
            xdpi = reported.xdpi * it.width / reported.panelWidth,
            ydpi = reported.ydpi * it.height / reported.panelHeight,
        )
    } ?: reported
}

fun findDecoder(mime: String, width: Int, height: Int, fps: Int): DecoderSupport? {
    val candidates = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
        !info.isEncoder && info.isHardwareAccelerated && info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
    }
    val rated = candidates.map { info ->
        val capabilities = info.getCapabilitiesForType(mime)
        val video = capabilities.videoCapabilities
        val wanted = MediaCodecInfo.VideoCapabilities.PerformancePoint(width, height, fps)
        val covers = video.supportedPerformancePoints?.any { it.covers(wanted) }
            ?: video.areSizeAndRateSupported(width, height, fps.toDouble())
        DecoderSupport(info.name, covers, capabilities.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency))
    }
    return rated.firstOrNull { it.coversNative } ?: rated.firstOrNull()
}

private fun stableDeviceId(context: Context): String {
    val androidId = Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID) ?: "unknown"
    return MessageDigest.getInstance("SHA-256").digest(androidId.toByteArray()).joinToString("") { "%02x".format(it) }
}

fun buildHello(context: Context, panel: PanelSpec, surfaceWidth: Int, surfaceHeight: Int, audioSampleRate: Int): String {
    val fps = panel.refreshHz.roundToInt()
    val hevc = findDecoder("video/hevc", panel.panelWidth, panel.panelHeight, fps)
    val avc = findDecoder("video/avc", panel.panelWidth, panel.panelHeight, fps)
    return buildString {
        appendLine("protocol=1")
        appendLine("manufacturer=${Build.MANUFACTURER}")
        appendLine("model=${Build.MODEL}")
        appendLine("deviceId=${stableDeviceId(context)}")
        appendLine("panelWidth=${panel.panelWidth}")
        appendLine("panelHeight=${panel.panelHeight}")
        appendLine("surfaceWidth=$surfaceWidth")
        appendLine("surfaceHeight=$surfaceHeight")
        appendLine("refreshHz=${panel.refreshHz}")
        appendLine("widthMm=${panel.widthMm}")
        appendLine("heightMm=${panel.heightMm}")
        appendLine("xdpi=${panel.xdpi}")
        appendLine("ydpi=${panel.ydpi}")
        appendLine("densityDpi=${panel.densityDpi}")
        appendLine("rotation=${panel.rotation}")
        appendLine("supportedModes=${panel.supportedModes}")
        appendLine("audioSampleRate=$audioSampleRate")
        appendLine("hevcDecoder=${hevc?.name.orEmpty()}")
        appendLine("hevcCoversNative=${hevc?.coversNative ?: false}")
        appendLine("hevcLowLatency=${hevc?.lowLatency ?: false}")
        appendLine("avcDecoder=${avc?.name.orEmpty()}")
        appendLine("avcCoversNative=${avc?.coversNative ?: false}")
        appendLine("avcLowLatency=${avc?.lowLatency ?: false}")
    }
}

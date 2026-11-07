package dev.androidusbmonitor

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.hardware.display.DisplayManager
import android.media.AudioManager
import android.net.wifi.WifiManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView

private const val MODE_SWITCH_TIMEOUT_MS = 1500L
private const val DEFAULT_AUDIO_SAMPLE_RATE = 48000
private const val EXTRA_WIFI = "wifi"
private const val WIFI_LOCK_TAG = "AndroidUsbMonitor"

class MainActivity : Activity(), SurfaceHolder.Callback, DisplayManager.DisplayListener {
    private lateinit var surfaceView: SurfaceView
    private lateinit var status: TextView
    private val mainHandler = Handler(Looper.getMainLooper())

    private var surface: Surface? = null
    private var surfaceWidth = 0
    private var surfaceHeight = 0
    private var hasWindowFocus = false
    private var modeSettled = false
    private var session: StreamSession? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private val overWifi get() = intent.getBooleanExtra(EXTRA_WIFI, false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        configureWindow()
        if (overWifi) {
            // Wi-Fi power save collapses the stream for seconds otherwise. Android applies this only with the app in front and the screen on.
            wifiLock = getSystemService(WifiManager::class.java).createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, WIFI_LOCK_TAG).apply { acquire() }
        }

        surfaceView = SurfaceView(this)
        status = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 20f
            gravity = Gravity.CENTER
            text = "Starting"
        }
        setContentView(FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(surfaceView)
            addView(status, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        })
        hideSystemBars()
        surfaceView.holder.addCallback(this)

        requestNativeMode()
    }

    private fun configureWindow() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.setDecorFitsSystemWindows(false)
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            preferMinimalPostProcessing = true
        }
    }

    private fun hideSystemBars() {
        window.insetsController?.let {
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            it.hide(WindowInsets.Type.systemBars())
        }
    }

    private fun requestNativeMode() {
        val target = bestNativeMode(display)
        window.attributes = window.attributes.apply { preferredDisplayModeId = target.modeId }
        // The buffer must be the panel's real pixels even on devices that lay their UI out at a lower resolution.
        val width = TvPanel.current?.width ?: maxOf(target.physicalWidth, target.physicalHeight)
        val height = TvPanel.current?.height ?: minOf(target.physicalWidth, target.physicalHeight)
        surfaceView.holder.setFixedSize(width, height)

        getSystemService(DisplayManager::class.java).registerDisplayListener(this, mainHandler)
        if (display.mode.modeId == target.modeId) {
            modeSettled = true
        } else {
            // The system may refuse, for example when the user limited the refresh rate. What actually runs is reported.
            mainHandler.postDelayed({
                modeSettled = true
                tryStart()
            }, MODE_SWITCH_TIMEOUT_MS)
        }
    }

    override fun onDisplayChanged(displayId: Int) {
        if (displayId == display.displayId && display.mode.modeId == window.attributes.preferredDisplayModeId) {
            modeSettled = true
            tryStart()
        }
    }

    override fun onDisplayAdded(displayId: Int) = Unit
    override fun onDisplayRemoved(displayId: Int) = Unit

    override fun onWindowFocusChanged(focused: Boolean) {
        super.onWindowFocusChanged(focused)
        hasWindowFocus = focused
        if (focused) {
            hideSystemBars()
            tryStart()
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Unit

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surface = holder.surface
        surfaceWidth = width
        surfaceHeight = height
        tryStart()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stopSession()
        surface = null
    }

    private fun tryStart() {
        val target = surface ?: return
        if (session != null || !hasWindowFocus || !modeSettled) {
            return
        }
        val panel = measurePanel(display)
        // A frame rate request keeps adaptive panels from dropping below the rate that was just reported to the PC.
        target.setFrameRate(panel.refreshHz, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, Surface.CHANGE_FRAME_RATE_ALWAYS)

        val nativeRate = getSystemService(Context.AUDIO_SERVICE).let { it as AudioManager }
            .getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: DEFAULT_AUDIO_SAMPLE_RATE
        val hello = buildHello(this, panel, surfaceWidth, surfaceHeight, nativeRate)
        // This TV's SurfaceFlinger stays at 60 Hz whatever the panel can do, so its picture has to go around it.
        val tunnelAudioSession = TvPanel.current?.let { getSystemService(AudioManager::class.java).generateAudioSessionId() }
        session = StreamSession(
            target,
            hello,
            display.presentationDeadlineNanos,
            overWifi,
            tunnelAudioSession,
            onStreaming = { mainHandler.post { status.visibility = View.GONE } },
            onWaiting = {
                mainHandler.post {
                    status.visibility = View.VISIBLE
                    status.text = "${panel.panelWidth} x ${panel.panelHeight} at ${panel.refreshHz} Hz\nWaiting for the PC companion"
                }
            },
        )
    }

    private fun stopSession() {
        session?.stop()
        session = null
    }

    override fun onDestroy() {
        getSystemService(DisplayManager::class.java).unregisterDisplayListener(this)
        stopSession()
        wifiLock?.release()
        super.onDestroy()
    }
}

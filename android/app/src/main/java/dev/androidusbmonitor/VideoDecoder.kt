package dev.androidusbmonitor

import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

private const val TAG = "AndroidUsbMonitor"
private const val PTS_MICROSECONDS_PER_FRAME_ID = 1000L
private const val TRACE_CAPACITY = 1024
private const val INPUT_WAIT_MS = 20L
private const val MAX_INPUT_BUFFERS = 64

class VideoDecoder(
    surface: Surface,
    mime: String,
    width: Int,
    height: Int,
    refreshHz: Float,
    requestedDecoder: String?,
    presentationDeadlineNs: Long,
    private val connection: Connection,
    private val onFirstFrameShown: () -> Unit,
) {
    private val thread = HandlerThread("video-decoder", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    private val handler = Handler(thread.looper)
    private val pacer = VsyncPacer(handler, presentationDeadlineNs)
    private val availableInputs = ArrayBlockingQueue<Int>(MAX_INPUT_BUFFERS)
    private val codec: MediaCodec

    private val receivedNs = LongArray(TRACE_CAPACITY)
    private val queuedNs = LongArray(TRACE_CAPACITY)
    private val decodedNs = LongArray(TRACE_CAPACITY)
    private val statsPayload = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN)

    private var needKeyframe = true
    private var firstFrameShown = false

    init {
        val format = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            setFloat(MediaFormat.KEY_OPERATING_RATE, refreshHz)
            // Vendor equivalents of low latency; decoders ignore keys they do not know.
            setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
            setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
            setInteger("vendor.low-latency.enable", 1)
        }
        val name = requestedDecoder
            ?: MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(format)
            ?: findDecoder(mime, width, height, refreshHz.toInt())?.name
            ?: error("no hardware decoder for $mime at ${width}x$height")
        Log.i(TAG, "decoder $name for $mime ${width}x$height at $refreshHz Hz")
        codec = MediaCodec.createByCodecName(name)
        codec.setCallback(callback(), handler)
        codec.configure(format, surface, null, 0)
        codec.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT)
        codec.start()
    }

    private fun callback() = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
            availableInputs.offer(index)
        }

        override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            val frameId = info.presentationTimeUs / PTS_MICROSECONDS_PER_FRAME_ID
            val slot = (frameId % TRACE_CAPACITY).toInt()
            val nowNs = System.nanoTime()
            decodedNs[slot] = nowNs
            val presentNs = pacer.schedule(nowNs)
            if (presentNs == VsyncSlots.SKIP) {
                codec.releaseOutputBuffer(index, false)
            } else {
                codec.releaseOutputBuffer(index, pacer.releaseTimestampNs(presentNs))
            }
            sendStats(frameId, slot, presentNs)
            if (!firstFrameShown && presentNs != VsyncSlots.SKIP) {
                firstFrameShown = true
                onFirstFrameShown()
            }
        }

        override fun onError(codec: MediaCodec, error: MediaCodec.CodecException) {
            Log.e(TAG, "decoder failed: ${error.diagnosticInfo}", error)
            connection.close()
        }

        override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) = Unit
    }

    private fun sendStats(frameId: Long, slot: Int, presentNs: Long) {
        statsPayload.clear()
        statsPayload.putLong(receivedNs[slot]).putLong(queuedNs[slot]).putLong(decodedNs[slot]).putLong(presentNs)
        runCatching { connection.send(PacketType.FRAME_STATS, frameId, statsPayload.array()) }
    }

    /** Called on the stream thread with the time the frame finished arriving. */
    fun queueFrame(data: ByteArray, length: Int, frameId: Long, flags: Int, arrivedNs: Long) {
        val isKeyframe = flags and FLAG_KEYFRAME != 0
        if (needKeyframe && !isKeyframe) {
            return
        }
        val index = availableInputs.poll(INPUT_WAIT_MS, TimeUnit.MILLISECONDS)
        if (index == null) {
            // A dropped frame breaks the reference chain, so everything until the next keyframe would be garbage.
            needKeyframe = true
            connection.send(PacketType.KEYFRAME_REQUEST, 0)
            return
        }
        needKeyframe = false

        val slot = (frameId % TRACE_CAPACITY).toInt()
        receivedNs[slot] = arrivedNs
        val buffer = codec.getInputBuffer(index) ?: error("decoder returned no input buffer")
        buffer.clear()
        buffer.put(data, 0, length)
        queuedNs[slot] = System.nanoTime()
        codec.queueInputBuffer(index, 0, length, frameId * PTS_MICROSECONDS_PER_FRAME_ID, 0)
    }

    fun release() {
        pacer.stop()
        runCatching { codec.stop() }
        runCatching { codec.release() }
        thread.quitSafely()
    }
}

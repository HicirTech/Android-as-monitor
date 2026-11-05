package dev.androidusbmonitor

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

private const val TAG = "AndroidUsbMonitor"
private const val CHANNELS = 2
private const val BYTES_PER_SAMPLE = 4
private const val BUFFER_MILLISECONDS = 40

class AudioPlayer(sampleRate: Int) {
    private val track: AudioTrack
    private var buffer = ByteBuffer.allocateDirect(64 * 1024).order(ByteOrder.nativeOrder())
    private var writtenBytes = 0L
    private var discardedBytes = 0L

    init {
        val bufferBytes = sampleRate * BUFFER_MILLISECONDS / 1000 * CHANNELS * BYTES_PER_SAMPLE
        val minimum = AudioTrack.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT)
        track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build(),
            )
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setBufferSizeInBytes(maxOf(bufferBytes, minimum))
            .build()
        track.play()
    }

    /**
     * Never blocks the stream thread, which also carries the picture. When the PC's clock runs ahead of the phone's and
     * the track is full, the excess is discarded, so sound can never drift behind the picture.
     */
    fun write(data: ByteArray, length: Int) {
        if (buffer.capacity() < length) {
            buffer = ByteBuffer.allocateDirect(length * 2).order(ByteOrder.nativeOrder())
        }
        buffer.clear()
        buffer.put(data, 0, length)
        buffer.flip()
        val written = track.write(buffer, length, AudioTrack.WRITE_NON_BLOCKING)
        if (written > 0) {
            writtenBytes += written
        }
        discardedBytes += length - maxOf(written, 0)
    }

    fun release() {
        val frameBytes = CHANNELS * BYTES_PER_SAMPLE
        Log.i(
            TAG,
            "audio: wrote ${writtenBytes / frameBytes} frames, discarded ${discardedBytes / frameBytes}, " +
                "played ${track.playbackHeadPosition}, underruns ${track.underrunCount}, " +
                "low latency ${track.performanceMode == AudioTrack.PERFORMANCE_MODE_LOW_LATENCY}, buffer ${track.bufferSizeInFrames} frames",
        )
        runCatching { track.stop() }
        track.release()
    }
}

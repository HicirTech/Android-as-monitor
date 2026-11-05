package dev.androidusbmonitor

import android.os.Process
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

private const val TAG = "AndroidUsbMonitor"
private const val RECONNECT_DELAY_MS = 250L

class StreamSession(
    private val surface: Surface,
    private val hello: String,
    private val presentationDeadlineNs: Long,
    private val onStreaming: () -> Unit,
    private val onWaiting: () -> Unit,
) {
    @Volatile private var stopped = false
    @Volatile private var connection: Connection? = null

    private val worker = thread(name = "stream", isDaemon = true) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
        while (!stopped) {
            onWaiting()
            runCatching { runConnection() }.onFailure { Log.i(TAG, "connection ended: $it") }
            if (!stopped) {
                Thread.sleep(RECONNECT_DELAY_MS)
            }
        }
    }

    private fun runConnection() {
        val link = Connection.open()
        connection = link
        var decoder: VideoDecoder? = null
        var audio: AudioPlayer? = null
        val pongPayload = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        try {
            link.send(PacketType.HELLO, 0, hello.toByteArray())
            val packet = Packet()
            while (!stopped) {
                link.read(packet)
                val arrivedNs = System.nanoTime()
                when (packet.type) {
                    PacketType.START -> {
                        val start = parseKeyValues(String(packet.payload, 0, packet.length))
                        decoder = VideoDecoder(
                            surface,
                            start.getValue("codec"),
                            start.getValue("width").toInt(),
                            start.getValue("height").toInt(),
                            start.getValue("refreshHz").toFloat(),
                            start["decoder"],
                            presentationDeadlineNs,
                            link,
                            onStreaming,
                        )
                        audio = start["sampleRate"]?.let { AudioPlayer(it.toInt()) }
                        link.send(PacketType.READY, 0)
                    }
                    PacketType.VIDEO_FRAME -> decoder?.queueFrame(packet.payload, packet.length, packet.timestamp, packet.flags, arrivedNs)
                    PacketType.AUDIO_PCM -> audio?.write(packet.payload, packet.length)
                    PacketType.PING -> {
                        pongPayload.clear()
                        pongPayload.putLong(System.nanoTime())
                        link.send(PacketType.PONG, packet.timestamp, pongPayload.array())
                    }
                }
            }
        } finally {
            connection = null
            link.close()
            decoder?.release()
            audio?.release()
        }
    }

    private fun parseKeyValues(text: String): Map<String, String> =
        text.lineSequence().filter { '=' in it }.associate { it.substringBefore('=') to it.substringAfter('=') }

    fun stop() {
        stopped = true
        connection?.close()
        worker.join()
    }
}

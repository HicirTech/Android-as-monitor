package dev.androidusbmonitor

import android.net.LocalSocket
import android.net.LocalSocketAddress
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder

// Mirrors shared/stream_protocol.h on the PC side.
object PacketType {
    const val HELLO = 1
    const val START = 2
    const val READY = 3
    const val VIDEO_FRAME = 4
    const val AUDIO_PCM = 5
    const val PING = 6
    const val PONG = 7
    const val FRAME_STATS = 8
    const val KEYFRAME_REQUEST = 9
}

const val FLAG_KEYFRAME = 0x01
const val STREAM_SOCKET_NAME = "android_usb_monitor"
const val STREAM_PORT = 27183

private const val HEADER_BYTES = 16
private const val RECEIVE_BUFFER_BYTES = 2 * 1024 * 1024
private const val INPUT_BUFFER_BYTES = 256 * 1024
private const val ACCEPT_TIMEOUT_MS = 250

class Packet {
    var type = 0
    var flags = 0
    var timestamp = 0L
    var length = 0
    var payload = ByteArray(512 * 1024)
}

class Connection private constructor(stream: InputStream, private val output: OutputStream, private val closeSocket: () -> Unit) {
    private val input = DataInputStream(BufferedInputStream(stream, INPUT_BUFFER_BYTES))
    private val headerIn = ByteArray(HEADER_BYTES)
    private val headerInView = ByteBuffer.wrap(headerIn).order(ByteOrder.LITTLE_ENDIAN)
    private val sendLock = Any()

    fun read(packet: Packet) {
        input.readFully(headerIn)
        packet.type = headerIn[0].toInt() and 0xFF
        packet.flags = headerIn[1].toInt() and 0xFF
        packet.length = headerInView.getInt(4)
        packet.timestamp = headerInView.getLong(8)
        if (packet.payload.size < packet.length) {
            packet.payload = ByteArray(packet.length * 2)
        }
        input.readFully(packet.payload, 0, packet.length)
    }

    fun send(type: Int, timestamp: Long, payload: ByteArray = EMPTY, length: Int = payload.size) {
        val message = ByteBuffer.allocate(HEADER_BYTES + length).order(ByteOrder.LITTLE_ENDIAN)
        message.put(type.toByte()).put(0).putShort(0).putInt(length).putLong(timestamp).put(payload, 0, length)
        synchronized(sendLock) {
            output.write(message.array())
            output.flush()
        }
    }

    fun close() = closeSocket()

    companion object {
        private val EMPTY = ByteArray(0)

        fun open(): Connection {
            val socket = LocalSocket()
            socket.connect(LocalSocketAddress(STREAM_SOCKET_NAME))
            socket.receiveBufferSize = RECEIVE_BUFFER_BYTES
            return Connection(socket.inputStream, socket.outputStream) {
                // Closing a LocalSocket alone does not wake a thread blocked reading from it.
                runCatching { socket.shutdownInput() }
                runCatching { socket.shutdownOutput() }
                runCatching { socket.close() }
            }
        }

        fun over(socket: Socket): Connection {
            socket.tcpNoDelay = true
            return Connection(socket.inputStream, socket.outputStream) { runCatching { socket.close() } }
        }
    }
}

// The phone's end of a Wi-Fi link. The phone listens so that Windows needs no inbound firewall rule.
class ConnectionListener(address: InetSocketAddress) : AutoCloseable {
    private val server = ServerSocket().apply {
        reuseAddress = true
        // Set before binding, because accepted sockets negotiate their window from it.
        receiveBufferSize = RECEIVE_BUFFER_BYTES
        soTimeout = ACCEPT_TIMEOUT_MS
        bind(address)
    }

    // Throws SocketTimeoutException when no PC came, so that a stopped session is not left waiting for one.
    fun accept() = Connection.over(server.accept())

    override fun close() = server.close()
}

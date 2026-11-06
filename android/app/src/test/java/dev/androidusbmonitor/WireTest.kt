package dev.androidusbmonitor

import java.io.EOFException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread
import kotlin.system.exitProcess

private const val HEADER_BYTES = 16
private const val JOIN_TIMEOUT_MS = 5000L
private const val READER_BLOCKS_AFTER_MS = 100L
// Larger than a new Packet's buffer, so that it has to grow.
private const val LARGE_FRAME_BYTES = 600 * 1024

private val LOOPBACK = InetAddress.getLoopbackAddress()

private var failures = 0

private fun check(condition: Boolean, what: String) {
    if (!condition) {
        println("FAIL $what")
        failures++
    }
}

private fun freeAddress() = ServerSocket(0, 1, LOOPBACK).use { InetSocketAddress(LOOPBACK, it.localPort) }

private fun withLink(test: (pc: Socket, phone: Connection) -> Unit) {
    val address = freeAddress()
    ConnectionListener(address).use { listener ->
        Socket(address.address, address.port).use { pc ->
            val phone = listener.accept()
            try {
                test(pc, phone)
            } finally {
                phone.close()
            }
        }
    }
}

private fun aPacketFromThePcArrivesWhole() = withLink { pc, phone ->
    val payload = ByteArray(LARGE_FRAME_BYTES) { it.toByte() }
    val message = ByteBuffer.allocate(HEADER_BYTES + payload.size).order(ByteOrder.LITTLE_ENDIAN)
    message.put(PacketType.VIDEO_FRAME.toByte()).put(FLAG_KEYFRAME.toByte()).putShort(0).putInt(payload.size).putLong(42).put(payload)
    val writer = thread(isDaemon = true) { pc.getOutputStream().write(message.array()) }

    val packet = Packet()
    phone.read(packet)
    writer.join(JOIN_TIMEOUT_MS)
    check(packet.type == PacketType.VIDEO_FRAME && packet.flags == FLAG_KEYFRAME && packet.timestamp == 42L, "the header arrives as sent")
    check(packet.length == payload.size && packet.payload.copyOf(packet.length).contentEquals(payload), "and so does the payload")
}

private fun aPacketToThePcIsFramedTheSameWay() = withLink { pc, phone ->
    val payload = byteArrayOf(1, 2, 3)
    phone.send(PacketType.PONG, 42, payload)

    val received = ByteArray(HEADER_BYTES + payload.size)
    pc.getInputStream().readNBytes(received, 0, received.size)
    val view = ByteBuffer.wrap(received).order(ByteOrder.LITTLE_ENDIAN)
    check(received[0].toInt() == PacketType.PONG && view.getInt(4) == payload.size && view.getLong(8) == 42L, "the PC reads the header the protocol describes")
    check(received.copyOfRange(HEADER_BYTES, received.size).contentEquals(payload), "and then the payload")
}

private fun aStoppedSessionIsNotLeftWaitingForAPc() {
    val address = freeAddress()
    ConnectionListener(address).use { listener ->
        var waited: Result<Connection>? = null
        val waiter = thread(isDaemon = true) { waited = runCatching { listener.accept() } }
        waiter.join(JOIN_TIMEOUT_MS)
        check(waited?.exceptionOrNull() is SocketTimeoutException, "waiting for a PC ends, so that the session can see it was stopped")
        if (waiter.isAlive) {
            return@use
        }
        Socket(address.address, address.port).use {
            val phone = runCatching { listener.accept() }
            check(phone.isSuccess, "and a PC that comes after that still connects")
            phone.getOrNull()?.close()
        }
    }
}

private fun closingWakesAThreadBlockedReading() = withLink { _, phone ->
    val reader = thread(isDaemon = true) { runCatching { phone.read(Packet()) } }
    Thread.sleep(READER_BLOCKS_AFTER_MS)
    phone.close()
    reader.join(JOIN_TIMEOUT_MS)
    check(!reader.isAlive, "closing wakes the reader, because stopping a session waits for it")
}

private fun aPcThatLeavesEndsTheRead() = withLink { pc, phone ->
    pc.close()
    check(runCatching { phone.read(Packet()) }.exceptionOrNull() is EOFException, "a PC that leaves ends the read, so that the session waits for the next one")
}

private fun theNextSessionCanListenOnTheSamePort() {
    val address = freeAddress()
    ConnectionListener(address).use { listener ->
        Socket(address.address, address.port).use { listener.accept().close() }
    }
    val again = runCatching { ConnectionListener(address).close() }
    check(again.isSuccess, "the port is free again right after a session used it: ${again.exceptionOrNull()}")
}

fun main() {
    aPacketFromThePcArrivesWhole()
    aPacketToThePcIsFramedTheSameWay()
    aStoppedSessionIsNotLeftWaitingForAPc()
    closingWakesAThreadBlockedReading()
    aPcThatLeavesEndsTheRead()
    theNextSessionCanListenOnTheSamePort()
    if (failures > 0) {
        println("$failures wire checks failed")
        exitProcess(1)
    }
    println("wire tests passed")
}

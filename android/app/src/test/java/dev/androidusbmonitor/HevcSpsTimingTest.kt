package dev.androidusbmonitor

import java.io.ByteArrayOutputStream
import kotlin.system.exitProcess

// What NVENC writes for 3840x2160 at 120 fps: num_units_in_tick 1, time_scale 120.
private val VPS = hex("40010c01ffff21600000030090000003000003009c970240")
private val SPS = hex("42010121600000030090000003000003009ca001e02002207c4e5974a4211917543016a020202080000003008000003c17d240")
private val PPS = hex("4401c1f7c0cc90")
private val SLICE = hex("2601af1c00000380ff")

private val SPS_AT_60 = hex("42010121600000030090000003000003009ca001e02002207c4e5974a4211917543016a020202080000003008000001e17d240")
// time_scale 2 turns its own bytes into 00 00 01, which must not reach the decoder unescaped.
private val SPS_AT_2 = hex("42010121600000030090000003000003009ca001e02002207c4e5974a4211917543016a02020208000000300800000030117d240")

private val START_CODE = hex("00000001")
private val SHORT_START_CODE = hex("000001")

private var failures = 0

private fun check(condition: Boolean, what: String) {
    if (!condition) {
        println("FAIL $what")
        failures++
    }
}

private fun hex(text: String) = ByteArray(text.length / 2) { text.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

private fun frameOf(vararg nals: ByteArray, startCode: ByteArray = START_CODE): ByteArray =
    nals.fold(ByteArray(0)) { frame, nal -> frame + startCode + nal }

private fun rewrite(frame: ByteArray, fps: Int) = withDeclaredHevcFrameRate(frame, frame.size, fps)

private class BitWriter {
    private val out = ByteArrayOutputStream()
    private var current = 0
    var position = 0
        private set

    fun u(count: Int, value: Long) {
        for (bit in count - 1 downTo 0) {
            current = (current shl 1) or ((value shr bit) and 1).toInt()
            if (++position % 8 == 0) {
                out.write(current)
                current = 0
            }
        }
    }

    fun flag(value: Boolean) = u(1, if (value) 1 else 0)

    fun ue(value: Long) {
        val coded = value + 1
        val bits = 64 - java.lang.Long.numberOfLeadingZeros(coded)
        u(bits - 1, 0)
        u(bits, coded)
    }

    /** Adds the stop bit and the alignment that end an RBSP. */
    fun finish(): ByteArray {
        u(1, 1)
        while (position % 8 != 0) {
            u(1, 0)
        }
        return out.toByteArray()
    }
}

private fun escaped(rbsp: ByteArray): ByteArray {
    val out = ByteArrayOutputStream()
    var zeros = 0
    for (value in rbsp) {
        val byte = value.toInt() and 0xFF
        if (zeros == 2 && byte <= 3) {
            out.write(3)
            zeros = 0
        }
        out.write(byte)
        zeros = if (byte == 0) zeros + 1 else 0
    }
    return out.toByteArray()
}

private fun unescaped(payload: ByteArray): ByteArray {
    val out = ByteArrayOutputStream()
    var zeros = 0
    for (value in payload) {
        val byte = value.toInt() and 0xFF
        if (zeros == 2 && byte == 3) {
            zeros = 0
            continue
        }
        out.write(byte)
        zeros = if (byte == 0) zeros + 1 else 0
    }
    return out.toByteArray()
}

private fun readU32(rbsp: ByteArray, position: Int): Long {
    var value = 0L
    for (index in position until position + 32) {
        value = (value shl 1) or ((rbsp[index shr 3].toLong() shr (7 - (index and 7))) and 1)
    }
    return value
}

private class SpsShape(
    val hasVui: Boolean = true,
    val hasTiming: Boolean = true,
    val scalingListData: Boolean = false,
    val predictedRefPicSet: Boolean = false,
)

private class BuiltSps(val nal: ByteArray, val timingFields: Int)

/** An SPS with every optional part this has to step over, as an encoder may write it, declaring 120 fps. */
private fun buildSps(shape: SpsShape = SpsShape()): BuiltSps {
    val bits = BitWriter()
    val maxSubLayersMinus1 = 2
    val log2PocLsb = 8
    bits.u(4, 0)
    bits.u(3, maxSubLayersMinus1.toLong())
    bits.flag(true)
    // The profile, tier and level: nonzero, so that a wrong skip cannot read as valid.
    repeat(88 + 8) { bits.flag(true) }
    bits.flag(true) // Sub-layer 0: profile and level.
    bits.flag(true)
    bits.flag(false) // Sub-layer 1: neither.
    bits.flag(false)
    repeat(8 - maxSubLayersMinus1) { bits.u(2, 0) }
    repeat(88 + 8) { bits.flag(true) }
    bits.ue(0)
    bits.ue(3) // chroma_format_idc, which adds separate_colour_plane_flag.
    bits.flag(false)
    bits.ue(3840)
    bits.ue(2176)
    bits.flag(true) // Conformance window.
    repeat(3) { bits.ue(0) }
    bits.ue(8)
    repeat(2) { bits.ue(0) }
    bits.ue((log2PocLsb - 4).toLong())
    bits.flag(true) // Ordering info for each sub-layer.
    repeat(maxSubLayersMinus1 + 1) {
        bits.ue(4)
        bits.ue(0)
        bits.ue(0)
    }
    repeat(6) { bits.ue(1) }
    bits.flag(shape.scalingListData)
    if (shape.scalingListData) {
        bits.flag(true)
    }
    repeat(2) { bits.flag(true) }
    bits.flag(true) // PCM.
    bits.u(8, 0x77)
    repeat(2) { bits.ue(1) }
    bits.flag(false)
    bits.ue(2) // Two short-term reference picture sets.
    bits.ue(1) // The first: one negative and one positive picture.
    bits.ue(1)
    repeat(2) {
        bits.ue(0)
        bits.flag(true)
    }
    bits.flag(shape.predictedRefPicSet)
    if (!shape.predictedRefPicSet) {
        bits.ue(0)
        bits.ue(0)
    }
    bits.flag(true) // Long-term reference pictures.
    bits.ue(2)
    repeat(2) {
        bits.u(log2PocLsb, 1)
        bits.flag(true)
    }
    repeat(2) { bits.flag(true) }
    bits.flag(shape.hasVui)
    var timingFields = -1
    if (shape.hasVui) {
        bits.flag(true) // Aspect ratio, extended.
        bits.u(8, 255)
        bits.u(32, 0x00010001)
        bits.flag(true) // Overscan.
        bits.flag(true)
        bits.flag(true) // Video signal type with a colour description.
        bits.u(4, 5)
        bits.flag(true)
        bits.u(24, 0x010101)
        bits.flag(true) // Chroma location.
        repeat(2) { bits.ue(0) }
        repeat(3) { bits.flag(false) }
        bits.flag(true) // Default display window.
        repeat(4) { bits.ue(0) }
        bits.flag(shape.hasTiming)
        if (shape.hasTiming) {
            timingFields = bits.position
            bits.u(32, 1)
            bits.u(32, 120)
            bits.flag(false)
            bits.flag(false)
        }
        bits.flag(false)
    }
    bits.flag(false)
    return BuiltSps(hex("4201") + escaped(bits.finish()), timingFields)
}

private fun theRateNvencDeclaresIsReplaced() {
    val rewritten = rewrite(frameOf(VPS, SPS, PPS, SLICE), 60)
    check(rewritten != null && rewritten.contentEquals(frameOf(VPS, SPS_AT_60, PPS, SLICE)), "only the rate changes in a frame from NVENC")
}

private fun aNewRateThatLooksLikeAStartCodeIsEscaped() {
    val rewritten = rewrite(frameOf(VPS, SPS, PPS, SLICE), 2)
    check(rewritten != null && rewritten.contentEquals(frameOf(VPS, SPS_AT_2, PPS, SLICE)), "the frame grows by the escape the new rate needs")
}

private fun shortStartCodesAreFound() {
    val rewritten = rewrite(frameOf(VPS, SPS, PPS, SLICE, startCode = SHORT_START_CODE), 60)
    check(rewritten != null && rewritten.contentEquals(frameOf(VPS, SPS_AT_60, PPS, SLICE, startCode = SHORT_START_CODE)), "three-byte start codes")
}

private fun onlyTheFramesBytesCount() {
    val frame = frameOf(VPS, SPS, PPS, SLICE)
    val buffer = frame + ByteArray(64) { 0x55 }
    val rewritten = withDeclaredHevcFrameRate(buffer, frame.size, 60)
    check(rewritten != null && rewritten.contentEquals(frameOf(VPS, SPS_AT_60, PPS, SLICE)), "what follows the frame in its buffer stays out")
}

private fun theRateIsFoundPastEveryOptionalField() {
    val sps = buildSps()
    val rewritten = rewrite(frameOf(sps.nal, SLICE), 60)
    check(rewritten != null, "an SPS with every optional field can be rewritten")
    if (rewritten != null) {
        val end = rewritten.size - SLICE.size - START_CODE.size
        val rbsp = unescaped(rewritten.copyOfRange(START_CODE.size + 2, end))
        val before = unescaped(sps.nal.copyOfRange(2, sps.nal.size))
        check(readU32(rbsp, sps.timingFields) == 1L, "num_units_in_tick")
        check(readU32(rbsp, sps.timingFields + 32) == 60L, "time_scale")
        check(rbsp.size == before.size, "the payload keeps its size")
        check(rbsp.copyOf(sps.timingFields / 8).contentEquals(before.copyOf(sps.timingFields / 8)), "what comes before the timing stays the same")
    }
}

private fun framesThatDeclareNoRateAreLeftAlone() {
    check(rewrite(frameOf(SLICE), 60) == null, "a frame without an SPS")
    check(rewrite(frameOf(SLICE, SPS), 60) == null, "an SPS after a slice is not this frame's")
    check(rewrite(ByteArray(0), 60) == null, "an empty frame")
    check(rewrite(frameOf(buildSps(SpsShape(hasVui = false)).nal, SLICE), 60) == null, "an SPS without VUI")
    check(rewrite(frameOf(buildSps(SpsShape(hasTiming = false)).nal, SLICE), 60) == null, "VUI without timing")
}

private fun whatCannotBeReadIsLeftAlone() {
    check(rewrite(frameOf(buildSps(SpsShape(scalingListData = true)).nal, SLICE), 60) == null, "scaling list data")
    check(rewrite(frameOf(buildSps(SpsShape(predictedRefPicSet = true)).nal, SLICE), 60) == null, "a predicted reference picture set")
    check(rewrite(frameOf(SPS.copyOf(SPS.size / 2)), 60) == null, "an SPS cut in half")
    check(rewrite(frameOf(SPS.copyOf(SPS.size - 6)), 60) == null, "an SPS that ends inside the timing")
}

fun main() {
    theRateNvencDeclaresIsReplaced()
    aNewRateThatLooksLikeAStartCodeIsEscaped()
    shortStartCodesAreFound()
    onlyTheFramesBytesCount()
    theRateIsFoundPastEveryOptionalField()
    framesThatDeclareNoRateAreLeftAlone()
    whatCannotBeReadIsLeftAlone()
    if (failures > 0) {
        println("$failures SPS timing checks failed")
        exitProcess(1)
    }
    println("SPS timing tests passed")
}

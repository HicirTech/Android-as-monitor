package dev.androidusbmonitor

import java.io.ByteArrayOutputStream

private const val NAL_FIRST_NON_VCL = 32
private const val NAL_SPS = 33
private const val NAL_HEADER_BYTES = 2
private const val PROFILE_BITS = 88
private const val LEVEL_BITS = 8
private const val COLOUR_DESCRIPTION_BITS = 24
private const val SAMPLE_ASPECT_RATIO_BITS = 32
private const val ASPECT_RATIO_EXTENDED = 255L
private const val MIN_LOG2_POC_LSB = 4
private const val U32_BITS = 32

/**
 * The frame with its SPS changed to declare [fps], or null when it has no SPS, or one that declares no rate or that this cannot read.
 * Only the two timing fields change.
 */
fun withDeclaredHevcFrameRate(frame: ByteArray, length: Int, fps: Int): ByteArray? {
    val sps = findSps(frame, length) ?: return null
    val payloadStart = sps.first + NAL_HEADER_BYTES
    val payloadEnd = sps.last + 1
    val rbsp = unescape(frame, payloadStart, payloadEnd)
    val timingFields = runCatching { timingFieldsPosition(rbsp) }.getOrNull() ?: return null
    overwriteU32(rbsp, timingFields, 1)
    overwriteU32(rbsp, timingFields + U32_BITS, fps)
    val payload = escape(rbsp)

    val rewritten = ByteArray(payloadStart + payload.size + length - payloadEnd)
    frame.copyInto(rewritten, 0, 0, payloadStart)
    payload.copyInto(rewritten, payloadStart)
    frame.copyInto(rewritten, payloadStart + payload.size, payloadEnd, length)
    return rewritten
}

/** The bytes of the SPS NAL unit, which if present comes before the frame's first slice. */
private fun findSps(frame: ByteArray, length: Int): IntRange? {
    var start = nalAfterStartCode(frame, 0, length)
    while (start < length) {
        val type = (frame[start].toInt() shr 1) and 0x3F
        if (type < NAL_FIRST_NON_VCL) {
            return null
        }
        val end = nalEnd(frame, start, length)
        if (type == NAL_SPS) {
            return start until end
        }
        start = nalAfterStartCode(frame, end, length)
    }
    return null
}

private fun nalAfterStartCode(frame: ByteArray, from: Int, length: Int): Int {
    for (index in from until length - 2) {
        if (frame[index].toInt() == 0 && frame[index + 1].toInt() == 0 && frame[index + 2].toInt() == 1) {
            return index + 3
        }
    }
    return length
}

private fun nalEnd(frame: ByteArray, start: Int, length: Int): Int {
    for (index in start until length - 2) {
        if (frame[index].toInt() == 0 && frame[index + 1].toInt() == 0 && frame[index + 2].toInt() in 0..1) {
            return index
        }
    }
    return length
}

private fun unescape(nal: ByteArray, start: Int, end: Int): ByteArray {
    val out = ByteArrayOutputStream(end - start)
    var zeros = 0
    for (index in start until end) {
        val byte = nal[index].toInt()
        if (zeros >= 2 && byte == 3) {
            zeros = 0
            continue
        }
        out.write(byte)
        zeros = if (byte == 0) zeros + 1 else 0
    }
    return out.toByteArray()
}

private fun escape(rbsp: ByteArray): ByteArray {
    val out = ByteArrayOutputStream(rbsp.size + rbsp.size / 16)
    var zeros = 0
    for (value in rbsp) {
        val byte = value.toInt() and 0xFF
        if (zeros >= 2 && byte <= 3) {
            out.write(3)
            zeros = 0
        }
        out.write(byte)
        zeros = if (byte == 0) zeros + 1 else 0
    }
    return out.toByteArray()
}

private fun overwriteU32(data: ByteArray, position: Int, value: Int) {
    for (bit in 0 until U32_BITS) {
        val index = (position + bit) shr 3
        val mask = 0x80 shr ((position + bit) and 7)
        val old = data[index].toInt()
        val isSet = (value ushr (U32_BITS - 1 - bit)) and 1 == 1
        data[index] = (if (isSet) old or mask else old and mask.inv()).toByte()
    }
}

private class BitReader(private val data: ByteArray) {
    var position = 0
        private set

    fun read(count: Int): Long {
        var value = 0L
        repeat(count) {
            val bit = (data[position shr 3].toInt() shr (7 - (position and 7))) and 1
            value = (value shl 1) or bit.toLong()
            position++
        }
        return value
    }

    fun flag() = read(1) == 1L

    fun skip(count: Int) {
        position += count
    }

    fun readUe(): Long {
        var zeros = 0
        while (!flag()) {
            zeros++
        }
        return (1L shl zeros) - 1 + read(zeros)
    }

    fun skipUe(count: Int) = repeat(count) { readUe() }
}

/**
 * Where num_units_in_tick and time_scale start in an SPS payload, or null when it declares no rate or uses what this does not read.
 * Follows seq_parameter_set_rbsp() and vui_parameters() of H.265. No encoder here writes scaling lists or predicted reference sets.
 * Throws when the payload ends early.
 */
private fun timingFieldsPosition(rbsp: ByteArray): Int? {
    val bits = BitReader(rbsp)
    bits.skip(4) // sps_video_parameter_set_id
    val maxSubLayersMinus1 = bits.read(3).toInt()
    bits.skip(1) // sps_temporal_id_nesting_flag
    skipProfileTierLevel(bits, maxSubLayersMinus1)
    bits.skipUe(1) // sps_seq_parameter_set_id
    if (bits.readUe() == 3L) { // chroma_format_idc
        bits.skip(1) // separate_colour_plane_flag
    }
    bits.skipUe(2) // pic_width_in_luma_samples, pic_height_in_luma_samples
    if (bits.flag()) { // conformance_window_flag
        bits.skipUe(4)
    }
    bits.skipUe(2) // bit_depth_luma_minus8, bit_depth_chroma_minus8
    val log2PocLsb = bits.readUe().toInt() + MIN_LOG2_POC_LSB
    val orderingInfoForEachSubLayer = bits.flag()
    repeat(if (orderingInfoForEachSubLayer) maxSubLayersMinus1 + 1 else 1) {
        bits.skipUe(3) // max_dec_pic_buffering_minus1, max_num_reorder_pics, max_latency_increase_plus1
    }
    bits.skipUe(6) // coding block sizes, transform block sizes, transform hierarchy depths
    if (bits.flag() && bits.flag()) { // scaling_list_enabled_flag, sps_scaling_list_data_present_flag
        return null
    }
    bits.skip(2) // amp_enabled_flag, sample_adaptive_offset_enabled_flag
    if (bits.flag()) { // pcm_enabled_flag
        bits.skip(8) // pcm_sample_bit_depth_luma_minus1, pcm_sample_bit_depth_chroma_minus1
        bits.skipUe(2) // log2_min_pcm_luma_coding_block_size_minus3, log2_diff_max_min_pcm_luma_coding_block_size
        bits.skip(1) // pcm_loop_filter_disabled_flag
    }
    val shortTermRefPicSets = bits.readUe().toInt()
    for (set in 0 until shortTermRefPicSets) {
        if (set != 0 && bits.flag()) { // inter_ref_pic_set_prediction_flag
            return null
        }
        val pictures = bits.readUe().toInt() + bits.readUe().toInt() // num_negative_pics, num_positive_pics
        repeat(pictures) {
            bits.skipUe(1) // delta_poc_minus1
            bits.skip(1) // used_by_curr_pic_flag
        }
    }
    if (bits.flag()) { // long_term_ref_pics_present_flag
        repeat(bits.readUe().toInt()) {
            bits.skip(log2PocLsb + 1) // lt_ref_pic_poc_lsb_sps, used_by_curr_pic_lt_sps_flag
        }
    }
    bits.skip(2) // sps_temporal_mvp_enabled_flag, strong_intra_smoothing_enabled_flag
    if (!bits.flag()) { // vui_parameters_present_flag
        return null
    }
    if (bits.flag() && bits.read(LEVEL_BITS) == ASPECT_RATIO_EXTENDED) { // aspect_ratio_info_present_flag, aspect_ratio_idc
        bits.skip(SAMPLE_ASPECT_RATIO_BITS)
    }
    if (bits.flag()) { // overscan_info_present_flag
        bits.skip(1) // overscan_appropriate_flag
    }
    if (bits.flag()) { // video_signal_type_present_flag
        bits.skip(4) // video_format, video_full_range_flag
        if (bits.flag()) { // colour_description_present_flag
            bits.skip(COLOUR_DESCRIPTION_BITS)
        }
    }
    if (bits.flag()) { // chroma_loc_info_present_flag
        bits.skipUe(2)
    }
    bits.skip(3) // neutral_chroma_indication_flag, field_seq_flag, frame_field_info_present_flag
    if (bits.flag()) { // default_display_window_flag
        bits.skipUe(4)
    }
    if (!bits.flag()) { // vui_timing_info_present_flag
        return null
    }
    val position = bits.position
    bits.read(U32_BITS)
    bits.read(U32_BITS)
    return position
}

private fun skipProfileTierLevel(bits: BitReader, maxSubLayersMinus1: Int) {
    bits.skip(PROFILE_BITS + LEVEL_BITS)
    val present = List(maxSubLayersMinus1) { bits.flag() to bits.flag() }
    if (maxSubLayersMinus1 > 0) {
        bits.skip(2 * (8 - maxSubLayersMinus1)) // reserved_zero_2bits
    }
    for ((profilePresent, levelPresent) in present) {
        if (profilePresent) {
            bits.skip(PROFILE_BITS)
        }
        if (levelPresent) {
            bits.skip(LEVEL_BITS)
        }
    }
}

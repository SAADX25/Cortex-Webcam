package com.cortex.sender.transport

import java.nio.ByteBuffer
import kotlin.random.Random

/** RFC 6184, packetization-mode=1. No Annex-B prefixes on the wire. */
class H264Rtp(
    private val maxPacketBytes: Int = 1200,
    private val ssrc: Int = Random.nextInt(),
    private var sequence: Int = Random.nextInt(65536),
) {
    private val timestampBase = Random.nextInt().toLong() and 0xffffffffL
    private var firstPts: Long? = null

    init { require(maxPacketBytes in 64..1400) }

    fun packetize(nals: List<ByteArray>, ptsUs: Long, emit: (ByteArray) -> Unit) {
        packetize(nals, ptsUs, null, emit)
    }
    /** RFC 8285 one-byte header extension, local id 1: clockwise quarter turns. */
    fun packetize(nals: List<ByteArray>, ptsUs: Long, rotation: Int?, emit: (ByteArray) -> Unit) {
        require(rotation == null || rotation in listOf(0,90,180,270))
        val headerBytes = if (rotation == null) 12 else 20
        if (firstPts == null) firstPts = ptsUs
        val timestamp = (timestampBase + (ptsUs - firstPts!!) * 90L / 1000L) and 0xffffffffL
        nals.forEachIndexed { nalIndex, nal ->
            require(nal.isNotEmpty())
            val lastNal = nalIndex == nals.lastIndex
            if (nal.size <= maxPacketBytes - headerBytes) {
                emit(header(nal.size, timestamp, lastNal, rotation).apply { put(nal) }.array())
            } else {
                val indicator = (nal[0].toInt() and 0xe0) or 28
                val type = nal[0].toInt() and 0x1f
                var offset = 1
                while (offset < nal.size) {
                    val size = minOf(maxPacketBytes - headerBytes - 2, nal.size - offset)
                    val end = offset + size == nal.size
                    val fu = type or (if (offset == 1) 0x80 else 0) or (if (end) 0x40 else 0)
                    emit(header(size + 2, timestamp, lastNal && end, rotation).apply {
                        put(indicator.toByte()); put(fu.toByte()); put(nal, offset, size)
                    }.array())
                    offset += size
                }
            }
        }
    }

    private fun header(payload: Int, timestamp: Long, marker: Boolean, rotation: Int?): ByteBuffer =
        ByteBuffer.allocate((if (rotation == null) 12 else 20) + payload).apply {
            put((if (rotation == null) 0x80 else 0x90).toByte()); put((96 or if (marker) 0x80 else 0).toByte())
            putShort((sequence++ and 0xffff).toShort()); putInt(timestamp.toInt()); putInt(ssrc)
            if (rotation != null) { putShort(0xbede.toShort()); putShort(1); put(0x10); put((rotation/90).toByte()); put(0); put(0) }
        }
}

object H264Nals {
    /** MediaCodec AVC output: Annex-B, four-byte AVCC lengths, or a single raw NAL. */
    fun split(data: ByteArray): List<ByteArray> {
        if (data.isEmpty()) return emptyList()
        // Check the entire length-prefixed structure first: AVCC lengths such as
        // 256 (00 00 01 00) can otherwise be mistaken for Annex-B start codes.
        val avcc = mutableListOf<ByteArray>()
        var cursor = 0
        while (cursor + 4 <= data.size) {
            val length = ByteBuffer.wrap(data, cursor, 4).int
            if (length <= 0 || length > data.size - cursor - 4) break
            val header = data[cursor + 4].toInt() and 255
            if (header and 0x80 != 0 || header and 31 !in 1..23) break
            avcc += data.copyOfRange(cursor + 4, cursor + 4 + length)
            cursor += 4 + length
        }
        if (cursor == data.size && avcc.isNotEmpty()) return avcc
        fun prefix(i: Int): Int = when {
            i + 3 <= data.size && data[i] == 0.toByte() && data[i + 1] == 0.toByte() && data[i + 2] == 1.toByte() -> 3
            i + 4 <= data.size && data[i] == 0.toByte() && data[i + 1] == 0.toByte() && data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte() -> 4
            else -> 0
        }
        if (prefix(0) > 0) {
            val result = mutableListOf<ByteArray>()
            var start = prefix(0)
            var i = start
            while (i < data.size) {
                val p = prefix(i)
                if (p > 0) {
                    if (i > start) result += data.copyOfRange(start, i)
                    start = i + p; i = start
                } else i++
            }
            if (start < data.size) result += data.copyOfRange(start, data.size)
            return result
        }
        return listOf(data)
    }
}

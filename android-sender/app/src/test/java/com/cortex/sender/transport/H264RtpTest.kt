package com.cortex.sender.transport

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer

class H264RtpTest {
    @Test fun carriesQuarterTurnWithoutChangingH264AndRespectsMtu() {
        for(rotation in listOf(0,90,180,270)) {
            val nal=ByteArray(4096){0x55}.apply{this[0]=0x65}
            val packets=mutableListOf<ByteArray>()
            H264Rtp().packetize(listOf(nal),100_000,rotation,packets::add)
            assertTrue(packets.all{it.size<=1200 && it[0].toInt() and 0x10 != 0})
            assertTrue(packets.all{(it[12].toInt() and 255)==0xbe && (it[13].toInt() and 255)==0xde && it[16].toInt()==0x10 && it[17].toInt()==rotation/90})
            val restored=byteArrayOf(((packets[0][20].toInt() and 0xe0) or (packets[0][21].toInt() and 31)).toByte())+
                packets.fold(byteArrayOf()){bytes,packet->bytes+packet.copyOfRange(22,packet.size)}
            assertArrayEquals(nal,restored)
            assertTrue(packets.dropLast(1).all{it[1].toInt() and 128==0})
            assertEquals(128,packets.last()[1].toInt() and 128)
        }
    }
    @Test fun fuAReassemblesAndMarksOnlyLastFragment() {
        val nal = ByteArray(4096) { (it % 251).toByte() }.apply { this[0] = 0x65 }
        val packets = mutableListOf<ByteArray>()
        H264Rtp(1200, 123, 65535).packetize(listOf(nal), 100_000, packets::add)
        assertTrue(packets.all { it.size <= 1200 })
        assertEquals(65535, ByteBuffer.wrap(packets[0], 2, 2).short.toInt() and 65535)
        assertEquals(0, ByteBuffer.wrap(packets[1], 2, 2).short.toInt() and 65535)
        assertEquals(0x80, packets.first()[13].toInt() and 0x80)
        assertEquals(0x40, packets.last()[13].toInt() and 0x40)
        assertTrue(packets.dropLast(1).all { it[1].toInt() and 0x80 == 0 })
        assertEquals(0x80, packets.last()[1].toInt() and 0x80)
        val restored = byteArrayOf(((packets[0][12].toInt() and 0xe0) or
            (packets[0][13].toInt() and 0x1f)).toByte()) +
            packets.fold(byteArrayOf()) { acc, packet -> acc + packet.copyOfRange(14, packet.size) }
        assertArrayEquals(nal, restored)
    }

    @Test fun accessUnitSharesTimestampAndAdvancesAt90Khz() {
        val rtp = H264Rtp()
        val first = mutableListOf<ByteArray>()
        rtp.packetize(listOf(byteArrayOf(0x67, 1), byteArrayOf(0x68, 2), byteArrayOf(0x65, 3)), 1_000_000, first::add)
        fun timestamp(packet: ByteArray) = ByteBuffer.wrap(packet, 4, 4).int.toLong() and 0xffffffffL
        assertEquals(1, first.map(::timestamp).distinct().size)
        assertTrue(first.dropLast(1).all { it[1].toInt() and 0x80 == 0 })
        val second = mutableListOf<ByteArray>()
        rtp.packetize(listOf(byteArrayOf(0x41, 4)), 1_100_000, second::add)
        assertEquals(9000L, (timestamp(second.single()) - timestamp(first.first())) and 0xffffffffL)
    }

    @Test fun parsesAnnexBAndAvccWithoutSendingLengthPrefixes() {
        val a = byteArrayOf(0x67, 0x42, 0x80.toByte())
        val b = byteArrayOf(0x68, 0x11)
        val annexB = byteArrayOf(0, 0, 0, 1) + a + byteArrayOf(0, 0, 1) + b
        val avcc = ByteBuffer.allocate(4 + a.size + 4 + b.size).putInt(a.size).put(a).putInt(b.size).put(b).array()
        for (input in listOf(annexB, avcc)) {
            val nals = H264Nals.split(input)
            assertEquals(2, nals.size)
            assertArrayEquals(a, nals[0]); assertArrayEquals(b, nals[1])
        }
    }

    @Test fun avccLengthThatLooksLikeStartCodeIsNotAnnexB() {
        val nal = ByteArray(256) { 0x55 }.apply { this[0] = 0x65 }
        val avcc = ByteBuffer.allocate(260).putInt(nal.size).put(nal).array()
        assertArrayEquals(nal, H264Nals.split(avcc).single())
    }
}

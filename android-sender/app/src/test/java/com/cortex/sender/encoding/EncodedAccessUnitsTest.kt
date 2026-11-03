package com.cortex.sender.encoding
import org.junit.Assert.*
import org.junit.Test
class EncodedAccessUnitsTest {
    @Test fun joinsPartialFrameBeforeRtpPacketization() {
        val frames=mutableListOf<ByteArray>();var discarded=0
        val join=EncodedAccessUnits({bytes,_,_->frames+=bytes},{discarded++})
        join.offer(byteArrayOf(0,0),100,false,true)
        join.offer(byteArrayOf(0,1,0x65),100,false,true)
        assertTrue(frames.isEmpty())
        join.offer(byteArrayOf(1,2,3),100,false,false)
        assertArrayEquals(byteArrayOf(0,0,0,1,0x65,1,2,3),frames.single())
        assertEquals(0,discarded)
    }
    @Test fun boundsDamagedPartialOutputsAndRecoversAtNextFrame() {
        val frames=mutableListOf<ByteArray>();var dropped=0
        val join=EncodedAccessUnits({b,_,_->frames+=b},{dropped++},8)
        join.offer(ByteArray(6),100,false,true);join.offer(ByteArray(6),100,false,false)
        assertEquals(1,dropped);assertTrue(frames.isEmpty())
        join.offer(byteArrayOf(0x65,1),200,false,false)
        assertEquals(1,frames.size)
        join.offer(byteArrayOf(1),300,false,true);join.offer(byteArrayOf(0x65),400,false,false)
        assertEquals(2,dropped)
    }
}

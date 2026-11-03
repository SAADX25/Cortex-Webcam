package com.cortex.sender.discovery
import org.junit.Assert.*
import org.junit.Test

class HandshakeProtocolTest {
    private val nonce = "0123456789abcdef0123456789abcdef"
    @Test fun synchronizesFrameRateWithAcknowledgement() {
        assertEquals("CORTEX/2 HELLO $nonce H264 1920 1080 30 0\n", HandshakeProtocol.syncedHello(nonce, 30, 0))
        val response = HandshakeProtocol.syncedReady("CORTEX/2 READY $nonce 5004 H264 1920 1080 60 41", nonce)
        assertEquals(5004, response.port)
        assertEquals(60, response.fps)
        assertEquals(41L, response.commandId)
        assertEquals("CORTEX/2 HELLO $nonce H264 1920 1080 30 41\n", HandshakeProtocol.syncedHello(nonce, 30, response.commandId))
        assertEquals(30, HandshakeProtocol.syncedReady("CORTEX/2 READY $nonce 5004 H264 1920 1080 30 41", nonce).fps)
    }
    @Test fun rejectsInvalidOrUnrelatedFrameRateCommands() {
        listOf(
            "CORTEX/2 READY ${"f".repeat(32)} 5004 H264 1920 1080 60 1",
            "CORTEX/2 READY $nonce 5004 H264 1920 1080 120 1",
            "CORTEX/2 READY $nonce 0 H264 1920 1080 60 1",
            "CORTEX/2 READY $nonce 5004 H264 1920 1080 60 -1",
            "CORTEX/2 READY $nonce 5004 H264 1920 1080 60 9223372036854775808",
            "CORTEX/2 READY $nonce 5004 H264 1920 1080 60 1 extra"
        ).forEach { assertThrows(IllegalArgumentException::class.java) { HandshakeProtocol.syncedReady(it, nonce) } }
    }
    @Test fun matchesPcWireContract() {
        assertEquals("CORTEX/1 HELLO $nonce H264 1920 1080 30\n", HandshakeProtocol.hello(nonce, 30))
        assertEquals(5004, HandshakeProtocol.ready("CORTEX/1 READY $nonce 5004 H264 1920 1080 30\n", nonce, 30))
    }
    @Test fun rejectsWrongNonceSettingsBusyAndInvalidPort() {
        val invalid = listOf(
            "CORTEX/1 READY ${"0".repeat(32)} 5004 H264 1920 1080 30",
            "CORTEX/1 READY $nonce 5004 H264 1920 1080 60",
            "CORTEX/1 READY $nonce 0 H264 1920 1080 30",
            "CORTEX/1 BUSY $nonce",
            "CORTEX/1 READY $nonce 5004 H264 1920 1080 30 extra",
        )
        invalid.forEach { message ->
            assertThrows(IllegalArgumentException::class.java) { HandshakeProtocol.ready(message, nonce, 30) }
        }
    }
    @Test fun validatesReceiverFeedbackAndRejectsUnrelatedReplies() {
        assertEquals(90L to 1800L, HandshakeProtocol.video("CORTEX/1 VIDEO $nonce 90 1800", nonce))
        listOf("CORTEX/1 VIDEO ${"f".repeat(32)} 90 1800", "CORTEX/1 VIDEO $nonce -1 0", "CORTEX/1 VIDEO $nonce 1 2 extra").forEach {
            assertThrows(IllegalArgumentException::class.java) { HandshakeProtocol.video(it, nonce) }
        }
    }
}

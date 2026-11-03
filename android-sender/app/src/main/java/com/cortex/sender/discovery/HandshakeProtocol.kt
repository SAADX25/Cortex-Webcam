package com.cortex.sender.discovery

object HandshakeProtocol {
    private val noncePattern = Regex("[0-9a-f]{32}")
    data class SyncedReady(val port: Int, val fps: Int, val commandId: Long)
    fun syncedHello(nonce: String, fps: Int, acknowledged: Long, device: String = ""): String {
        require(noncePattern.matches(nonce) && fps in listOf(30, 60) && acknowledged >= 0)
        val label = device.filter { it.code in 32..126 }.take(64)
        val metadata = if (label.isEmpty()) "" else " device=" + label.toByteArray().joinToString("") { "%02x".format(it) }
        return "CORTEX/2 HELLO $nonce H264 1920 1080 $fps $acknowledged$metadata\n"
    }
    fun syncedReady(message: String, nonce: String): SyncedReady {
        require(message.length <= 256 && noncePattern.matches(nonce))
        val fields = message.trim().split(Regex("\\s+"))
        require(fields.size == 9 && fields[0] == "CORTEX/2" && fields[1] == "READY" && fields[2] == nonce &&
            fields[4] == "H264" && fields[5] == "1920" && fields[6] == "1080")
        val port = fields[3].toInt().also { require(it in 1..65535) }
        val rate = fields[7].toInt().also { require(it == 30 || it == 60) }
        require(fields[8].matches(Regex("[0-9]{1,19}")))
        val command = fields[8].toLong().also { require(it >= 0) }
        return SyncedReady(port, rate, command)
    }
    fun video(message: String, nonce: String): Pair<Long, Long> {
        require(message.length <= 256 && noncePattern.matches(nonce))
        val fields = message.trim().split(Regex("\\s+"))
        require(fields.size == 5 && fields[0] == "CORTEX/1" && fields[1] == "VIDEO" && fields[2] == nonce)
        return Pair(fields[3].toLong().also { require(it >= 0) }, fields[4].toLong().also { require(it >= 0) })
    }
    fun hello(nonce: String, fps: Int): String {
        require(noncePattern.matches(nonce) && fps in listOf(30, 60))
        return "CORTEX/1 HELLO $nonce H264 1920 1080 $fps\n"
    }
    fun ready(message: String, nonce: String, fps: Int): Int {
        require(message.length <= 256)
        val fields = message.trim().split(Regex("\\s+"))
        require(fields.size == 8 && fields[0] == "CORTEX/1" && fields[1] == "READY" && fields[2] == nonce &&
            fields[4] == "H264" && fields[5] == "1920" && fields[6] == "1080" && fields[7] == fps.toString()) {
            "Studio is unavailable, busy, or returned incompatible settings"
        }
        return fields[3].toInt().also { require(it in 1..65535) }
    }
}

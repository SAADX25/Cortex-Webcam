package com.cortex.sender.core
import android.net.Network

data class StreamConfig(
    val host: String,
    val port: Int = 5004,
    val fps: Int = 30,
    val bitrate: Int = if (fps == 60) 16_000_000 else 12_000_000,
    val preferCbr: Boolean = true,
    val network: Network? = null,
) {
    val width = 1920
    val height = 1080
    init {
        require(host.isNotBlank()) { "Enter the PC address" }
        require(port in 1..65535)
        require(fps == 30 || fps == 60)
        require(bitrate in 1_000_000..50_000_000)
    }
}

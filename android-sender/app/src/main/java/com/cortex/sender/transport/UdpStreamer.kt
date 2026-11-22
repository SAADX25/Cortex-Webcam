package com.cortex.sender.transport
import android.net.Network

import java.io.Closeable
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.LockSupport
import kotlin.concurrent.thread

/** Owns compressed data only. Never receives a raw camera frame. */
class UdpStreamer(
    host: String,
    port: Int,
    private val requestKeyframe: () -> Unit,
    private val onError: (Throwable) -> Unit,
    network: Network? = null,
    private val orientation: () -> Int = { 0 },
) : Closeable {
    private data class AccessUnit(val nals: List<ByteArray>, val ptsUs: Long, val rotation: Int)
    private val queue = ArrayBlockingQueue<AccessUnit>(8)
    private val running = AtomicBoolean(true)
    private val socket = DatagramSocket()
    private var waitingForIdr = true
    private var sps: ByteArray? = null
    private var pps: ByteArray? = null
    private val worker: Thread

    init {
        try {
            network?.bindSocket(socket)
            socket.connect(InetAddress.getByName(host), port)
            socket.sendBufferSize = 2 * 1024 * 1024
            worker = thread(name = "rtp-udp-send") {
                val rtp = H264Rtp()
                val burst = ArrayList<ByteArray>(64)
                try {
                    while (running.get()) {
                        val unit = queue.take()
                        burst.clear()
                        rtp.packetize(unit.nals, unit.ptsUs, unit.rotation) { bytes -> burst.add(bytes) }
                        val paceLargeFrame = burst.size > 16
                        for (i in burst.indices) {
                            val bytes = burst[i]
                            socket.send(DatagramPacket(bytes, bytes.size))
                            if (paceLargeFrame && (i + 1) % 6 == 0) {
                                LockSupport.parkNanos(250_000L)
                            }
                        }
                    }
                } catch (e: Exception) { if (running.getAndSet(false)) onError(e) }
            }
        } catch (e: Exception) { socket.close(); throw e }
    }

    @Synchronized
    fun offer(bytes: ByteArray, ptsUs: Long, configOnly: Boolean) {
        if (!running.get()) return
        val parsed = H264Nals.split(bytes)
        parsed.forEach { nal ->
            when (nal[0].toInt() and 31) { 7 -> sps = nal; 8 -> pps = nal }
        }
        if (configOnly && parsed.none { (it[0].toInt() and 31) in 1..5 }) return
        val idr = parsed.any { it[0].toInt() and 31 == 5 }
        if (waitingForIdr && (!idr || sps == null || pps == null)) return
        val nals = if (idr) listOfNotNull(sps, pps) + parsed.filter {
            val type = it[0].toInt() and 31
            type != 7 && type != 8
        } else parsed
        if (!queue.offer(AccessUnit(nals, ptsUs, orientation()))) {
            queue.clear()
            waitingForIdr = true
            requestKeyframe()
        } else if (idr) waitingForIdr = false
    }

    override fun close() {
        running.set(false)
        socket.close() // Unblocks a send before joining.
        worker.interrupt()
        if (Thread.currentThread() !== worker) worker.join(1500)
        queue.clear()
    }
}

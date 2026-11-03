package com.cortex.sender.discovery

import android.content.Context
import android.net.*
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.cortex.sender.core.StreamConfig
import java.io.Closeable
import java.net.*
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicReference

/** mDNS first; directed LAN broadcast and a remembered, verified PC provide recovery. */
class StudioDiscovery(context: Context,
    private val onReady: (StreamConfig) -> Unit,
    private val onLost: () -> Unit,
    private val onStatus: (String) -> Unit,
    private val onReceiver: (Long, Long) -> Unit = { _, _ -> },
) : Closeable {
    private data class Endpoint(val name: String, val address: InetAddress, val port: Int, val network: Network)
    private val app = context.applicationContext
    private val nsd = app.getSystemService(NsdManager::class.java)
    private val connectivity = app.getSystemService(ConnectivityManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { task -> Thread(task, "studio-link").apply { isDaemon = true } }
    private val socket = AtomicReference<DatagramSocket?>()
    private var running = false
    private var generation = 0
    private var wifi: Network? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null
    private var listener: NsdManager.DiscoveryListener? = null
    private var resolver: NsdManager.ResolveListener? = null
    private val candidates = linkedMapOf<String, NsdServiceInfo>()
    private var selected: Endpoint? = null
    private var selectedVideoPort = 0
    private var lastRemoteFrames: Long? = null
    private var busy = false
    private var failures = 0
    private var cursor = 0
    private var fps = 30
    private var frameRateRevision = 0
    private var acknowledgedCommand = 0L
    private var selectedSync: Boolean? = null
    private var bitrateCbr = true
    private var manualHost = ""
    private var controlPort = 5005
    private var rememberedHost = ""
    private var ticks = 0
    private var resolverStarted = 0L
    private val poll = object : Runnable {
        override fun run() {
            if (!running) return
            ticks++
            val network = wifi
            if (network != null) {
                if (listener == null && manualHost.isBlank()) beginBrowse(network)
                if (!busy) {
                    val endpoint = selected
                    when {
                        endpoint != null -> handshake(endpoint, true)
                        manualHost.isNotBlank() -> findDirect(network, manualHost)
                        ticks % 3 == 1 && rememberedHost.isNotBlank() -> findDirect(network, rememberedHost)
                        candidates.isNotEmpty() && ticks % 2 == 0 -> resolveNext()
                        else -> broadcast(network)
                    }
                } else if (resolver != null && android.os.SystemClock.elapsedRealtime() - resolverStarted > 4000) {
                    cancelResolver()
                }
            }
            main.postDelayed(this, 2000)
        }
    }
    fun setFrameRate(value: Int) {
        require(value == 30 || value == 60)
        main.post { if (fps != value) { fps = value; frameRateRevision++ } }
    }
    fun reconnect() { if (running) start(fps, bitrateCbr, manualHost, controlPort) }
    fun start(frameRate: Int, preferCbr: Boolean, target: String = "", handshakePort: Int = 5005) {
        stop(); running = true; fps = frameRate; bitrateCbr = preferCbr
        manualHost = target; controlPort = handshakePort; ticks = 0
        rememberedHost = app.getSharedPreferences("studio-link", 0).getString("pc", "").orEmpty()
        val token = generation
        onStatus("Searching for Studio on your local Wi-Fi…")
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { main.post {
                if (running && token == generation && wifi == null) {
                    wifi = network
                    if (manualHost.isBlank()) beginBrowse(network)
                    main.removeCallbacks(poll); main.post(poll)
                }
            } }
            override fun onLost(network: Network) { main.post {
                if (running && token == generation && wifi == network) {
                    // Invalidate in-flight socket and DNS callbacks before changing networks.
                    generation++; endBrowse(); wifi = null; disconnect()
                    start(fps, bitrateCbr, manualHost, controlPort)
                    onStatus("Wi-Fi disconnected. Rejoin the same router as your PC.")
                }
            } }
        }
        networkCallback = callback
        try {
            connectivity.registerNetworkCallback(NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build(), callback)
            main.post(poll)
        } catch (e: Exception) { stop(); onStatus("Network access unavailable: ${e.message}") }
    }
    private fun beginBrowse(network: Network) {
        val token = generation
        val discovery = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(type: String) = Unit
            override fun onDiscoveryStopped(type: String) = Unit
            override fun onStartDiscoveryFailed(type: String, error: Int) { main.post {
                if (valid(token, network) && listener === this) {
                    runCatching { nsd.stopServiceDiscovery(this) }; listener = null
                }
            } }
            override fun onStopDiscoveryFailed(type: String, error: Int) = Unit
            override fun onServiceFound(info: NsdServiceInfo) { main.post {
                if (valid(token, network) && info.serviceType.trimEnd('.') == SERVICE_TYPE.trimEnd('.')) {
                    candidates[info.serviceName] = info
                    if (!busy && selected == null) resolveNext()
                }
            } }
            override fun onServiceLost(info: NsdServiceInfo) { main.post {
                if (valid(token, network)) candidates.remove(info.serviceName)
                // DNS expiry alone must not tear down a healthy, heartbeating stream.
            } }
        }
        listener = discovery
        try {
            if (Build.VERSION.SDK_INT >= 33) nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, network, app.mainExecutor, discovery)
            else nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discovery)
        } catch (_: Exception) { listener = null }
    }
    @Suppress("DEPRECATION")
    private fun resolveNext() {
        val network = wifi ?: return
        if (busy || candidates.isEmpty()) return
        val candidate = candidates.values.toList()[(cursor++ and Int.MAX_VALUE) % candidates.size]
        val token = generation; busy = true; resolverStarted = android.os.SystemClock.elapsedRealtime()
        val resolve = object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, error: Int) { main.post {
                if (valid(token, network) && resolver === this) { busy = false; resolver = null }
            } }
            override fun onServiceResolved(info: NsdServiceInfo) { main.post {
                if (!valid(token, network) || resolver !== this) return@post
                busy = false; resolver = null
                val addresses = if (Build.VERSION.SDK_INT >= 34) info.hostAddresses else listOfNotNull(info.host)
                val address = addresses.firstOrNull { it is Inet4Address } ?: return@post
                fun txt(key: String) = info.attributes[key]?.toString(Charsets.UTF_8)
                if (txt("v") == "1" && txt("codec") == "h264" && txt("transport") == "rtp" && info.port in 1..65535)
                    handshake(Endpoint(info.serviceName, address, info.port, network), false)
            } }
        }
        resolver = resolve
        try { nsd.resolveService(candidate, resolve) } catch (_: Exception) { busy = false; resolver = null }
    }
    private fun newSocket(network: Network): DatagramSocket {
        val udp = DatagramSocket(null)
        try {
            socket.set(udp); network.bindSocket(udp); udp.bind(InetSocketAddress(0)); udp.soTimeout = 450
            return udp
        } catch (e: Exception) { udp.close(); throw e }
    }
    private fun findDirect(network: Network, host: String) {
        // Resolution is deliberately off the main thread, including manual addresses.
        busy = true; val token = generation
        worker.execute {
            val address = runCatching { network.getAllByName(host).first { it is Inet4Address } }.getOrNull()
            main.post {
                if (!valid(token, network)) return@post
                busy = false
                if (address != null) handshake(Endpoint("Direct", address, controlPort, network), false)
            }
        }
    }
    private fun broadcast(network: Network) {
        val token = generation; busy = true
        val broadcasts = connectivity.getLinkProperties(network)?.linkAddresses.orEmpty().mapNotNull { link ->
            val address = link.address as? Inet4Address ?: return@mapNotNull null
            if (link.prefixLength !in 1..30) return@mapNotNull null
            val raw = address.address
            val ip = raw.fold(0L) { value, byte -> (value shl 8) or (byte.toLong() and 255) }
            val mask = (0xffffffffL shl (32 - link.prefixLength)) and 0xffffffffL
            val value = ip or (mask xor 0xffffffffL)
            InetAddress.getByAddress(ByteArray(4) { (value shr (24 - it * 8)).toByte() })
        }.toMutableList().apply { add(InetAddress.getByAddress(byteArrayOf(-1,-1,-1,-1))) }
        worker.execute {
            val found = runCatching {
                val nonce = nonce(); val bytes = "CORTEX/1 DISCOVER $nonce\n".toByteArray(Charsets.US_ASCII)
                newSocket(network).use { udp ->
                    udp.broadcast = true
                    broadcasts.distinct().forEach { address -> runCatching { udp.send(DatagramPacket(bytes, bytes.size, address, controlPort)) } }
                    val deadline = android.os.SystemClock.elapsedRealtime() + 700
                    while (android.os.SystemClock.elapsedRealtime() < deadline) {
                        val reply = DatagramPacket(ByteArray(512), 512)
                        try {
                            udp.receive(reply)
                            if (reply.port == controlPort && String(reply.data, 0, reply.length, Charsets.US_ASCII).trim() == "CORTEX/1 OFFER $nonce")
                                return@runCatching Endpoint("LAN discovery", reply.address, reply.port, network)
                        } catch (_: SocketTimeoutException) { }
                    }
                    null
                }
            }.getOrNull()
            socket.getAndSet(null)?.close()
            main.post {
                if (!valid(token, network)) return@post
                busy = false
                if (found != null) handshake(found, false)
                else if (ticks > 3) onStatus("Studio not found. Open Studio on the PC and allow local network access. Same router, no guest Wi-Fi.")
            }
        }
    }
    private fun handshake(endpoint: Endpoint, heartbeat: Boolean) {
        val token = generation; val rate = fps; val rateRevision = frameRateRevision
        val previousAck = acknowledgedCommand; val syncMode = selectedSync; busy = true
        worker.execute {
            var feedback: Pair<Long, Long>? = null
            var synced: HandshakeProtocol.SyncedReady? = null
            val result = runCatching {
                val nonce = nonce()
                newSocket(endpoint.network).use { udp ->
                    udp.connect(endpoint.address, endpoint.port)
                    var videoPort: Int? = null
                    // Legacy PCs stay compatible. Probe v2 only until this endpoint's
                    // capabilities are known; every response is bound to this nonce.
                    val attempts = if (syncMode == false) listOf(false, false) else listOf(true, true, false)
                    for (useSync in attempts) {
                        val request = (if(useSync) HandshakeProtocol.syncedHello(nonce, rate, previousAck, "${Build.MANUFACTURER} ${Build.MODEL}")
                            else HandshakeProtocol.hello(nonce, rate)).toByteArray(Charsets.US_ASCII)
                        udp.send(DatagramPacket(request, request.size))
                        try {
                            val response = DatagramPacket(ByteArray(512), 512); udp.receive(response)
                            val message = String(response.data, 0, response.length, Charsets.US_ASCII)
                            videoPort = if (useSync) HandshakeProtocol.syncedReady(message, nonce).also { synced = it }.port
                                else HandshakeProtocol.ready(message, nonce, rate)
                            break
                        } catch (_: SocketTimeoutException) { }
                    }
                    val port = videoPort ?: error("Studio did not answer. Check PC network access.")
                    if (heartbeat) {
                        val query = "CORTEX/1 STATUS $nonce\n".toByteArray(Charsets.US_ASCII)
                        udp.send(DatagramPacket(query, query.size))
                        runCatching {
                            val reply = DatagramPacket(ByteArray(512), 512); udp.receive(reply)
                            feedback = HandshakeProtocol.video(String(reply.data, 0, reply.length, Charsets.US_ASCII), nonce)
                        }
                    }
                    port
                }
            }
            socket.getAndSet(null)?.close()
            main.post {
                if (!valid(token, endpoint.network)) return@post
                busy = false
                // A local frame-rate change supersedes a response still in flight.
                if (rateRevision != frameRateRevision) return@post
                if (result.isSuccess) {
                    failures = 0
                    val port = result.getOrThrow()
                    val receiverRestarted = feedback?.first?.let { frames -> lastRemoteFrames?.let { frames < it } } == true
                    val effectiveRate = synced?.fps ?: rate
                    selectedSync = synced != null
                    acknowledgedCommand = synced?.commandId ?: 0L
                    if (effectiveRate != fps) { fps = effectiveRate; frameRateRevision++ }
                    if (!heartbeat || port != selectedVideoPort || receiverRestarted || effectiveRate != rate) {
                        if (heartbeat && (port != selectedVideoPort || receiverRestarted)) disconnect()
                        selectedSync = synced != null
                        acknowledgedCommand = synced?.commandId ?: 0L
                        selected = endpoint
                        selectedVideoPort = port
                        app.getSharedPreferences("studio-link", 0).edit().putString("pc", endpoint.address.hostAddress).apply()
                        onStatus("Studio found · ${endpoint.address.hostAddress}")
                        onReady(StreamConfig(checkNotNull(endpoint.address.hostAddress), port, effectiveRate,
                            preferCbr = bitrateCbr, network = endpoint.network))
                    }
                    feedback?.let { lastRemoteFrames = it.first }
                    feedback?.let { onReceiver(it.first, it.second) }
                } else if (heartbeat && ++failures >= 3) {
                    disconnect(); onStatus("Studio disconnected. Reconnecting automatically…")
                } else if (!heartbeat && manualHost.isNotBlank()) onStatus("PC is not answering at $manualHost:$controlPort. Use Allow phone connection in Studio.")
            }
        }
    }
    private fun nonce() = UUID.randomUUID().toString().replace("-", "")
    private fun valid(token: Int, network: Network) = running && token == generation && wifi == network
    private fun disconnect() { val had = selected != null; selected = null; selectedVideoPort = 0; lastRemoteFrames = null; failures = 0; selectedSync = null; acknowledgedCommand = 0; if (had) onLost() }
    private fun cancelResolver() {
        if (Build.VERSION.SDK_INT >= 34) resolver?.let { runCatching { nsd.stopServiceResolution(it) } }
        resolver = null; busy = false
    }
    private fun endBrowse() {
        listener?.let { runCatching { nsd.stopServiceDiscovery(it) } }; listener = null
        cancelResolver(); candidates.clear(); socket.getAndSet(null)?.close()
    }
    fun stop() {
        running = false; generation++; main.removeCallbacks(poll)
        networkCallback?.let { runCatching { connectivity.unregisterNetworkCallback(it) } }; networkCallback = null
        endBrowse(); wifi = null; selected = null; selectedVideoPort = 0; lastRemoteFrames = null; failures = 0; selectedSync = null; acknowledgedCommand = 0
    }
    override fun close() { stop(); worker.shutdownNow() }
    companion object { const val SERVICE_TYPE = "_cortexcam._udp." }
}

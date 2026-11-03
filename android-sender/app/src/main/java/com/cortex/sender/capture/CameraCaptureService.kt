package com.cortex.sender.capture

import android.Manifest
import android.annotation.SuppressLint
import android.app.*
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.hardware.camera2.*
import android.media.MediaCodec
import android.os.*
import android.util.Range
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import android.graphics.SurfaceTexture
import com.cortex.sender.core.StreamConfig
import com.cortex.sender.encoding.HardwareEncoder
import com.cortex.sender.transport.UdpStreamer
import com.cortex.sender.ui.MainActivity
import com.cortex.sender.discovery.StudioDiscovery
import com.cortex.sender.preview.GpuPreview
import com.cortex.sender.core.VideoOrientation

/** All camera/session/encoder ownership transitions are serialized on cameraHandler. */
class CameraCaptureService : Service() {
    inner class LocalBinder : Binder() { val service get() = this@CameraCaptureService }
    private val binder = LocalBinder()
    private val cameraThread = HandlerThread("camera-control")
    private lateinit var cameraHandler: Handler
    private val mainHandler = Handler(Looper.getMainLooper())
    private var camera: CameraDevice? = null
    private var cameraClosing: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: HardwareEncoder? = null
    private var streamer: UdpStreamer? = null
    private var config: StreamConfig? = null
    private var requestedConfig: StreamConfig? = null
    @Volatile var usingFront = false
        private set
    @Volatile var cameraReady = false
        private set
    @Volatile var switchingCamera = false
        private set
    private var pendingReopen: Pair<CameraDevice, () -> Unit>? = null
    private var aeRange = Range(30, 30)
    @Volatile var actualFps = 30
        private set
    @Volatile var receiverFrames = 0L
        private set
    @Volatile var destination = ""
        private set
    private var lastReceiverFrames = -1L
    private var receiverProgressAt = 0L
    private var preview: Surface? = null
    private var gpuPreview: GpuPreview? = null
    private var streamWakeLock: PowerManager.WakeLock? = null
    private var sensorDegrees = 90
    private var displayDegrees = 0
    private var frontFacing = false
    @Volatile var rotationDegrees = 90
        private set
    private var powerSaver = false
    private var generation = 0
    private var sessionGeneration = 0
    @Volatile var status: String = "Stopped"
        private set
    @Volatile var onStatus: ((String) -> Unit)? = null
    private var pendingAutomatic: StreamConfig? = null
    var onAutoReady: ((StreamConfig) -> Unit)? = null
        set(value) {
            field = value
            if (!isActive) pendingAutomatic?.let { value?.invoke(it) }
        }
    @Volatile var isActive = false
        private set
    @Volatile var isForeground = false
        private set
    @Volatile private var automaticStream = false
    @Volatile private var discoveryEnabled = false
    private var recovery: Runnable? = null
    private var consecutiveFailures = 0
    private lateinit var discovery: StudioDiscovery

    override fun onCreate() {
        super.onCreate()
        cameraThread.start()
        cameraHandler = Handler(cameraThread.looper)
        usingFront = getSharedPreferences("camera",0).getBoolean("front",false)
        discovery = StudioDiscovery(this,
            onReady = { settings ->
                pendingAutomatic = settings
                if (!isActive) {
                    if (isForeground) start(settings, automatic = true)
                    else onAutoReady?.invoke(settings)
                } else if (actualFps != settings.fps) {
                    start(settings, automatic = true)
                }
            },
            onLost = {
                pendingAutomatic = null
                if (automaticStream) {
                    isActive = false
                    cameraHandler.post {
                        stopPipeline()
                        updateStatus("Studio disconnected; waiting for discovery")
                    }
                }
            },
            onStatus = { text -> if (!isActive) updateStatus(text) },
            onReceiver = { frames, packets ->
                val now = SystemClock.elapsedRealtime()
                if (lastReceiverFrames >= 0 && frames > lastReceiverFrames) receiverProgressAt = now
                lastReceiverFrames = frames
                receiverFrames = if (receiverProgressAt > 0 && now - receiverProgressAt < 5000) frames else 0
                if (receiverFrames > 0) consecutiveFailures = 0
                if (isActive) updateStatus(if (receiverFrames > 0) "Live · Studio confirmed video · 1080p/$actualFps"
                    else if (packets > 0) "Connected · Studio is waiting for a complete video frame"
                    else "Camera active · Studio has not received video. Check PC network access.")
            })
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel("stream", "Camera streaming", NotificationManager.IMPORTANCE_LOW))
    }
    override fun onBind(intent: Intent): IBinder = binder
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if(intent?.action == "com.cortex.sender.STOP") { stop(); return START_NOT_STICKY }
        val pending = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val notification = Notification.Builder(this, "stream")
            .setContentTitle("Cortex camera streaming").setContentText("Tap to manage the stream")
            .setSmallIcon(android.R.drawable.presence_video_online).setOngoing(true).setContentIntent(pending)
            .addAction(Notification.Action.Builder(null,"Stop streaming",PendingIntent.getService(this,1,
                Intent(this,CameraCaptureService::class.java).setAction("com.cortex.sender.STOP"),PendingIntent.FLAG_IMMUTABLE)).build()).build()
        val type = if (Build.VERSION.SDK_INT >= 30) ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA else 0
        startForeground(1, notification, type)
        isForeground = true
        return START_NOT_STICKY
    }

    fun enableDiscovery(fps: Int, preferCbr: Boolean, target: String = "", controlPort: Int = 5005) {
        cancelRecovery(); consecutiveFailures = 0
        discovery.stop()
        pendingAutomatic = null
        receiverFrames = 0
        lastReceiverFrames = -1; receiverProgressAt = 0
        if (isActive) {
            isActive = false
            cameraHandler.post { stopPipeline() }
        }
        automaticStream = true
        discoveryEnabled = true
        discovery.start(fps, preferCbr, target, controlPort)
    }
    fun disableDiscovery(stopAutomatic: Boolean = true) {
        cancelRecovery()
        discoveryEnabled = false
        pendingAutomatic = null
        discovery.stop()
        if (!stopAutomatic) automaticStream = false
        else if (automaticStream) stop()
    }
    fun start(config: StreamConfig, automatic: Boolean = false) {
        cancelRecovery()
        pendingAutomatic = null
        receiverFrames = 0; lastReceiverFrames = -1; receiverProgressAt = 0
        automaticStream = automatic
        if (!automatic) { discoveryEnabled = false; discovery.stop() }
        cameraHandler.post {
            restartPipeline(config)
        }
    }
    /** Preserve the active Studio lease, including acknowledged remote commands. */
    fun changeFrameRate(value: Int) {
        require(value == 30 || value == 60)
        discovery.setFrameRate(value)
        cameraHandler.post {
            val settings = requestedConfig ?: config ?: return@post
            if (isActive) restartPipeline(settings.copy(fps = value, bitrate = if(value == 60) 16_000_000 else 12_000_000))
        }
    }
    private fun restartPipeline(settings: StreamConfig) {
        val closingCamera = camera ?: cameraClosing
        stopPipeline()
        isActive = true; switchingCamera = closingCamera != null
        val token = generation
        val reopen: () -> Unit = {
            if(token == generation) try { startPipeline(settings) } catch(error: Exception) { fail(error) }
        }
        // Wait for Camera2 to release the old device before opening it again.
        if(closingCamera != null) pendingReopen = closingCamera to reopen else reopen()
    }
    fun stop() {
        cancelRecovery()
        pendingAutomatic = null
        discoveryEnabled = false; discovery.stop(); automaticStream = false
        cameraHandler.post {
            stopPipeline()
            updateStatus("Stopped")
            stopForeground(STOP_FOREGROUND_REMOVE); isForeground = false
            stopSelf()
        }
    }
    fun setPreview(surface: Surface?) = cameraHandler.post {
        if (preview === surface) return@post
        preview = surface
        gpuPreview?.attach(if (!powerSaver) surface else null)
        updateRepeatingRequest()
    }
    fun setPowerSaver(enabled: Boolean) = cameraHandler.post {
        if (powerSaver == enabled) return@post
        powerSaver = enabled
        gpuPreview?.attach(if (enabled) null else preview)
        updateRepeatingRequest()
    }
    fun setDisplayRotation(degrees: Int) = cameraHandler.post {
        displayDegrees = degrees
        rotationDegrees = VideoOrientation.clockwise(sensorDegrees, displayDegrees, frontFacing)
        gpuPreview?.rotate(rotationDegrees, VideoOrientation.previewCorrection(sensorDegrees,rotationDegrees,frontFacing))
    }
    fun flipCamera() = cameraHandler.post {
        if(switchingCamera || (isActive && !cameraReady)) return@post
        val settings = requestedConfig ?: config
        val targetFront = !usingFront
        if(settings == null) {
            usingFront = targetFront
            getSharedPreferences("camera",0).edit().putBoolean("front",usingFront).apply()
            updateStatus(if(usingFront) "Front camera selected" else "Back camera selected")
            return@post
        }
        try { selectCamera(settings,targetFront) } catch(error: Exception) {
            updateStatus("Error: ${error.message}");return@post
        }
        val closingCamera = camera ?: cameraClosing
        stopPipeline()
        usingFront = targetFront
        getSharedPreferences("camera",0).edit().putBoolean("front",usingFront).apply()
        receiverFrames=0;lastReceiverFrames=-1;receiverProgressAt=0
        switchingCamera=true;isActive=true
        val token=generation
        val reopen: () -> Unit = {
            if(token==generation) {
                try { startPipeline(settings) } catch(error: Exception) { fail(error) }
            }
        }
        updateStatus("Switching camera…")
        // Camera callbacks use cameraHandler too, so onClosed runs only after
        // this pending operation is installed. Stop/reconnect cancels it.
        if(closingCamera!=null) pendingReopen=closingCamera to reopen else reopen()
    }

    private fun reconfigure() {
        try { if (camera != null) createSession() } catch (e: Exception) { fail(e) }
    }

    @SuppressLint("MissingPermission", "WakelockTimeout")
    private fun startPipeline(settings: StreamConfig) {
        check(checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) { "Camera permission required" }
        // Intentionally held only for the explicit foreground camera stream;
        // every stop/error/destroy path releases it, including while UI is absent.
        streamWakeLock = getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"Cortex:CameraStream").apply { acquire() }
        val manager = getSystemService(CameraManager::class.java)
        requestedConfig = settings
        val selection=selectCamera(settings,usingFront)
        val id=selection.id;val effective=selection.settings;aeRange=selection.range
        val characteristics = manager.getCameraCharacteristics(id)
        sensorDegrees = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        frontFacing = characteristics.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        rotationDegrees = VideoOrientation.clockwise(sensorDegrees, displayDegrees, frontFacing)
        gpuPreview = GpuPreview(1920,1080).also {
            it.rotate(rotationDegrees,VideoOrientation.previewCorrection(sensorDegrees,rotationDegrees,frontFacing))
            it.attach(if (!powerSaver) preview else null)
        }
        actualFps = effective.fps
        destination = "${effective.host}:${effective.port}"
        discovery.setFrameRate(actualFps)
        config = effective
        val token = generation
        streamer = UdpStreamer(effective.host, effective.port,
            requestKeyframe = { encoder?.requestKeyframe() },
            onError = { error -> cameraHandler.post { if (generation == token) fail(error) } },
            network = effective.network,
            orientation = { rotationDegrees })
        encoder = HardwareEncoder(effective,
            onAccessUnit = { bytes, pts, codecConfig -> streamer?.offer(bytes, pts, codecConfig) },
            onError = { error -> cameraHandler.post { if (generation == token) fail(error) } })
        encoder!!.start()
        updateStatus("Opening camera; ${encoder!!.codecName}")
        manager.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                if (token != generation) { device.close(); return }
                camera = device
                reconfigure()
            }
            override fun onDisconnected(device: CameraDevice) {
                device.close()
                if (token == generation) fail(IllegalStateException("Camera disconnected"))
            }
            override fun onError(device: CameraDevice, error: Int) {
                device.close()
                if (token == generation) fail(IllegalStateException("Camera error $error"))
            }
            override fun onClosed(device: CameraDevice) {
                if(cameraClosing === device) cameraClosing = null
                val pending=pendingReopen
                if(pending?.first===device) { pendingReopen=null;pending.second.invoke() }
            }
        }, cameraHandler)
    }

    private data class Selection(val id: String,val settings: StreamConfig,val range: Range<Int>)
    private fun selectCamera(settings: StreamConfig,front: Boolean): Selection {
        val manager=getSystemService(CameraManager::class.java)
        val wanted=if(front)CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
        val ids=manager.cameraIdList.filter { manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING)==wanted }
        // Select actual capabilities, not a Samsung model-name allowlist. Fall back
        // to 30 if regular-session 1080p60 is unavailable on this camera/firmware.
        for (rate in if (settings.fps == 60) listOf(60, 30) else listOf(30)) {
            val id = ids.firstOrNull { id ->
            val c = manager.getCameraCharacteristics(id)
            val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val duration = runCatching { map?.getOutputMinFrameDuration(MediaCodec::class.java, Size(1920, 1080)) ?: Long.MAX_VALUE }.getOrDefault(Long.MAX_VALUE)
            c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.any { it.upper == rate } == true &&
                map?.getOutputSizes(MediaCodec::class.java)?.contains(Size(1920, 1080)) == true &&
                map.getOutputSizes(SurfaceTexture::class.java)?.contains(Size(1920, 1080)) == true &&
                duration <= 1_000_000_000L / rate + 1
            }
            if (id != null) {
                val range = manager.getCameraCharacteristics(id).get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)!!
                    .filter { it.upper == rate }.maxBy { it.lower }
                return Selection(id,settings.copy(fps=rate),range)
            }
        }
        error("${if(front) "Front" else "Back"} camera does not support 1080p at 30/60 fps")
    }

    @Suppress("DEPRECATION")
    private fun createSession() {
        val device = camera ?: return
        val settings = config ?: return
        val input = encoder?.inputSurface ?: return
        // Both configured surfaces live for the whole stream. UI/background and
        // Power Saver change only request targets, never this session or codec.
        val outputs = listOf(input, checkNotNull(gpuPreview).cameraSurface)
        val token = generation
        val sessionToken = ++sessionGeneration
        session?.close()
        session = null
        device.createCaptureSession(outputs, object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(created: CameraCaptureSession) {
                if (token != generation || sessionToken != sessionGeneration) { created.close(); return }
                session = created
                try {
                    updateRepeatingRequest()
                    if (!isActive) return
                    cameraReady=true;switchingCamera=false
                    encoder?.requestKeyframe()
                    updateStatus("Sending 1080p/${settings.fps} · waiting for Studio confirmation${if (powerSaver) " · Power Saver" else ""}")
                } catch (e: Exception) { fail(e) }
            }
            override fun onConfigureFailed(created: CameraCaptureSession) {
                created.close()
                if (token == generation && sessionToken == sessionGeneration)
                    fail(IllegalStateException("Camera rejected the encoder/preview surface combination"))
            }
        }, cameraHandler)
    }

    private fun updateRepeatingRequest() {
        val device = camera ?: return
        val activeSession = session ?: return
        val input = encoder?.inputSurface ?: return
        try {
            val request = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(input)
                if (!powerSaver && preview?.isValid == true) gpuPreview?.cameraSurface?.let { addTarget(it) }
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, aeRange)
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                if(Build.VERSION.SDK_INT >= 31) set(CaptureRequest.SCALER_ROTATE_AND_CROP, CaptureRequest.SCALER_ROTATE_AND_CROP_NONE)
                val modes = getSystemService(CameraManager::class.java).getCameraCharacteristics(device.id)
                    .get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
                if (modes.contains(CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO))
                    set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
            }.build()
            activeSession.setRepeatingRequest(request, null, cameraHandler)
        } catch (error: Exception) { fail(error) }
    }

    private fun stopPipeline() {
        isActive = false
        cameraReady=false;switchingCamera=false;pendingReopen=null
        generation++; sessionGeneration++
        runCatching { session?.close() }; session = null
        camera?.let { cameraClosing = it }
        runCatching { camera?.close() }; camera = null
        runCatching { gpuPreview?.close() }; gpuPreview = null
        streamWakeLock?.let { if(it.isHeld) it.release() }; streamWakeLock = null
        // Closing transport first unblocks sends; the drain thread then exits before codec release.
        runCatching { streamer?.close() }
        runCatching { encoder?.close() }; encoder = null
        streamer = null
        config = null
        requestedConfig = null
    }
    private fun fail(error: Throwable) {
        stopPipeline()
        mainHandler.post {
            pendingAutomatic = null
            if (automaticStream && discoveryEnabled) {
                cancelRecovery()
                val delay = minOf(15_000L, 1500L shl minOf(consecutiveFailures++, 3))
                recovery = Runnable {
                    recovery = null
                    if (automaticStream && discoveryEnabled) discovery.reconnect()
                }.also { mainHandler.postDelayed(it, delay) }
                updateStatus("Error: ${error.message ?: error.javaClass.simpleName}. Retrying automatically…")
            } else {
                discovery.stop(); discoveryEnabled = false; automaticStream = false
                updateStatus("Error: ${error.message ?: error.javaClass.simpleName}")
            }
        }
        stopForeground(STOP_FOREGROUND_REMOVE)
        isForeground = false
        stopSelf()
    }
    private fun updateStatus(value: String) {
        status = value
        mainHandler.post { onStatus?.invoke(value) }
    }
    private fun cancelRecovery() { recovery?.let { mainHandler.removeCallbacks(it) }; recovery = null }
    override fun onDestroy() {
        cancelRecovery()
        onStatus = null
        onAutoReady = null
        discovery.close()
        cameraHandler.post { stopPipeline(); cameraThread.quitSafely() }
        super.onDestroy()
    }
}

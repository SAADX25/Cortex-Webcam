package com.cortex.sender.encoding

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Bundle
import android.view.Surface
import com.cortex.sender.core.StreamConfig
import java.io.Closeable
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

class HardwareEncoder(
    config: StreamConfig,
    private val onAccessUnit: (ByteArray, Long, Boolean) -> Unit,
    private val onError: (Throwable) -> Unit,
) : Closeable {
    private val codec: MediaCodec
    val inputSurface: Surface
    val codecName: String
    private val running = AtomicBoolean(false)
    private val syncRequested = AtomicBoolean(false)
    private var worker: Thread? = null
    private var started = false

    init {
        val candidates = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter {
            it.isEncoder && it.isHardwareAccelerated && !it.isSoftwareOnly &&
                it.supportedTypes.any { type -> type.equals("video/avc", true) }
        }
        var selected: MediaCodec? = null
        var surface: Surface? = null
        val failures = mutableListOf<String>()
        for (info in candidates) {
            var attempt: MediaCodec? = null
            try {
                val caps = info.getCapabilitiesForType("video/avc")
                require(caps.colorFormats.contains(MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface))
                require(caps.videoCapabilities.areSizeAndRateSupported(config.width, config.height, config.fps.toDouble()))
                require(caps.videoCapabilities.bitrateRange.contains(config.bitrate))
                val profile = listOf(MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline,
                    MediaCodecInfo.CodecProfileLevel.AVCProfileMain).firstOrNull { preferredProfile ->
                    caps.profileLevels.any { it.profile == preferredProfile }
                } ?: error("A GPU-compatible Constrained Baseline or Main profile is required")
                val preferred = if (config.preferCbr) MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR else MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR
                val alternate = if (config.preferCbr) MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR else MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
                val mode = listOf(preferred, alternate).firstOrNull { caps.encoderCapabilities.isBitrateModeSupported(it) }
                    ?: error("Neither CBR nor VBR is supported")
                val format = MediaFormat.createVideoFormat("video/avc", config.width, config.height).apply {
                    setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                    setInteger(MediaFormat.KEY_BIT_RATE, config.bitrate)
                    setInteger(MediaFormat.KEY_FRAME_RATE, config.fps)
                    setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                    setInteger(MediaFormat.KEY_BITRATE_MODE, mode)
                    setInteger(MediaFormat.KEY_PROFILE, profile)
                    setInteger(MediaFormat.KEY_LEVEL, if (config.fps == 60) MediaCodecInfo.CodecProfileLevel.AVCLevel42 else MediaCodecInfo.CodecProfileLevel.AVCLevel41)
                    setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
                    setInteger(MediaFormat.KEY_LATENCY, 0)
                    setInteger(MediaFormat.KEY_PRIORITY, 0)
                    setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
                    setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
                    setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
                    setFloat(MediaFormat.KEY_OPERATING_RATE, config.fps.toFloat())
                }
                attempt = MediaCodec.createByCodecName(info.name)
                attempt.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                surface = attempt.createInputSurface()
                selected = attempt
                break
            } catch (e: Exception) {
                runCatching { attempt?.release() }
                failures += "${info.name}: ${e.message}"
            }
        }
        codec = selected ?: error("No hardware AVC encoder supports this configuration. ${failures.joinToString()}")
        inputSurface = checkNotNull(surface)
        codecName = codec.name
    }

    fun start() {
        check(!started)
        codec.start()
        started = true
        running.set(true)
        worker = thread(name = "hardware-avc-drain") {
            val info = MediaCodec.BufferInfo()
            val accessUnits = EncodedAccessUnits(onAccessUnit, { syncRequested.set(true) })
            try {
                while (running.get()) {
                    if (syncRequested.getAndSet(false)) codec.setParameters(Bundle().apply {
                        putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                    })
                    when (val index = codec.dequeueOutputBuffer(info, 10_000)) {
                        MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            val format = codec.outputFormat
                            for (key in listOf("csd-0", "csd-1")) format.getByteBuffer(key)?.duplicate()?.let { buffer ->
                                val bytes = ByteArray(buffer.remaining())
                                buffer.get(bytes)
                                onAccessUnit(bytes, 0, true)
                            }
                        }
                        MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                        else -> if (index >= 0) {
                            try {
                                if (info.size > 0) {
                                    // This is compressed H.264, never raw camera pixels.
                                    val buffer = checkNotNull(codec.getOutputBuffer(index)).duplicate()
                                    buffer.position(info.offset)
                                    buffer.limit(info.offset + info.size)
                                    val bytes = ByteArray(info.size)
                                    buffer.get(bytes)
                                    accessUnits.offer(bytes, info.presentationTimeUs,
                                        info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0,
                                        info.flags and MediaCodec.BUFFER_FLAG_PARTIAL_FRAME != 0)
                                }
                            } finally { codec.releaseOutputBuffer(index, false) }
                        }
                    }
                }
            } catch (e: Exception) { if (running.getAndSet(false)) onError(e) }
        }
    }

    fun requestKeyframe() { syncRequested.set(true) }

    override fun close() {
        running.set(false)
        worker?.join() // Drain polls with a 10 ms timeout; never release under its thread.
        try { if (started) codec.stop() } finally {
            try { codec.release() } finally { inputSurface.release() }
        }
    }
}

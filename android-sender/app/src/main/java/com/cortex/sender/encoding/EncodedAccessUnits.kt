package com.cortex.sender.encoding

import java.io.ByteArrayOutputStream

/** Joins only compressed MediaCodec partial outputs, never camera pixels. */
class EncodedAccessUnits(
    private val emit: (ByteArray, Long, Boolean) -> Unit,
    private val discarded: () -> Unit,
    private val maxBytes: Int = 4 * 1024 * 1024,
) {
    private var pending: ByteArrayOutputStream? = null
    private var pts = 0L
    private var config = false
    fun offer(bytes: ByteArray, timestampUs: Long, codecConfig: Boolean, partial: Boolean) {
        if (pending != null && pts != timestampUs) { pending = null; discarded() }
        if (bytes.size + (pending?.size() ?: 0) > maxBytes) { pending = null; discarded(); return }
        if (partial || pending != null) {
            if (pending == null) { pending = ByteArrayOutputStream(bytes.size); pts = timestampUs; config = codecConfig }
            pending!!.write(bytes); config = config || codecConfig
            if (!partial) { val complete = pending!!.toByteArray(); pending = null; emit(complete, timestampUs, config) }
        } else if (bytes.isNotEmpty()) emit(bytes, timestampUs, codecConfig)
    }
}

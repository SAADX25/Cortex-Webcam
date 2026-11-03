package com.cortex.sender.core

object VideoOrientation {
    fun previewCorrection(sensorDegrees: Int, encodedRotation: Int, frontFacing: Boolean = false): Int {
        require(sensorDegrees in setOf(0,90,180,270) && encodedRotation in setOf(0,90,180,270))
        // SurfaceTexture already maps the sensor buffer to its natural orientation.
        // Front-camera display rotation uses the opposite basis to encoded metadata.
        // Correct the local preview only; do not change the RTP/encoder rotation.
        return if (frontFacing) (sensorDegrees - encodedRotation + 360) % 360
               else (encodedRotation - sensorDegrees + 360) % 360
    }
    fun clockwise(sensorDegrees: Int, displayDegrees: Int, frontFacing: Boolean = false): Int {
        require(sensorDegrees in setOf(0,90,180,270) && displayDegrees in setOf(0,90,180,270))
        return (sensorDegrees + (if (frontFacing) displayDegrees else -displayDegrees) + 360) % 360
    }
}

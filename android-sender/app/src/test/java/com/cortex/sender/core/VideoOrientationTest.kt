package com.cortex.sender.core
import org.junit.Assert.*
import org.junit.Test
class VideoOrientationTest {
    @Test fun rearCameraKeepsBothPortraitAndLandscapeUpright() {
        assertEquals(90,VideoOrientation.clockwise(90,0))
        assertEquals(0,VideoOrientation.clockwise(90,90))
        assertEquals(270,VideoOrientation.clockwise(90,180))
        assertEquals(180,VideoOrientation.clockwise(90,270))
        assertEquals(0,VideoOrientation.clockwise(270,270))
    }
    @Test fun rejectsNonQuarterTurns() {
        assertThrows(IllegalArgumentException::class.java){VideoOrientation.clockwise(45,0)}
    }
    @Test fun surfaceTextureDoesNotRotateTheSensorTwice() {
        for(sensor in listOf(90,270)) {
            for(display in listOf(0,90,180,270)) {
                val rear=VideoOrientation.clockwise(sensor,display)
                val front=VideoOrientation.clockwise(sensor,display,true)
                assertEquals((360-display)%360,VideoOrientation.previewCorrection(sensor,rear))
                assertEquals((360-display)%360,VideoOrientation.previewCorrection(sensor,front,true))
            }
        }
    }
    @Test fun frontLandscapePreviewDoesNotIntroduceAHalfTurn() {
        for(sensor in listOf(0,90,180,270)) for(display in listOf(0,90,180,270)) {
            val encoded=VideoOrientation.clockwise(sensor,display,true)
            assertEquals((360-display)%360,VideoOrientation.previewCorrection(sensor,encoded,true))
            assertEquals((sensor+display)%360,encoded)
        }
        assertEquals(270,VideoOrientation.previewCorrection(270,0,true))
        assertEquals(90,VideoOrientation.previewCorrection(270,180,true))
    }
    @Test fun frontCameraReportsItsOwnSensorRotation() {
        assertEquals(270,VideoOrientation.clockwise(270,0,true))
        assertEquals(0,VideoOrientation.clockwise(270,90,true))
        assertEquals(90,VideoOrientation.clockwise(270,180,true))
        assertEquals(180,VideoOrientation.clockwise(270,270,true))
    }
}

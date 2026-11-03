package com.cortex.sender.ui

import android.view.ContextThemeWrapper
import android.view.View
import android.widget.TextView
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/** Real Android ViewGroup ownership regression, without opening a camera or network socket. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class CameraScreenRotationTest {
    private fun screen() = CameraScreen(
        ContextThemeWrapper(RuntimeEnvironment.getApplication(), android.R.style.Theme_Material_NoActionBar),
        onSettings = {}, onPause = {}, onFlip = {}, onSaver = {},
        onOrientation = {}, onFps = {}, onDetails = {},
    )
    private fun layout(view: View, width: Int, height: Int) {
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY))
        view.layout(0, 0, width, height)
    }
    @Test fun repeatedPortraitLandscapeAndReverseRotationsKeepOneParentPerControl() {
        val view = screen()
        repeat(12) { index ->
            layout(view, 1080, 2400)
            assertTrue(view.preview.width > 0 && view.preview.height > 0)
            layout(view, 2400, 1080)
            assertTrue(view.preview.width > view.preview.height)
            layout(view, 1080, 2400)
            view.render(true, true, index % 2 == 0, false, true, true, 30, "Live")
            assertTrue(view.preview.height > view.preview.width)
        }
    }
    @Test fun startingInLandscapeThenReturningToPortraitDoesNotReattachAnOwnedLabel() {
        val view = screen()
        layout(view, 2400, 1080)
        layout(view, 1080, 2400)
        view.select(2, 60)
        layout(view, 2400, 1080)
        layout(view, 1080, 2400)
        assertNotNull(view.preview.parent)
    }
}

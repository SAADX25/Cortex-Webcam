package com.cortex.sender.ui

import android.app.Activity
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Insets
import android.graphics.Rect
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.TextView
import com.cortex.sender.R
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowDialog
import java.io.File
import java.util.Locale

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "mdpi")
class CameraScreenDesignTest {
    private fun descendants(view: View): List<View> = listOf(view) +
        if (view is ViewGroup) (0 until view.childCount).flatMap { descendants(view.getChildAt(it)) } else emptyList()
    private fun layout(view: View, width: Int, height: Int) {
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY))
        view.layout(0, 0, width, height)
    }
    private fun create(): Pair<Activity, CameraScreen> {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        activity.setTheme(android.R.style.Theme_Material_NoActionBar)
        val screen = CameraScreen(activity, {}, {}, {}, {}, {}, {}, {})
        activity.setContentView(screen)
        return activity to screen
    }
    @Test fun controlsRemainReachableOnSmallScreensAndInArabic() {
        for (language in listOf("en", "ar")) {
            RuntimeEnvironment.setQualifiers("${language}-r${if (language == "ar") "JO" else "US"}-mdpi")
            val (activity, screen) = create()
            screen.render(true, true, false, false, true, true, 60, "Live")
            for ((width, height) in listOf(320 to 568, 412 to 840, 640 to 320, 800 to 360)) {
                layout(screen, width, height)
                val clickables = descendants(screen).filter { it.isClickable && it.visibility == View.VISIBLE }
                assertEquals("Unique actions at $language $width x $height", 7, clickables.size)
                for (view in clickables) {
                    val bounds = Rect(0, 0, view.width, view.height)
                    screen.offsetDescendantRectToMyCoords(view, bounds)
                    assertTrue("Touch target ${view.contentDescription}: $bounds", view.width >= 48 && view.height >= 48)
                    assertTrue("Clipped action ${view.contentDescription}: $bounds in $width x $height",
                        bounds.left >= 0 && bounds.top >= 0 && bounds.right <= width && bounds.bottom <= height)
                    assertFalse(view.contentDescription.isNullOrBlank())
                    if (view is ViewGroup) for (label in descendants(view).filterIsInstance<TextView>()) {
                        val textBounds = Rect(0, 0, label.width, label.height)
                        view.offsetDescendantRectToMyCoords(label, textBounds)
                        assertTrue("Label clipped in ${view.contentDescription}: $textBounds / ${view.width} x ${view.height}",
                            textBounds.left >= 0 && textBounds.top >= 0 && textBounds.right <= view.width && textBounds.bottom <= view.height)
                    }
                }
                assertTrue("Usable preview", screen.preview.width >= 160 && screen.preview.height >= 160)
            }
            activity.finish()
        }
    }
    @Test fun landscapeHonorsSystemInsetsWithLargerText() {
        RuntimeEnvironment.setQualifiers("ar-rJO-mdpi")
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        activity.setTheme(android.R.style.Theme_Material_NoActionBar)
        val configuration = Configuration(activity.resources.configuration).apply { fontScale = 1.3f }
        val context = activity.createConfigurationContext(configuration)
        val screen = CameraScreen(context, {}, {}, {}, {}, {}, {}, {})
        screen.render(false, false, false, false, true, true, 30, "Searching")
        screen.dispatchApplyWindowInsets(WindowInsets.Builder()
            .setInsets(WindowInsets.Type.systemBars(), Insets.of(0, 24, 0, 24)).build())
        layout(screen, 640, 320)
        for (view in descendants(screen).filter { it.isClickable }) {
            val bounds = Rect(0, 0, view.width, view.height)
            screen.offsetDescendantRectToMyCoords(view, bounds)
            assertTrue("Control behind a system bar: $bounds", bounds.top >= 24 && bounds.bottom <= 296)
        }
        activity.finish()
    }
    @Test fun optionsApplyCallbacksAndReflectWindowsRateWithoutReopening() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        activity.setTheme(android.R.style.Theme_Material_NoActionBar)
        var chosenRate = 0
        var chosenMode = -1
        val screen = CameraScreen(activity, {}, {}, {}, {}, { chosenMode = it }, { chosenRate = it }, {})
        activity.setContentView(screen)
        layout(screen, 412, 840)
        descendants(screen).first { it.contentDescription?.startsWith("Frame rate:") == true }.performClick()
        val dialog = ShadowDialog.getLatestDialog()
        assertTrue(dialog.isShowing)
        val views = descendants(dialog.window!!.decorView)
        (views.first { it is TextView && it.text.toString() == activity.getString(R.string.camera_fps_smooth) }).performClick()
        assertEquals(60, chosenRate)
        (views.first { it is TextView && it.text.toString() == activity.getString(R.string.camera_portrait) }).performClick()
        assertEquals(1, chosenMode)
        screen.select(1, 60)
        assertTrue(views.first { it is TextView && it.text.toString() == activity.getString(R.string.camera_fps_smooth) }.isSelected)
        screen.select(1, 30)
        assertTrue(views.first { it is TextView && it.text.toString() == activity.getString(R.string.camera_fps_balanced) }.isSelected)
        layout(screen, 840, 412)
        assertFalse("Rotation closes a sheet whose dimensions are stale", dialog.isShowing)
        activity.finish()
    }
    @Test @GraphicsMode(GraphicsMode.Mode.NATIVE)
    fun renderReviewImagesFromRealAndroidViews() {
        for (language in listOf("en", "ar")) {
            RuntimeEnvironment.setQualifiers("${language}-r${if (language == "ar") "JO" else "US"}-mdpi")
            val (activity, screen) = create()
            screen.select(0, 60)
            screen.render(false, false, true, false, true, true, 60, "Paused")
            for ((width, height) in listOf(412 to 840, 800 to 360)) {
                layout(screen, width, height)
                // Deterministic static render, without a connected camera.
                descendants(screen).forEach { it.animate().cancel(); it.alpha = 1f }
                val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                screen.draw(Canvas(bitmap))
                val file = File("build/ui-review/camera-$language-${width}x$height.png")
                file.parentFile?.mkdirs()
                file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
                bitmap.recycle()
            }
            activity.finish()
        }
    }
}

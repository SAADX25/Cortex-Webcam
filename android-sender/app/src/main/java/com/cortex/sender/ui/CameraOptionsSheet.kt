package com.cortex.sender.ui

import android.app.Dialog
import android.content.Context
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import com.cortex.sender.R

/** Scrollable on small landscape screens; reflects remote selection changes too. */
internal class CameraOptionsSheet(context: Context,
    private val onOrientation: (Int) -> Unit,
    private val onFps: (Int) -> Unit,
) : Dialog(context) {
    private val ui = CameraUi(context)
    private val modeButtons = mutableListOf<Button>()
    private val rateButtons = mutableListOf<Button>()
    init {
        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(ui.dp(20), ui.dp(20), ui.dp(20), ui.dp(20))
            background = ui.shape(ui.surface, 24, ui.border)
        }
        fun caption(id: Int, title: Boolean = false) {
            content.addView(ui.label(context.getString(id), if (title) 20f else 13f,
                if (title) ui.foreground else ui.muted, title),
                LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = ui.dp(14) })
        }
        caption(R.string.camera_video_settings, true)
        caption(R.string.camera_video_settings_hint)
        caption(R.string.camera_orientation)
        fun option(id: Int, action: () -> Unit) = Button(context).apply {
            text = context.getString(id); textSize = 13f; isAllCaps = false
            minWidth = 0; minimumWidth = 0
            minHeight = ui.dp(48); minimumHeight = ui.dp(48)
            setPadding(ui.dp(6), ui.dp(10), ui.dp(6), ui.dp(10))
            setOnClickListener { performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY); action() }
            ui.press(this)
        }
        val modes = LinearLayout(context)
        listOf(R.string.camera_auto, R.string.camera_portrait, R.string.camera_landscape).forEachIndexed { index, id ->
            val button = option(id) { onOrientation(index) }
            modeButtons += button
            modes.addView(button, LinearLayout.LayoutParams(0, -2, 1f).apply { if (index > 0) marginStart = ui.dp(8) })
        }
        content.addView(modes, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = ui.dp(12) })
        caption(R.string.camera_orientation_hint)
        caption(R.string.camera_frame_rate)
        listOf(R.string.camera_fps_balanced, R.string.camera_fps_smooth).forEachIndexed { index, id ->
            val button = option(id) { onFps(if (index == 0) 30 else 60) }
            rateButtons += button
            content.addView(button, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = ui.dp(8) })
        }
        caption(R.string.camera_fps_hint)
        content.addView(option(R.string.camera_done) { dismiss() }.apply {
            background = ui.touch(ui.accent); setTextColor(ui.background)
        }, LinearLayout.LayoutParams(-1, -2))
        setContentView(ScrollView(context).apply { isFillViewport = false; addView(content) })
        window?.apply {
            setBackgroundDrawableResource(android.R.color.transparent)
            addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            setDimAmount(.55f)
            setGravity(Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL)
        }
    }
    override fun show() {
        super.show()
        val bounds = context.resources.displayMetrics
        val width = minOf(bounds.widthPixels - ui.dp(24), ui.dp(460))
        window?.decorView?.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
        window?.setLayout(width, minOf(window?.decorView?.measuredHeight ?: bounds.heightPixels, bounds.heightPixels - ui.dp(64)))
        window?.attributes = window?.attributes?.apply { y = ui.dp(12) }
    }
    fun select(mode: Int, fps: Int) {
        fun skin(button: Button, selected: Boolean) {
            button.isSelected = selected
            button.background = ui.touch(if (selected) 0xff183b4a.toInt() else ui.raised, 14,
                if (selected) ui.accent else ui.border)
            button.setTextColor(if (selected) ui.accent else ui.foreground)
            // Radio semantics expose selection to TalkBack without extra glyphs.
            button.accessibilityDelegate = object : View.AccessibilityDelegate() {
                override fun onInitializeAccessibilityNodeInfo(host: View, info: android.view.accessibility.AccessibilityNodeInfo) {
                    super.onInitializeAccessibilityNodeInfo(host, info)
                    info.isCheckable = true; info.isChecked = selected
                }
            }
        }
        modeButtons.forEachIndexed { index, button -> skin(button, index == mode) }
        rateButtons.forEachIndexed { index, button -> skin(button, (if (index == 0) 30 else 60) == fps) }
    }
}

package com.cortex.sender.ui

import android.animation.ValueAnimator
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.view.*
import android.widget.*
import com.cortex.sender.R
import com.cortex.sender.ui.CameraIcon.Symbol

/** Viewfinder chrome only. Reflow never replaces the SurfaceView or owns capture. */
class CameraScreen(context: Context,
    private val onSettings: () -> Unit,
    private val onPause: () -> Unit,
    private val onFlip: () -> Unit,
    private val onSaver: () -> Unit,
    private val onOrientation: (Int) -> Unit,
    private val onFps: (Int) -> Unit,
    private val onDetails: () -> Unit,
) : FrameLayout(context) {
    private val ui = CameraUi(context)
    private fun dp(n: Int) = ui.dp(n)
    private fun s(id: Int) = context.getString(id)
    val preview = SurfaceView(context).apply { contentDescription = s(R.string.camera_preview) }
    private val page = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private val header = LinearLayout(context).apply { gravity = Gravity.CENTER_VERTICAL }
    private val body = LinearLayout(context)
    private val viewfinder = FrameLayout(context).apply {
        background = ui.shape(Color.BLACK, 20, ui.border)
        clipToOutline = true
    }
    private val dock = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(12), dp(10), dp(12), dp(10))
        background = ui.shape(ui.surface, 22, ui.border)
    }
    private val status = ui.label(s(R.string.camera_searching), 12f, ui.accent, true).apply {
        gravity = Gravity.CENTER
        setPadding(dp(10), dp(8), dp(10), dp(8))
        minHeight = dp(48)
        minWidth = dp(48)
        maxWidth = dp(112)
        background = ui.touch(ui.surface, 24)
        isClickable = true; isFocusable = true
        setOnClickListener { onDetails() }
    }
    private fun badge(value: String) = ui.label(value, 11f).apply {
        setPadding(dp(10), dp(7), dp(10), dp(7))
        background = ui.shape(0xcc0c1725.toInt(), 10)
    }
    private val quality = badge("1080p / 30 fps")
    private val facing = badge(s(R.string.camera_rear))
    private val hint = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        setPadding(dp(24), dp(16), dp(24), dp(16))
        background = ui.shape(0xff0c1522.toInt(), 20, ui.border)
    }
    private val hintTitle = ui.label(s(R.string.camera_hint_searching), 18f, bold = true).apply { gravity = Gravity.CENTER }
    private val hintBody = ui.label(s(R.string.camera_hint_network), 13f, ui.muted).apply { gravity = Gravity.CENTER }
    private val caption = ui.label(s(R.string.camera_controls), 11f, ui.muted, true)
    private var selectedMode = 0
    private var selectedFps = 30
    private var lastLayout: Pair<Boolean, Boolean>? = null
    private var sheet: CameraOptionsSheet? = null

    private inner class Action(symbol: Symbol, title: String, primary: Boolean = false, action: () -> Unit) : LinearLayout(context) {
        val glyph = ImageView(context).apply {
            setImageDrawable(CameraIcon(symbol))
            imageTintList = ColorStateList.valueOf(if (primary) ui.background else ui.foreground)
        }
        val label = ui.label(title, 12f, if (primary) ui.background else ui.foreground, primary).apply { gravity = Gravity.CENTER }
        init {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER
            isClickable = true; isFocusable = true
            contentDescription = title
            background = ui.touch(if (primary) ui.accent else ui.raised, 16)
            setPadding(dp(8), dp(8), dp(8), dp(8))
            addView(glyph, LinearLayout.LayoutParams(dp(22), dp(22)))
            addView(label, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8) })
            setOnClickListener { performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY); action() }
            ui.press(this)
        }
        fun compact(value: Boolean) {
            orientation = if (value) VERTICAL else HORIZONTAL
            setPadding(dp(8), dp(if (value) 4 else 8), dp(8), dp(if (value) 4 else 8))
            glyph.layoutParams = LinearLayout.LayoutParams(dp(if (value) 20 else 22), dp(if (value) 20 else 22))
            label.layoutParams = LinearLayout.LayoutParams(-2, -2).apply {
                if (value) topMargin = dp(3) else marginStart = dp(8)
            }
            label.textSize = if (value) 11f else 12f
        }
    }
    private val flip = Action(Symbol.FLIP, s(R.string.camera_flip), action = onFlip)
    private val saver = Action(Symbol.MOON, s(R.string.camera_dim), action = onSaver)
    private val pause = Action(Symbol.PAUSE, s(R.string.camera_pause), true, onPause)
    private val orientation = Action(Symbol.AUTO, s(R.string.camera_auto)) { showOptions() }
    private val fps = Action(Symbol.SPEED, "30 fps") { showOptions() }

    init {
        setBackgroundColor(ui.background)
        val logo = ui.label("C", 21f, ui.background, true).apply {
            gravity = Gravity.CENTER
            background = ui.shape(ui.accent, 12)
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        header.addView(logo, LinearLayout.LayoutParams(dp(36), dp(36)).apply { marginEnd = dp(10) })
        val identity = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER_VERTICAL or Gravity.START }
        identity.addView(ui.label("Cortex", 18f, bold = true), LinearLayout.LayoutParams(-2, -2))
        identity.addView(ui.label(s(R.string.camera_studio), 10f, ui.muted).apply { setPadding(0, dp(3), 0, 0) }, LinearLayout.LayoutParams(-2, -2))
        header.addView(identity, LinearLayout.LayoutParams(0, -1, 1f))
        header.addView(status, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(6) })
        header.addView(ImageButton(context).apply {
            setImageDrawable(CameraIcon(Symbol.SETTINGS))
            imageTintList = ColorStateList.valueOf(ui.foreground)
            setPadding(dp(14), dp(14), dp(14), dp(14))
            background = ui.touch(ui.surface, 16, ui.border)
            contentDescription = s(R.string.camera_settings)
            tooltipText = contentDescription
            setOnClickListener { onSettings() }
            ui.press(this)
        }, LinearLayout.LayoutParams(dp(48), dp(48)))
        page.addView(header, LinearLayout.LayoutParams(-1, dp(56)).apply { bottomMargin = dp(10) })
        viewfinder.addView(preview, LayoutParams(-1, -1))
        hint.addView(ImageView(context).apply {
            setImageDrawable(CameraIcon(Symbol.WIFI)); imageTintList = ColorStateList.valueOf(ui.accent)
        }, LinearLayout.LayoutParams(dp(36), dp(36)).apply { bottomMargin = dp(18) })
        hint.addView(hintTitle, LinearLayout.LayoutParams(-1, -2))
        hint.addView(hintBody, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(10) })
        viewfinder.addView(hint, LayoutParams(-1, -1))
        viewfinder.addView(quality, LayoutParams(-2, -2, Gravity.TOP or Gravity.START).apply { setMargins(dp(12), dp(12), dp(12), 0) })
        viewfinder.addView(facing, LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.START).apply { setMargins(dp(12), 0, dp(12), dp(12)) })
        body.addView(viewfinder); body.addView(dock)
        page.addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        addView(page, LayoutParams(-1, -1))
        // Explicit edge-to-edge in Activity, with a single owner for safe insets.
        setOnApplyWindowInsetsListener { _, insets ->
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                val safe = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                page.setPadding(dp(14) + safe.left, dp(6) + safe.top, dp(14) + safe.right, dp(10) + safe.bottom)
            } else {
                @Suppress("DEPRECATION")
                page.setPadding(dp(14) + insets.systemWindowInsetLeft, dp(6) + insets.systemWindowInsetTop,
                    dp(14) + insets.systemWindowInsetRight, dp(10) + insets.systemWindowInsetBottom)
            }
            insets
        }
        page.setPadding(dp(14), dp(6), dp(14), dp(10))
        reflow(false, false)
        select(0, 30)
    }
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        if (w > 0 && h > 0) reflow(w > h, h < dp(390))
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
    }
    private fun detach(view: View) { (view.parent as? ViewGroup)?.removeView(view) }
    private fun reflow(wide: Boolean, short: Boolean) {
        val next = wide to short
        if (lastLayout == next) return
        lastLayout = next
        sheet?.dismiss(); sheet = null
        // Keep preview attached. Only move retained chrome between rows.
        listOf(caption, flip, saver, pause, orientation, fps).forEach(::detach)
        dock.removeAllViews()
        body.orientation = if (wide) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
        viewfinder.layoutParams = if (wide) LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(10) }
            else LinearLayout.LayoutParams(-1, 0, 1f).apply { bottomMargin = dp(10) }
        dock.layoutParams = if (wide) LinearLayout.LayoutParams(dp(if (short) 184 else 204), -1)
            else LinearLayout.LayoutParams(-1, -2)
        fun row(vararg views: View, height: Int = 48) = LinearLayout(context).apply {
            gravity = Gravity.CENTER_VERTICAL
            views.forEachIndexed { index, view ->
                addView(view, LinearLayout.LayoutParams(0, dp(height), 1f).apply { if (index > 0) marginStart = dp(8) })
            }
        }
        fun add(view: View, top: Int = 0, height: Int = -2) {
            dock.addView(view, LinearLayout.LayoutParams(-1, if (height > 0) dp(height) else height).apply { topMargin = dp(top) })
        }
        flip.compact(true); saver.compact(true)
        flip.label.text = s(R.string.camera_flip_short)
        saver.label.text = s(R.string.camera_dim_short)
        pause.compact(false); orientation.compact(false); fps.compact(false)
        if (wide) {
            dock.gravity = Gravity.CENTER_VERTICAL
            if (!short) add(caption)
            add(row(flip, saver, height = if (short) 52 else 64), if (short) 0 else 12)
            add(pause, 8, 48)
            if (short) {
                orientation.compact(true); fps.compact(true)
                add(row(orientation, fps, height = 52), 8)
            } else {
                add(row(orientation), 8)
                add(row(fps), 8)
            }
        } else {
            dock.gravity = Gravity.CENTER
            add(caption)
            pause.compact(true)
            add(row(flip, pause, saver, height = 64), 8)
            add(row(orientation, fps), 8)
        }
    }
    private fun showOptions() {
        if (sheet?.isShowing == true) return
        sheet = CameraOptionsSheet(context, onOrientation, onFps).also {
            it.select(selectedMode, selectedFps); it.show()
        }
    }
    fun select(mode: Int, fps: Int) {
        selectedMode = mode.coerceIn(0, 2); selectedFps = fps
        orientation.glyph.setImageDrawable(CameraIcon(when (selectedMode) { 1 -> Symbol.PORTRAIT; 2 -> Symbol.LANDSCAPE; else -> Symbol.AUTO }))
        val modeName = s(when (selectedMode) { 1 -> R.string.camera_portrait; 2 -> R.string.camera_landscape; else -> R.string.camera_auto })
        orientation.label.text = modeName
        orientation.contentDescription = "${s(R.string.camera_orientation)}: $modeName"
        this.fps.label.text = context.getString(R.string.camera_fps_value, fps)
        this.fps.contentDescription = "${s(R.string.camera_frame_rate)}: ${this.fps.label.text}"
        sheet?.select(selectedMode, fps)
    }
    fun render(active: Boolean, confirmed: Boolean, paused: Boolean, switching: Boolean,
               canFlip: Boolean, front: Boolean, fps: Int, message: String) {
        val error = message.startsWith("Error")
        val stateId = when {
            switching -> R.string.camera_switching
            paused -> R.string.camera_paused
            error -> R.string.camera_error
            confirmed -> R.string.camera_live
            active -> R.string.camera_connecting
            else -> R.string.camera_searching
        }
        val next = s(stateId)
        if (status.text.toString() != next) {
            status.text = next
            if (ValueAnimator.areAnimatorsEnabled()) {
                status.animate().cancel(); status.alpha = .6f
                status.animate().alpha(1f).setDuration(180).start()
            }
        }
        status.setTextColor(if (confirmed && !paused && !switching) ui.live else ui.accent)
        status.contentDescription = context.getString(R.string.camera_details, next)
        hint.visibility = if (active && !switching) View.GONE else View.VISIBLE
        hintTitle.text = s(when { switching -> R.string.camera_switching; paused -> R.string.camera_hint_paused; error -> R.string.camera_error; else -> R.string.camera_hint_searching })
        hintBody.text = s(when { paused -> R.string.camera_hint_resume; error -> R.string.camera_hint_error; else -> R.string.camera_hint_network })
        quality.text = "1080p / ${context.getString(R.string.camera_fps_value, fps)}"
        facing.text = s(if (front) R.string.camera_front else R.string.camera_rear)
        flip.isEnabled = canFlip && !switching; flip.alpha = if (flip.isEnabled) 1f else .4f
        saver.isEnabled = active; saver.alpha = if (active) 1f else .4f
        pause.glyph.setImageDrawable(CameraIcon(if (paused) Symbol.PLAY else Symbol.PAUSE))
        pause.label.text = s(if (paused) R.string.camera_resume else R.string.camera_pause)
        pause.contentDescription = s(if (paused) R.string.camera_resume_description else R.string.camera_pause_description)
    }
    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (ValueAnimator.areAnimatorsEnabled()) { dock.alpha = 0f; dock.animate().alpha(1f).setDuration(220).start() }
    }
    override fun onDetachedFromWindow() {
        sheet?.dismiss(); sheet = null
        status.animate().cancel(); dock.animate().cancel()
        super.onDetachedFromWindow()
    }
}

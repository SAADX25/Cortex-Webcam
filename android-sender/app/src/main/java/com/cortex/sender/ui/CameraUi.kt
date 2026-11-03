package com.cortex.sender.ui

import android.animation.ObjectAnimator
import android.animation.StateListAnimator
import android.animation.ValueAnimator
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.View
import android.widget.TextView

/** Shared tokens for the viewfinder and its settings sheet. */
internal class CameraUi(private val context: Context) {
    val background = 0xff090f19.toInt()
    val surface = 0xff111c2b.toInt()
    val raised = 0xff19283b.toInt()
    val border = 0xff2a3a4e.toInt()
    val accent = 0xff79d7ed.toInt()
    val foreground = 0xffeff5ff.toInt()
    val muted = 0xff9eafc5.toInt()
    val live = 0xff7ce5b7.toInt()
    fun dp(value: Int) = (context.resources.displayMetrics.density * value).toInt()
    fun shape(color: Int, radius: Int = 16, stroke: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radius).toFloat()
        if (stroke != 0) setStroke(dp(1), stroke)
    }
    fun touch(color: Int = raised, radius: Int = 16, stroke: Int = 0) =
        RippleDrawable(ColorStateList.valueOf(0x2679d7ed), shape(color, radius, stroke), null)
    fun label(value: String, size: Float = 13f, color: Int = foreground, bold: Boolean = false) =
        TextView(context).apply {
            text = value
            textSize = size
            setTextColor(color)
            includeFontPadding = false
            typeface = Typeface.create(if (bold) "sans-serif-medium" else "sans-serif", Typeface.NORMAL)
        }
    fun press(view: View) {
        if (!ValueAnimator.areAnimatorsEnabled()) return
        view.stateListAnimator = StateListAnimator().apply {
            addState(intArrayOf(android.R.attr.state_pressed), ObjectAnimator.ofPropertyValuesHolder(view,
                android.animation.PropertyValuesHolder.ofFloat(View.SCALE_X, .96f),
                android.animation.PropertyValuesHolder.ofFloat(View.SCALE_Y, .96f)).setDuration(90))
            addState(intArrayOf(), ObjectAnimator.ofPropertyValuesHolder(view,
                android.animation.PropertyValuesHolder.ofFloat(View.SCALE_X, 1f),
                android.animation.PropertyValuesHolder.ofFloat(View.SCALE_Y, 1f)).setDuration(160))
        }
    }
}

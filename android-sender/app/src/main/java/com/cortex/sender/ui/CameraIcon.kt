package com.cortex.sender.ui

import android.graphics.*
import android.graphics.drawable.Drawable

/** Small consistent vector icons; no font glyphs or bitmap assets. */
class CameraIcon(private val symbol: Symbol) : Drawable() {
    enum class Symbol { CAMERA, FLIP, PAUSE, PLAY, MOON, SETTINGS, WIFI, AUTO, PORTRAIT, LANDSCAPE, SPEED }
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color=Color.WHITE;style=Paint.Style.STROKE;strokeWidth=1.8f;strokeCap=Paint.Cap.ROUND;strokeJoin=Paint.Join.ROUND }
    private val paths = mutableListOf<Path>()
    init {
        fun path(vararg points: Float) { paths+=Path().apply { moveTo(points[0],points[1]);var i=2;while(i<points.size){lineTo(points[i],points[i+1]);i+=2} } }
        when(symbol) {
            Symbol.CAMERA,Symbol.FLIP -> {
                paths+=Path().apply { moveTo(3f,7f);lineTo(7f,7f);lineTo(9f,4f);lineTo(15f,4f);lineTo(17f,7f);lineTo(21f,7f);lineTo(21f,20f);lineTo(3f,20f);close() }
                if(symbol==Symbol.CAMERA)paths+=Path().apply { addCircle(12f,13f,3.6f,Path.Direction.CW) }
                else { paths+=Path().apply { addArc(RectF(7f,9f,17f,18f),205f,245f) };path(7f,14f,7f,10f,11f,10f);path(17f,13f,17f,17f,13f,17f) }
            }
            Symbol.PAUSE -> { path(9f,6f,9f,18f);path(15f,6f,15f,18f) }
            Symbol.SPEED -> {
                paths+=Path().apply { addArc(RectF(3f,3f,21f,21f),145f,250f); addCircle(12f,12f,1.2f,Path.Direction.CW) }
                path(12f,12f,17f,7f);path(7f,19f,17f,19f)
            }
            Symbol.PLAY -> path(8f,5f,19f,12f,8f,19f,8f,5f)
            Symbol.MOON -> paths+=Path().apply { moveTo(19.5f,15f);cubicTo(10f,18f,6f,9f,10f,3f);cubicTo(0f,5f,0f,19f,10f,21f);cubicTo(14f,22f,18f,20f,19.5f,15f);close() }
            Symbol.SETTINGS -> {path(4f,6f,20f,6f);path(4f,12f,20f,12f);path(4f,18f,20f,18f);paths+=Path().apply{addCircle(9f,6f,2f,Path.Direction.CW);addCircle(15f,12f,2f,Path.Direction.CW);addCircle(8f,18f,2f,Path.Direction.CW)} }
            Symbol.WIFI -> {paths+=Path().apply{addArc(RectF(0f,4f,24f,24f),225f,90f);addArc(RectF(5f,9f,19f,23f),225f,90f);addCircle(12f,19f,0.7f,Path.Direction.CW)} }
            Symbol.PORTRAIT -> paths+=Path().apply{addRoundRect(RectF(6f,2.5f,18f,21.5f),2f,2f,Path.Direction.CW);moveTo(10f,18.5f);lineTo(14f,18.5f)}
            Symbol.LANDSCAPE -> paths+=Path().apply{addRoundRect(RectF(2.5f,6f,21.5f,18f),2f,2f,Path.Direction.CW);moveTo(18.5f,10f);lineTo(18.5f,14f)}
            Symbol.AUTO -> { paths+=Path().apply { addRoundRect(RectF(8f,6f,16f,18f),1f,1f,Path.Direction.CW);addArc(RectF(1f,1f,23f,23f),190f,120f);addArc(RectF(1f,1f,23f,23f),10f,120f) };path(2f,8f,2f,3f,7f,3f);path(22f,16f,22f,21f,17f,21f) }
        }
    }
    override fun draw(canvas: Canvas) { canvas.save();canvas.translate(bounds.left.toFloat(),bounds.top.toFloat());canvas.scale(bounds.width()/24f,bounds.height()/24f);paths.forEach{canvas.drawPath(it,paint)};canvas.restore() }
    override fun setAlpha(alpha: Int){paint.alpha=alpha;invalidateSelf()}
    override fun setColorFilter(filter: ColorFilter?){paint.colorFilter=filter;invalidateSelf()}
    @Suppress("OVERRIDE_DEPRECATION") override fun getOpacity()=PixelFormat.TRANSLUCENT
    override fun getIntrinsicWidth()=24
    override fun getIntrinsicHeight()=24
}

package com.cortex.sender.preview

import android.graphics.SurfaceTexture
import android.opengl.*
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Service-owned camera preview target. Activity surfaces never enter the Camera2 session.
 * Only GPU textures and static quad vertices are used; no camera pixels are read by CPU. */
class GpuPreview(width: Int, height: Int) : Closeable {
    private val thread = HandlerThread("gpu-preview").apply { start() }
    private val handler = Handler(thread.looper)
    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var pbuffer: EGLSurface = EGL14.EGL_NO_SURFACE
    private var window: EGLSurface = EGL14.EGL_NO_SURFACE
    private lateinit var config: EGLConfig
    private lateinit var cameraTexture: SurfaceTexture
    lateinit var cameraSurface: Surface
        private set
    private var windowOwner: Surface? = null
    private var rotation = 0
    private var textureTurns = 0
    private var texture = 0
    private var program = 0
    private var closed = false
    private val sourceWidth = width
    private val sourceHeight = height
    private val transform = FloatArray(16)
    private val vertices = ByteBuffer.allocateDirect(16 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
        put(floatArrayOf(-1f,-1f,0f,0f, 1f,-1f,1f,0f, -1f,1f,0f,1f, 1f,1f,1f,1f)); position(0)
    }
    init {
        val ready = CountDownLatch(1)
        var failure: Throwable? = null
        handler.post {
            try { initialize(width, height) } catch (error: Throwable) { failure = error } finally { ready.countDown() }
        }
        check(ready.await(5, TimeUnit.SECONDS)) { "GPU preview initialization timed out" }
        failure?.let { close(); throw IllegalStateException("GPU preview unavailable", it) }
    }
    private fun shader(type: Int, text: String): Int {
        val id = GLES20.glCreateShader(type); GLES20.glShaderSource(id, text); GLES20.glCompileShader(id)
        val ok = IntArray(1); GLES20.glGetShaderiv(id, GLES20.GL_COMPILE_STATUS, ok, 0)
        check(ok[0] != 0) { GLES20.glGetShaderInfoLog(id) }; return id
    }
    private fun initialize(width: Int, height: Int) {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        check(EGL14.eglInitialize(display, IntArray(2), 0, IntArray(2), 0))
        val configs = arrayOfNulls<EGLConfig>(1); val count = IntArray(1)
        check(EGL14.eglChooseConfig(display, intArrayOf(EGL14.EGL_RENDERABLE_TYPE,EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE,EGL14.EGL_WINDOW_BIT or EGL14.EGL_PBUFFER_BIT,
            EGL14.EGL_RED_SIZE,8,EGL14.EGL_GREEN_SIZE,8,EGL14.EGL_BLUE_SIZE,8,EGL14.EGL_NONE), 0, configs, 0, 1, count, 0) && count[0] > 0)
        config = checkNotNull(configs[0])
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION,2,EGL14.EGL_NONE), 0)
        check(context != EGL14.EGL_NO_CONTEXT)
        pbuffer = EGL14.eglCreatePbufferSurface(display, config, intArrayOf(EGL14.EGL_WIDTH,1,EGL14.EGL_HEIGHT,1,EGL14.EGL_NONE), 0)
        check(EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context))
        val names = IntArray(1); GLES20.glGenTextures(1, names, 0); texture = names[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_WRAP_T,GLES20.GL_CLAMP_TO_EDGE)
        val vs = shader(GLES20.GL_VERTEX_SHADER, "attribute vec2 p;attribute vec2 t;varying vec2 uv;void main(){gl_Position=vec4(p,0.,1.);uv=t;}")
        val fs = shader(GLES20.GL_FRAGMENT_SHADER, """#extension GL_OES_EGL_image_external : require
            precision mediump float;uniform samplerExternalOES image;uniform mat4 st;uniform int turns;varying vec2 uv;
            void main(){vec2 q=uv;if(turns==1)q=vec2(1.-uv.y,uv.x);else if(turns==2)q=vec2(1.-uv.x,1.-uv.y);else if(turns==3)q=vec2(uv.y,1.-uv.x);
            gl_FragColor=texture2D(image,(st*vec4(q,0.,1.)).xy);}""")
        program = GLES20.glCreateProgram(); GLES20.glAttachShader(program, vs); GLES20.glAttachShader(program, fs); GLES20.glLinkProgram(program)
        val linked = IntArray(1); GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,linked,0); check(linked[0] != 0)
        GLES20.glDeleteShader(vs); GLES20.glDeleteShader(fs)
        cameraTexture = SurfaceTexture(texture).apply {
            setDefaultBufferSize(width, height)
            setOnFrameAvailableListener({ draw() }, handler)
        }
        cameraSurface = Surface(cameraTexture)
    }
    fun attach(surface: Surface?) { handler.post {
        if (closed || windowOwner === surface) return@post
        detach()
        if (surface?.isValid == true) {
            val created = EGL14.eglCreateWindowSurface(display, config, surface, intArrayOf(EGL14.EGL_NONE), 0)
            if (created != EGL14.EGL_NO_SURFACE) { window = created; windowOwner = surface }
        }
    } }
    fun rotate(degrees: Int, textureCorrection: Int = degrees) { handler.post { rotation = degrees / 90; textureTurns = textureCorrection / 90 } }
    private fun detach() {
        if (display != EGL14.EGL_NO_DISPLAY) EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)
        if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window)
        window = EGL14.EGL_NO_SURFACE; windowOwner = null
    }
    private fun draw() {
        if (closed) return
        // Drain the latest texture even if an Activity surface was destroyed mid-frame.
        if (!EGL14.eglMakeCurrent(display,pbuffer,pbuffer,context)) return
        runCatching { cameraTexture.updateTexImage(); cameraTexture.getTransformMatrix(transform) }.getOrElse { return }
        if (window == EGL14.EGL_NO_SURFACE || windowOwner?.isValid != true) return
        if (!EGL14.eglMakeCurrent(display,window,window,context)) { detach(); return }
        val w = IntArray(1); val h = IntArray(1)
        EGL14.eglQuerySurface(display,window,EGL14.EGL_WIDTH,w,0); EGL14.eglQuerySurface(display,window,EGL14.EGL_HEIGHT,h,0)
        val rw = if (rotation % 2 == 0) sourceWidth else sourceHeight
        val rh = if (rotation % 2 == 0) sourceHeight else sourceWidth
        val scale = minOf(w[0].toFloat()/rw,h[0].toFloat()/rh)
        val dw = (rw*scale).toInt(); val dh = (rh*scale).toInt()
        GLES20.glViewport(0,0,w[0],h[0]); GLES20.glClearColor(0f,0f,0f,1f); GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glViewport((w[0]-dw)/2,(h[0]-dh)/2,dw,dh); GLES20.glUseProgram(program)
        val p = GLES20.glGetAttribLocation(program,"p"); val t = GLES20.glGetAttribLocation(program,"t")
        vertices.position(0); GLES20.glVertexAttribPointer(p,2,GLES20.GL_FLOAT,false,16,vertices); GLES20.glEnableVertexAttribArray(p)
        vertices.position(2); GLES20.glVertexAttribPointer(t,2,GLES20.GL_FLOAT,false,16,vertices); GLES20.glEnableVertexAttribArray(t)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0); GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,texture)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program,"image"),0)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program,"turns"),textureTurns)
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program,"st"),1,false,transform,0)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4)
        if (!EGL14.eglSwapBuffers(display,window)) detach()
    }
    override fun close() {
        handler.post {
            closed = true; detach()
            if (::cameraSurface.isInitialized) cameraSurface.release()
            if (::cameraTexture.isInitialized) cameraTexture.release()
            if (program != 0) GLES20.glDeleteProgram(program)
            if (texture != 0) GLES20.glDeleteTextures(1,intArrayOf(texture),0)
            EGL14.eglMakeCurrent(display,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_CONTEXT)
            if (pbuffer != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display,pbuffer)
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display,context)
            if (display != EGL14.EGL_NO_DISPLAY) EGL14.eglTerminate(display)
            thread.quitSafely()
        }
        if (Thread.currentThread() !== thread) thread.join(2500)
    }
}

package com.cortex.sender.ui

import android.Manifest
import android.app.Activity
import android.app.Dialog
import android.content.*
import android.content.pm.PackageManager
import android.content.pm.ActivityInfo
import android.hardware.display.DisplayManager
import android.content.res.Configuration
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.os.*
import android.view.*
import android.widget.*
import com.cortex.sender.capture.CameraCaptureService
import com.cortex.sender.core.StreamConfig

/** UI only. Camera, encoder and discovery remain owned by the foreground service. */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private var service: CameraCaptureService? = null
    private var bound = false
    private var visible = false
    private var saver = false
    private var paused = false
    private var pending: StreamConfig? = null
    private var permissionPending = false
    private var rate = 30
    private var cbr = true
    private var target = ""
    private var controlPort = 5005
    private var orientationMode = 0
    private lateinit var root: FrameLayout
    private lateinit var screen: CameraScreen
    private lateinit var preview: SurfaceView
    private lateinit var black: View
    private var lastStatus = ""
    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(id: Int) = Unit
        override fun onDisplayRemoved(id: Int) = Unit
        override fun onDisplayChanged(id: Int) { syncRotation() }
    }
    private val main = Handler(Looper.getMainLooper())
    private val accent = Color.rgb(121, 215, 237)
    private val muted = Color.rgb(145, 164, 189)
    private val arabic get() = resources.configuration.locales[0].language == "ar"
    private fun t(en: String, ar: String) = if (arabic) ar else en
    private fun dp(value: Int) = (resources.displayMetrics.density * value).toInt()
    private fun shape(color: Int, radius: Int = 16, stroke: Int = 0) = GradientDrawable().apply {
        setColor(color); cornerRadius = dp(radius).toFloat()
        if (stroke != 0) setStroke(dp(1), stroke)
    }
    private fun text(value: String, size: Float = 14f, color: Int = Color.WHITE, bold: Boolean = false) = TextView(this).apply {
        text = value; textSize = size; setTextColor(color); includeFontPadding = false
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }
    private fun button(value: String, primary: Boolean = false, action: () -> Unit) = Button(this).apply {
        text = value; textSize = 14f; isAllCaps = false; minHeight = dp(48); minimumHeight = dp(48)
        setTextColor(if (primary) Color.rgb(10, 25, 45) else Color.rgb(226, 236, 249))
        background = RippleDrawable(ColorStateList.valueOf(0x337faaff), shape(if (primary) accent else 0xff1c2b3f.toInt(), 12), null)
        elevation = 0f; setPadding(dp(16), dp(6), dp(16), dp(6)); setOnClickListener { action() }
    }
    private fun LinearLayout.space(height: Int) { addView(View(this@MainActivity), LinearLayout.LayoutParams(1, dp(height))) }
    private fun LinearLayout.add(view: View, height: Int = -2) { addView(view, LinearLayout.LayoutParams(-1, if (height >= 0) dp(height) else height)) }
    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            service = (binder as CameraCaptureService.LocalBinder).service
            updateDisplayRotation()
            service?.onStatus = { renderStatus(it) }
            service?.setPowerSaver(saver)
            if (!saver && preview.holder.surface.isValid) service?.setPreview(preview.holder.surface)
            // Clear an old pending endpoint before installing the callback whose
            // setter may immediately replay it after a previous Wi-Fi session.
            if (service?.isActive != true && !paused) connect()
            service?.onAutoReady = { settings ->
                if (visible && !paused && service?.isActive != true) {
                    pending = settings
                    if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                        pending = null; beginStream(settings)
                    } else if (!permissionPending) {
                        permissionPending = true
                        val permissions = if(Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
                            arrayOf(Manifest.permission.CAMERA,Manifest.permission.POST_NOTIFICATIONS) else arrayOf(Manifest.permission.CAMERA)
                        getPreferences(0).edit().putBoolean("notificationAsked",true).apply()
                        requestPermissions(permissions, 10)
                    }
                }
            }
            renderStatus(service?.status.orEmpty())
        }
        override fun onServiceDisconnected(name: ComponentName) { service = null; renderStatus("Studio service disconnected. Tap Reconnect.") }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val prefs = getPreferences(0)
        // Migrate old builds which persisted auto-connect=false after a manual start.
        if (prefs.getInt("uiVersion", 0) < 3) prefs.edit().putInt("uiVersion", 3).putBoolean("automatic", true).apply()
        rate = prefs.getInt("rate", 30); cbr = prefs.getBoolean("cbr", true)
        orientationMode = prefs.getInt("orientationMode", 0)
        if(Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED && !prefs.getBoolean("notificationAsked",false)){
            prefs.edit().putBoolean("notificationAsked",true).apply()
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS),11)
        }
        @Suppress("DEPRECATION")
        window.statusBarColor = Color.TRANSPARENT
        @Suppress("DEPRECATION")
        window.navigationBarColor = Color.TRANSPARENT
        if (Build.VERSION.SDK_INT >= 30) window.setDecorFitsSystemWindows(false)
        else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        }
        root = FrameLayout(this).apply { setBackgroundColor(0xff090f19.toInt()) }
        screen = CameraScreen(this,
            onSettings = { connectionDialog() },
            onPause = {
                paused = !paused
                if (paused) { pending = null; service?.stop(); renderStatus("Paused") }
                else connect()
            },
            onFlip = { service?.flipCamera() },
            onSaver = { setSaver(true) },
            onOrientation = { changeOrientation(it) },
            onFps = { changeRate(it) },
            onDetails = { showDetails() })
        preview = screen.preview
        preview.holder.addCallback(this)
        root.addView(screen, FrameLayout.LayoutParams(-1,-1))
        black = View(this).apply {
            setBackgroundColor(Color.BLACK); visibility = View.GONE
            contentDescription = t("Tap to restore controls", "المس لاستعادة التحكم")
            setOnClickListener { setSaver(false) }
        }
        root.addView(black, FrameLayout.LayoutParams(-1,-1))
        setContentView(root)
        changeOrientation(orientationMode)
        renderStatus("Searching for Studio")
        root.requestApplyInsets()
    }
    private fun changeOrientation(mode: Int) {
        orientationMode = mode
        getPreferences(0).edit().putInt("orientationMode", mode).apply()
        requestedOrientation = when(mode) { 1 -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT; 2 -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE; else -> ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR }
        screen.select(orientationMode,rate)
        syncRotation()
    }
    private fun syncRotation() {
        updateDisplayRotation()
        main.postDelayed({ if(visible) updateDisplayRotation() },200)
        main.postDelayed({ if(visible) updateDisplayRotation() },500)
    }
    @Suppress("DEPRECATION")
    private fun updateDisplayRotation() {
        val rotation = if (Build.VERSION.SDK_INT >= 30) display?.rotation ?: Surface.ROTATION_0 else windowManager.defaultDisplay.rotation
        service?.setDisplayRotation(when(rotation){Surface.ROTATION_90->90;Surface.ROTATION_180->180;Surface.ROTATION_270->270;else->0})
    }
    private fun changeRate(value: Int) {
        if (rate == value) return
        rate = value; getPreferences(0).edit().putInt("rate", rate).apply(); screen.select(orientationMode,rate)
        if (!paused) {
            if (service?.isActive == true) service?.changeFrameRate(value) else connect()
        }
    }
    private fun connect() {
        pending = null
        service?.enableDiscovery(rate, cbr, target, controlPort)
    }
    private fun renderStatus(value: String) {
        lastStatus = value
        val active = service?.isActive == true
        if (active && service?.switchingCamera != true) {
            val effective = service?.actualFps ?: rate
            if (effective != rate) { rate = effective; getPreferences(0).edit().putInt("rate", rate).apply() }
            screen.select(orientationMode, rate)
        }
        screen.render(active, active && (service?.receiverFrames ?: 0) > 0, paused,
            service?.switchingCamera == true,
            service != null && (!active || service?.cameraReady == true),
            service?.usingFront == true, if(active) service?.actualFps ?: rate else rate, value)
        if(active) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }
    private fun showDetails() {
        val endpoint = service?.destination?.takeIf { it.isNotBlank() }
            ?: if(target.isBlank()) t("Automatic Wi-Fi discovery", "اكتشاف تلقائي عبر Wi-Fi") else "$target:$controlPort"
        android.app.AlertDialog.Builder(this)
            .setTitle(t("Studio connection", "اتصال الاستوديو"))
            .setMessage("$lastStatus\n\n$endpoint")
            .setPositiveButton(t("Reconnect", "إعادة الاتصال")) { _,_ -> paused=false;connect() }
            .setNegativeButton(t("Close", "إغلاق"),null).show()
    }
    private fun beginStream(settings: StreamConfig) {
        if (!visible || paused) return
        try { startForegroundService(Intent(this, CameraCaptureService::class.java)); service?.start(settings, automatic = true) }
        catch (e: Exception) { renderStatus("Error: ${e.message}") }
    }
    private fun connectionDialog() {
        val dialog = Dialog(this)
        val content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(24), dp(24), dp(24), dp(24)); background = shape(0xff121e2d.toInt(), 22) }
        content.add(text(t("Connect your Studio", "اتصال الاستوديو"), 23f, Color.WHITE, true)); content.space(10)
        content.add(text(t("Auto-connect is recommended. Manual connection also verifies the PC before sending video.", "الاتصال التلقائي هو الافتراضي. الاتصال اليدوي يتحقق أيضاً من جاهزية الكمبيوتر قبل البث."), 13f, muted)); content.space(20)
        val automatic = Switch(this).apply { text = t("Find PC automatically", "اكتشاف الكمبيوتر تلقائياً"); isChecked = target.isBlank(); setTextColor(Color.WHITE); buttonTintList = ColorStateList.valueOf(accent) }
        content.add(automatic); content.space(14)
        val address = EditText(this).apply { hint = t("PC address, e.g. 192.168.1.10", "عنوان الكمبيوتر، مثلاً 192.168.1.10"); setText(target); setTextColor(Color.WHITE); setHintTextColor(muted); textSize = 14f; setSingleLine(); inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_URI }
        content.add(address); content.space(8)
        val port = EditText(this).apply { hint = t("Connection port (not video)", "منفذ الاتصال، وليس الفيديو"); setText(controlPort.toString()); setTextColor(Color.WHITE); textSize = 14f; inputType = android.text.InputType.TYPE_CLASS_NUMBER }
        content.add(port)
        fun enabled() { address.isEnabled = !automatic.isChecked; port.isEnabled = !automatic.isChecked; address.alpha = if (automatic.isChecked) .4f else 1f; port.alpha = address.alpha }
        automatic.setOnCheckedChangeListener { _, _ -> enabled() }; enabled(); content.space(15)
        val bitrate = Switch(this).apply { text = t("Stable bitrate (CBR)", "معدل بيانات ثابت CBR"); isChecked = cbr; setTextColor(Color.WHITE) }
        content.add(bitrate); content.space(16)
        content.add(button(t("Save & reconnect", "حفظ وإعادة الاتصال"), true) {
            val parsed = port.text.toString().toIntOrNull()
            if (!automatic.isChecked && (address.text.isBlank() || parsed == null || parsed !in 1..65535)) {
                address.error = t("Enter a valid PC address and connection port", "أدخل عنوان الكمبيوتر ومنفذ الاتصال الصحيح"); return@button
            }
            target = if (automatic.isChecked) "" else address.text.toString().trim()
            controlPort = if (automatic.isChecked) 5005 else parsed!!; cbr = bitrate.isChecked
            getPreferences(0).edit().putBoolean("cbr", cbr).apply()
            paused = false; dialog.dismiss(); connect()
        }, 50)
        dialog.setContentView(ScrollView(this).apply { addView(content); isFillViewport=false }); dialog.window?.setBackgroundDrawableResource(android.R.color.transparent)
        dialog.window?.setLayout(minOf(resources.displayMetrics.widthPixels - dp(32), dp(440)), minOf(resources.displayMetrics.heightPixels - dp(72), dp(520))); dialog.show()
        dialog.window?.setLayout(minOf(resources.displayMetrics.widthPixels - dp(32), dp(440)), minOf(resources.displayMetrics.heightPixels - dp(72), dp(520)))
    }
    private fun setSaver(enabled: Boolean) {
        saver = enabled; service?.setPowerSaver(enabled)
        black.visibility = if (enabled) View.VISIBLE else View.GONE
        preview.visibility = if (enabled) View.INVISIBLE else View.VISIBLE
        window.attributes = window.attributes.apply { screenBrightness = if (enabled) .01f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE }
        if (Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.apply {
                systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                if (enabled) hide(WindowInsets.Type.systemBars()) else show(WindowInsets.Type.systemBars())
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = if (enabled) 5894 else
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        }
    }
    override fun onStart() {
        super.onStart()
        // Opening the phone app is the user's intent to go live, including after
        // pausing in a previous visit. Permission denial still requires consent.
        visible = true; paused = false
        getSystemService(DisplayManager::class.java).registerDisplayListener(displayListener,main)
        bound = bindService(Intent(this, CameraCaptureService::class.java), connection, Context.BIND_AUTO_CREATE)
    }
    override fun onConfigurationChanged(newConfig: Configuration) { super.onConfigurationChanged(newConfig); syncRotation() }
    override fun surfaceCreated(holder: SurfaceHolder) { if (!saver) service?.setPreview(holder.surface) }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) { if (!saver) service?.setPreview(holder.surface); syncRotation() }
    override fun surfaceDestroyed(holder: SurfaceHolder) { service?.setPreview(null) }
    override fun onStop() {
        visible = false; pending = null; service?.onAutoReady = null; service?.onStatus = null; service?.setPreview(null)
        getSystemService(DisplayManager::class.java).unregisterDisplayListener(displayListener)
        if (service?.isForeground != true) service?.disableDiscovery()
        if (bound) unbindService(connection); main.removeCallbacksAndMessages(null); bound = false; service = null; super.onStop()
    }
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, results)
        if (requestCode == 10) {
            permissionPending = false; val settings = pending; pending = null
            if (results.firstOrNull() == PackageManager.PERMISSION_GRANTED && settings != null && !paused && visible) beginStream(settings)
            else { paused = true; service?.disableDiscovery(); renderStatus(t("Camera permission is required. Tap Connect to try again.", "إذن الكاميرا مطلوب. اضغط اتصال للمحاولة مجدداً.")) }
        }
    }
}


# Cortex Android Sender — Module 1

Standalone Kotlin Android application, Android 10/API 29 or newer. Camera2 feeds a hardware AVC encoder through its input Surface. There is no ImageReader, raw pixel buffer, software encoder fallback, CameraX, or CPU pixel conversion.

## Build

Open this directory in Android Studio and select an installed Android SDK 35. Use JDK 17+ and Gradle 8.14.3:

```powershell
.\gradlew.bat testDebugUnitTest assembleDebug
```

The APK is produced at `app/build/outputs/apk/debug/app-debug.apk`. A physical device is required to verify the camera and hardware codec path; emulator support is insufficient evidence of hardware encoding.

## Use

Auto-connect is the default: keep Studio open or minimized on the PC and open the phone app on Wi-Fi. It discovers Studio, handshakes, then starts after camera permission is granted. Opening the app again rearms automatic connection even after Pause. mDNS, LAN broadcast and a previously verified PC address are tried automatically. The status confirms decoded video at Studio rather than just local camera activity. See [discovery protocol and setup](../docs/discovery.md).

1. Put the phone and PC on the same reachable Wi-Fi network. Permit the selected UDP port through the PC firewall.
2. Open the app and grant camera permission when requested. No Start button or address is required. For manual fallback, open the sliders icon (connection settings), disable automatic PC selection and enter the PC address and **connection** port (default 5005); readiness is still verified before video starts.
3. Enable Power Saver to remove the preview target from the Camera2 session, hide its SurfaceView, show an opaque `#000000` overlay, lower brightness, and hide system bars. Tap the black screen to restore controls and preview.
4. Pause releases camera, codec and socket for the current visit. Reconnect or reopening the app resumes discovery. Streaming otherwise continues when the activity is backgrounded through a camera foreground service; return through the notification. Screen-off operation is not guaranteed across device power policies.

`SurfaceView` displays a service-owned GPU preview texture. Attaching/detaching the UI only changes the EGL display surface and repeating-request targets; the Camera2 session and encoder remain alive. Power Saver reduces preview/display work, but camera/encoding heat remains. Black pixels save most display energy on OLED devices; LCD backlights behave differently.

## Module boundaries

* `core/StreamConfig`: immutable 1920×1080 configuration; accepts only 30 or 60 fps.
* `capture/CameraCaptureService`: foreground service, camera selection, fixed AE range, capture-session ownership, preview attachment and lifecycle. Camera control is serialized on a HandlerThread.
* `encoding/HardwareEncoder`: hardware-only codec enumeration, capability checks, Constrained Baseline/Main H.264, surface input, zero B-frames, requested latency zero, supported CBR/VBR selection. Its drain thread copies only compressed access units before releasing codec buffers.
* `transport/UdpStreamer`: three-access-unit queue and background UDP sender. Overflow flushes queued work, suppresses dependent frames, and requests an IDR. Cached SPS/PPS are sent before each IDR so receivers can join/recover.
* `transport/H264Rtp`: RFC 6184 single-NAL/FU-A packetization; no raw H.264 datagrams, container, or TCP.
* `ui/MainActivity`: permission flow, destination, stream controls, preview and black overlay. No media processing.

Transport and drain threads are separate so network work cannot retain MediaCodec output buffers. CPU copies of **compressed H.264** are intentional and required by this socket implementation. Raw camera-to-encoder video travels through the Surface exclusively.

## Wire contract

* RTP version 2, dynamic payload type 96, H.264, clock rate 90000 Hz.
* RFC 6184 `packetization-mode=1`, single NAL or FU-A; maximum UDP payload 1200 bytes including the RTP header.
* Random SSRC, initial sequence and timestamp; timestamp derived from encoder presentation time. One timestamp per access unit; marker only on its final packet.
* SPS/PPS are carried in-band. IDR interval requested at one second. Receiver must reassemble complete access units and discard incomplete pictures after packet loss.
* Orientation uses an RFC 8285 one-byte extension (BEDE, local ID 1), quarter turns clockwise. No RTCP, retransmission, pairing, encryption, or remote keyframe feedback. mDNS and UDP readiness/heartbeat are separate control-plane modules. Network loss is recovered at subsequent IDRs; local queue overload requests an immediate IDR. Keep use to a trusted LAN.

For an initial PC smoke test, open `docs/receiver.sdp` with a compatible RTP/H.264 receiver listening on port 5004. For example, with native FFmpeg tools installed on the PC:

```powershell
ffplay -protocol_whitelist file,udp,rtp -fflags nobuffer -flags low_delay -framedrop -i docs/receiver.sdp
```

Do not share port 5004 between Studio and the test player. Encoded dimensions remain 1920x1080; Studio 0.6+ applies the RTP orientation metadata to its preview and output.

## Capability and latency limits

1080p/60 requires a normal Camera2 session advertising an AE range with upper rate 60 and a sufficient minimum encoder-surface frame duration. A fixed range is preferred; variable exposure ranges cannot guarantee fixed cadence. If regular-session 60 is unavailable, the app tries 1080p30 and displays the actual selected rate. Camera session configuration is the final check of the simultaneous preview/encoder combination. Changing rate reconnects the camera immediately.

Baseline profile and `KEY_MAX_B_FRAMES=0` prevent intentional B-frame encoding. `KEY_LATENCY=0` is an optional vendor hint, not a measured latency guarantee. Hardware codec flags and supported formats are checked, but actual rate, thermals, queue depth inside the driver, and glass-to-glass latency must be measured on a phone. Exposure and resource pressure can still affect delivered cadence.

## Validation

Unit tests exercise FU-A reconstruction, MTU limits, sequence wrap, marker semantics, timestamp rate, and Annex-B/AVCC parsing. On-device acceptance:

1. Test 1080p/30 and supported 1080p/60 independently; confirm explicit failure for unsupported configurations.
2. Decode on the PC; verify IDR, SPS/PPS, no B pictures, resolution, timestamps and measured cadence.
3. Toggle Power Saver repeatedly; verify ongoing RTP, preview disappearance/restoration, and a pure black display with hidden bars.
4. Stop/start repeatedly, background/foreground the activity, revoke permission, disconnect Wi-Fi, and contend for the camera. Verify useful errors and resource release.
5. Simulate receiver packet loss and sender overload. Confirm recovery at an IDR without accumulating seconds of delay.
6. Measure thermal/power and end-to-end latency on physical hardware. Black overlay does not promise to prevent heating.

API references: [MediaCodec Surface input](https://developer.android.com/reference/android/media/MediaCodec#createInputSurface()), [encoder format keys](https://developer.android.com/reference/android/media/MediaFormat), [Camera2 request controls](https://developer.android.com/reference/android/hardware/camera2/CaptureRequest), and [H.264 RTP packetization](https://www.rfc-editor.org/rfc/rfc6184).

## Android 0.7 UI and camera switching

`ui/CameraScreen` owns responsive viewfinder chrome; `ui/CameraIcon` supplies native vector controls with tooltips and accessibility labels. Portrait uses a bottom dock, landscape a side rail. Long-press a control for its label. Tap the status pill for connection details/reconnect. Sliders open settings; moon enables black screen; camera-with-arrows switches front/rear. Phone icons select orientation, numbers select fps. Landscape cycles orientation and fps.

Camera selection is saved. Capabilities are checked before stopping the current stream. Switching waits for the old camera to close, opens the new camera, starts a fresh hardware encoder/RTP session and requests an IDR. Studio may briefly show reconnecting. Front 1080p60 falls back to 30 when unavailable; the preview badge shows the actual rate. Unsupported 1080p configurations keep the current stream.

The preview shader removes duplicate sensor rotation already included in the SurfaceTexture transform. Encoded orientation metadata remains compatible with Studio 0.6. Raw frames still travel directly from Camera2 to the hardware codec Surface; preview uses GPU textures only.

Device acceptance: switch rear/front/rear at 30/60, check both portrait and landscape phone/PC images, then Home/return and black-screen/return. Build/unit/lint success does not establish device-specific camera behavior.


## Android 0.8 preview and responsive controls

Front-camera preview correction now uses the display-space rotation basis while encoded RTP orientation remains unchanged. Layout chooses its rail/dock before measuring weighted preview children, preventing collapsed landscape previews. Controls provide subtle press/status/entry motion and honor the system animator switch; the camera Surface itself is never animated or recreated by these transitions. Camera and network lifecycles remain service-owned. Verify both cameras at 0/90/180/270 degrees on a physical phone and compare the Windows receiver before claiming device acceptance.

## Android 0.8.1 rotation crash fix
Detach the retained pause label along with every other reused control before rebuilding the portrait dock. Robolectric SDK 35 tests exercise the actual Android ViewGroups through repeated portrait/landscape layouts without opening camera/network resources. Both new tests reproduce the child-already-has-parent IllegalStateException with the previous code and pass with the fix. The quality APK uses versionCode 9; distribution is signed with the existing project development key.


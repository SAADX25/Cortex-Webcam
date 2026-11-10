# Validation — 2026-11-02

## Desktop 0.10.1 — independent face/body samples (2026-11-05)

- Reproduced the dropped-body-update bug with a controlled tracker: changing
  arm/torso measurements while holding the face capture timestamp failed the
  regression before the pipeline fix.
- Face and body now have independent capture timestamps, adaptive filters and
  sample clocks. Body remains responsive when face inference is cached or lost;
  missing phone/body input eases toward rest. Arm toggles and input rotation
  reset the body state. Shoulder-based torso confidence survives hidden arms.
- Release native build and self-contained Windows UI publish succeeded. All 12
  native tests passed across the final runs, including real ONNX fixture
  inference, cached-sample/face-loss/body-loss regressions and actual Ren pixel
  checks for independent torso yaw, torso roll and elbow bend. The UDP
  auto-connect test needed execution outside the network-isolated sandbox.
- All 50 WPF layout/focus checks passed at 1080x720, 1280x800, 1440x900 and
  1920x1080. Existing uncommitted tracking and UI changes were preserved.
- No Android update is required by this change. These automated and rendered
  checks do not establish tracking accuracy for the user's live physical motion.
- Launched the packaged Desktop 0.10.1 and observed the physical Samsung
  SM-S928B reconnect at 1920x1080, approximately 30 fps and zero packet gaps.
  Original camera framing was close and did not show all elbows/wrists.

Built with MSVC 19.51 / Visual Studio 2026, Windows SDK 10.0.26100.0, C++20, and FFmpeg 9.0.2 shared headers/import libraries/DLLs. Downloaded development archive SHA-256 verified against its publisher's checksum: `4d2060a8b34a940aa47d785142055bb92a63053781e55f2ace4546edd519a8f5`.

* Native receiver and diagnostic executable: build passed.
* CTest `rtp_tests`: five transport test groups passed.
* CTest `manager_tests`: source routing, switching, PiP selection, freshness and device checks passed with fake sources.
* Actual D3D11VA loopback: 1920×1080/30, eight-second synthetic sender, 216 GPU NV12 frames decoded, zero reported decode errors. 388 missing RTP packets detected during startup; later logged intervals showed no additional gaps and approximately 30 frames/second.
* Actual D3D11VA loopback: 1920×1080/60, eight-second synthetic sender, 434 GPU NV12 frames decoded, zero reported decode errors. 168 missing RTP packets detected during startup; later logged intervals showed no additional gaps and approximately 60 frames/second.
* After sender exit, scene availability expired rather than indefinitely retaining a frozen image.

These tests prove that received H.264 was decoded to D3D11/NV12 surfaces on this machine, with no CPU fallback or readback path. They do not establish zero startup loss, glass-to-glass latency, arbitrary GPU compatibility, sustained thermal behavior, physical-phone/Wi-Fi interoperability, USB camera capture, or rendered PiP output.

The development sandbox isolated loopback traffic; the successful network smoke tests ran with approved network access. Reproduce with `scripts/SmokeTest.ps1`; generated logs are under `build/smoke/`.

## Automatic discovery increment

* Windows build and all three CTest executables passed, including new control protocol tests for readiness, malformed requests, peer contention, lease renewal and expiry.
* Native Windows mDNS registration succeeded. The real broadcaster answered a multicast `_cortexcam._udp.local` PTR query.
* UDP handshake returned the exact nonce, H.264 geometry/frame rate and video port; a second loopback peer received BUSY while the first lease was active.
* Combined handshake-to-media smoke test decoded 96 GPU NV12 frames from a four-second 1080p/30 synthetic H.264 sender with zero decode errors. Startup packet loss was recovered at an IDR, as in the earlier receiver tests.
* Android build, RTP/handshake unit tests and lint passed. Discovery on a physical Android device and actual access-point multicast behavior have not been validated here.

Reproduce with `scripts/DiscoverySmokeTest.ps1 -FFmpegExe <path-to-ffmpeg.exe>`; logs are under `build/discovery-smoke/`. See the shared [discovery setup](../../docs/discovery.md) for ports and lifecycle behavior.

## Processing and virtual-output increment

* Native build and all four CTest executables passed, including real D3D11 pixel tests for NV12 array-slice conversion, green removal, foreground preservation, alpha/opaque export, solid/image replacement, PiP compositing and UnityCapture-compatible shared-memory receipt.
* Actual ONNX Runtime 1.24.4 / DirectML 1.15.4 RVM FP32 inference passed across five advancing frames. Profiling recorded 1,255 DirectML node events and 45 CPU node events; the CPU events were small int64 shape `Concat`/`Slice` operations. Recurrent outputs are bound back as GPU inputs.
* Profiled model-run durations were approximately 1.67 s cold, then 50.1, 55.6, 49.5 and 42.4 ms. This is not a 30/60 fps AI result; shader preprocessing, I/O transfers and compositing add work. The reduced-size AI tensor bridge is explicitly documented.
* A local blue H.264 High-profile clip with B pictures decoded on the shared D3D11VA device, composited and looped successfully, including delayed-frame draining.
* Integrated 1080p/30 RTP-to-render smoke test decoded 216 frames with zero reported decode errors. Startup packet loss recovered at IDR, as before. With no camera client installed/opened, output correctly reported inactive rather than pretending to deliver frames.
* Pinned upstream UnityCapture 32/64-bit DirectShow filter binaries and an administrator registration/unregistration script are provided. Registration was not performed. Zoom/Discord/OBS acceptance and transparent capture negotiation remain untested.

See [processing/output setup](processing-output.md) for commands, alpha behavior, supported background codecs and installation.

## Desktop 0.5: physical Samsung H.264 interoperability

- The user's Galaxy S24 Ultra was discovered at 192.168.1.21 and sent RTP without observed sequence gaps. The initial failure was after transport: SPS/PPS/IDR and access units arrived, but every keyframe failed GPU initialization.
- Actual FFmpeg diagnostics reported H.264 profile 66 (plain Baseline), level 40, with an H.264 DXVA decoder GUID available. FFmpeg 9.0.2's DXVA profile list accepts Constrained Baseline/Main/High and excludes plain Baseline.
- The receiver now enables `AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH` only for plain Baseline during hardware-format negotiation. Other profiles keep ordinary checks. `AV_PIX_FMT_D3D11` remains the only accepted decoded format; there is no CPU fallback. This does not implement unsupported Baseline coding tools such as FMO/ASO.
- With the existing Android 0.4 APK unchanged, the same physical phone produced an actual 1920x1080 live preview at approximately 59 incoming fps. At least 1,688 decoded frames were observed, with zero decode errors and zero observed packet gaps during that sample.
- A synthetic single-frame plain Baseline fixture now reproduces this profile label in the hardware-decoder regression test. It contains no camera footage. Source/packet diagnostics store only counters and decoder error text in `logs/receiver-status.log`; successful live video is not recorded.
- The counter explanation is available in tooltips and the package instructions. Physical Zoom/Discord/OBS delivery and end-to-end latency are not established by the preview test.

FFmpeg references: [DXVA profile selection](https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavcodec/dxva2.c), [profile compatibility flag](https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavcodec/avcodec.h).

## Desktop/Android 0.6: preview lifetime and orientation

- The phone's Camera2 session now holds a service-owned `SurfaceTexture` preview target and the MediaCodec input Surface throughout a stream. Activity surfaces are only EGL display windows; attaching/detaching them never recreates the camera session or codec. Background and Black screen requests retain only the encoder target, leaving capture/encoding active while eliminating preview rendering. A camera foreground service, bounded compressed queue and stream-lifetime partial wake lock support background delivery; a notification action explicitly stops capture.
- Android selects hardware Constrained Baseline when advertised, otherwise hardware Main, always requesting zero B frames and level 4.1/4.2. This avoids depending on the legacy plain-Baseline profile exception for new senders. Compressed partial MediaCodec buffers are joined before RTP packetization. Raw pixels are never read by CPU and the encoder stays directly attached to Camera2.
- RFC 8285 one-byte RTP extension local ID 1 carries a single byte: clockwise quarter turns 0..3. Its presence adds 8 header bytes; the 1200-byte UDP MTU remains bounded. A missing extension defaults to zero rotation for older senders. Codec dimensions remain 1920x1080.
- Windows retains rotation by RTP timestamp through decode, rotates RGBA on GPU before matting/compositing, and resizes the program output to 1080x1920 or 1920x1080. Foreground aspect ratio is preserved. A virtual camera consumer with fixed negotiated dimensions may need to reopen the camera after an aspect change.
- Android 0.6 layout replaces the large static headline with compact connection, framing, frame-rate and explicit pause controls. Auto follows display rotation, Portrait/ Landscape lock the corresponding sensor orientation. Permission prompts are serialized for a first install.
- Added tests cover orientation calculation, partial access-unit bounds/recovery, extension framing/MTU/reassembly, GPU rotation of asymmetric colored quadrants, and portrait output dimensions. Background Home/return, EGL preview orientation and Android hardware profile negotiation still require physical-device acceptance of the new APK; successful builds alone are not that evidence.

# Cortex Studio — GPU webcam pipeline

Native C++20 Windows module implementing **Source A first**: Android-compatible RTP/UDP reception, H.264 depacketization, hardware-only FFmpeg D3D11VA decoding, and GPU-frame publication. Includes the unified `IVideoSource` contract and working `VideoSourceManager` routing/PiP scene selection for both source identities.

D3D11 chroma key/PiP compositing, optional RVM/DirectML matting, image/looping video backgrounds, alpha export and UnityCapture output are implemented. The WPF Desktop UI hosts a native GPU preview through `CortexStudioNative.dll`. Desktop 0.4 keeps reception armed automatically and retries temporary network failures; open the Android app to go live. See [processing/output setup](docs/processing-output.md), including one-time virtual-camera registration. The physical USB capture adapter remains separate work. Tests use a fake USB source for routing and PiP pixel checks.

## Build and run

Studio publishes mDNS and accepts automatic phone handshakes, with a renewable peer lease. The default control port is UDP 5005; video remains UDP 5004. See [automatic connection setup](../docs/discovery.md).

Requirements: Windows 10/11 x64, hardware D3D11 H.264 decoder and current GPU driver, Visual Studio C++ desktop tools, CMake 3.25+, and an FFmpeg shared **development** distribution with headers, import libraries and DLLs. An `ffmpeg.exe` alone is insufficient.

```powershell
cmake -S . -B build -A x64 -DFFMPEG_ROOT=C:/path/to/ffmpeg-shared
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\cortex-receive.exe 5004 192.168.1.42
```

The optional second argument restricts packets to the phone's numeric IPv4 address. A third argument limits runtime in seconds, for example `5004 192.168.1.42 30`. The console reports decode, scene, render and virtual-output statistics. Chroma key and virtual output are enabled by default; use `--filter none --no-output` for decoder diagnostics. `--help` lists options.

Desktop uses automatic discovery by default. Manual Android connection takes a PC address and control port; the handshake returns the video port. Permit the selected ports through Windows Firewall on the intended LAN profile. `bindAddress` can restrict the listening interface. The transport is unencrypted and unauthenticated, intended for a trusted LAN. The CLI does not change firewall rules; Desktop's optional Allow phone helper requests administrator consent for an executable-specific local-subnet UDP exception.

CMake copies the selected distribution's DLLs alongside the diagnostic executable. Use compatible x64 headers/libraries/DLLs from the same package. The locally tested Gyan shared package is GPLv3; dependency choice affects redistribution licensing. Dependencies and build output are excluded from source control.

## Pipeline and file layout

```text
UDP socket → bounded RTP reorder → RFC 6184 depacketizer → Annex-B access unit
    → FFmpeg H.264 parser/decoder + D3D11VA → retained GPU NV12 frame
    → latest-frame slot → VideoSourceManager → D3D11 chroma/AI compositor
    → GPU alpha export → bounded readback → UnityCapture virtual camera

include/cortex/
  core/VideoFrame.h                 Common GPU frame and timestamp contract
  gpu/GpuDevice.h                   Shared D3D11 device/context and lock
  transport/H264RtpReceiver.h        Portable RTP/access-unit processing
  transport/UdpH264Source.h          Source A lifecycle, socket and publication
  decoding/HardwareH264Decoder.h     FFmpeg adapter; no FFmpeg types escape
  sources/IVideoSource.h             Common phone/USB source interface
  sources/VideoSourceManager.h       Source selection and PiP scene snapshots
src/                               Implementations in matching layers
apps/receiver_main.cpp              Diagnostic controller
tests/                             RTP and manager tests
scripts/SmokeTest.ps1               Actual GPU loopback decode test
docs/usb-source-contract.md         Next source adapter requirements
```

Camera capture, transport, decoding, scene routing, filtering, UI and output remain separate layers. The console app composes these services.

## GPU frame contract

Every source publishes `shared_ptr<const VideoFrame>` containing an **NV12 D3D11 texture**, array slice, visible dimensions, source timestamp, local receive/ready times, color matrix/range, and backing ownership. All sources share the same `GpuDevice`. Decode, color conversion and compositing stay on GPU. Staging readback is limited to UnityCapture output and optional reduced-resolution AI I/O; no CPU video pixel conversion is used.

FFmpeg's `AVFrame` is retained along with the COM texture: retaining a texture alone does not retain its decoder-pool slot. The frame owner also retains the GPU lock/device through teardown. Consumers must honor `arraySlice` and visible dimensions; the underlying texture may have padded dimensions. Decoder textures need not have shader-resource binding; the compositor uses D3D11 video processing to produce shader-readable RGBA images.

The decoder binds FFmpeg to the application's D3D11 device, selects only `AV_PIX_FMT_D3D11`, rejects non-NV12 output, uses one codec thread, requests low-delay behavior, and provides extra hardware surfaces. There is no software fallback. D3D11VA works through the installed GPU vendor's video driver, including supported NVIDIA hardware; this module does not expose a separate CUDA/NVDEC backend or incur CUDA/D3D interop.

All immediate-context commands must use `GpuDevice::mutex`, also supplied to FFmpeg's D3D11 hardware context. Frames/snapshots are immutable. Keep at most the latest scene and a small number of in-flight GPU frames; retaining arbitrary frame history can exhaust a decoder surface pool. Device loss is surfaced as a failed source and requires rebuilding the shared GPU and dependent sources.

## Switching and PiP

The manager keeps sources running while changing routing:

```cpp
manager.add(SourceId::Phone, phoneSource);
manager.add(SourceId::UsbWebcam, usbSource); // Future Media Foundation adapter.
manager.startAll();
manager.select(SourceId::UsbWebcam);
manager.setPictureInPicture(SourceId::Phone, {0.68f, 0.68f, 0.30f, 0.30f});
auto scene = manager.snapshot();
```

Each render tick pulls a snapshot: background first, optional inset second. Selection never stops/reopens a camera. Rectangles are normalized and validated. Duplicate self-PiP is suppressed. Frames older than 250 ms are omitted by default, so a dead phone does not become an indefinite frozen image. The compositor is responsible for aspect fitting, background fill, scaling, GPU color conversion and drawing these layers.

`start()` launches asynchronous source startup; inspect `status()` for bind/device/decoder failures. `Running` means the receive service is ready, not that a valid video frame has arrived. `latestFrame()` and snapshot availability indicate that separately. Lifecycle calls belong on controller threads; source workers do not call UI code.

## Wire compatibility and loss policy

* IPv4 UDP; RTP version 2; payload type 96; H.264/90000; packetization mode 1.
* Handles single NAL, FU-A and STAP-A. Android uses single NAL/FU-A, 1200-byte RTP datagrams, in-band SPS/PPS, no B pictures, and final-packet access-unit markers.
* RTP header extensions, CSRCs and padding are bounds-checked. Datagrams above 2048 bytes, malformed headers and unexpected payload types are rejected.
* Reordering is bounded to a 64-packet sequence window and an 8 ms wait by default. Missing sequence numbers invalidate incomplete pictures and dependent frames until a complete IDR with cached SPS/PPS arrives. The decoder is flushed on recovery.
* Corrupt decoded pictures/invalid bitstreams also re-enter IDR recovery. Unsupported hardware formats/device failures fail explicitly.
* Sequence and timestamp wrap are handled. A new SSRC is accepted after two seconds without an accepted packet from the current SSRC, clearing previous codec configuration. This accommodates sender restarts without bouncing between two active senders.
* Access-unit assembly is capped at 4 MiB; oversized frames are discarded. The OS receive buffer is requested at 256 KiB, the receive worker polls shutdown every 5 ms, and sources publish only their latest frame. There is no unbounded media queue. OS scheduling and hardware calls can exceed these requested intervals.

The current receive worker performs hardware decode submission synchronously, so GPU startup/stalls can cause socket loss rather than create an application backlog. Recovery waits for the next IDR (Android requests a one-second interval). Receiver feedback/keyframe requests, retransmission, RTCP, adaptive buffering, audio, HEVC and multi-phone sessions are outside this increment.

RTP timestamps are source-relative. `receivedAt` is local access-unit receive completion, not phone capture time or kernel socket arrival. `readyAt - receivedAt` helps inspect receiver processing; it is not glass-to-glass latency. No cross-device clock synchronization or latency claim is implied.

## Verification

```powershell
# Portable transport tests without FFmpeg/GPU dependencies:
cmake -S . -B build-core -DCORTEX_BUILD_RECEIVER=OFF
cmake --build build-core --config Debug
ctest --test-dir build-core -C Debug --output-on-failure

# Real GPU decode with synthetic PC-encoded H.264 over loopback:
.\scripts\SmokeTest.ps1 -FFmpegExe C:/path/to/ffmpeg.exe -Fps 30
.\scripts\SmokeTest.ps1 -FFmpegExe C:/path/to/ffmpeg.exe -Fps 60 -Port 55006
```

The synthetic test fixture uses software encoding on the PC only. Receiver output is accepted only as D3D11/NV12 GPU frames. Tests cover reorder/FU-A reconstruction, loss and IDR recovery, timestamp/sequence wrap, duplicates, malformed headers, missing markers, STAP-A, sender restart, source switching, PiP routing, stale-frame suppression and shared-device validation. Physical Android/Wi-Fi interoperability and USB capture still need device testing.

References: [FFmpeg hardware decode example](https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c), [FFmpeg D3D11 context and frame ownership](https://github.com/FFmpeg/FFmpeg/blob/master/libavutil/hwcontext_d3d11va.h), [RFC 6184](https://www.rfc-editor.org/rfc/rfc6184).

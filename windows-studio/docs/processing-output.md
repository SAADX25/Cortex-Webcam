# GPU processing and virtual-camera output

The receiver now runs D3D11 chroma key, image/video replacement, optional RVM/DirectML matting, PiP pixel compositing and UnityCapture output. The physical USB capture adapter and a graphical Studio UI remain separate work. Console controls and the source-manager API drive this stage.

## Run

From `windows-studio`:

```powershell
.\build\Debug\cortex-receive.exe --filter chroma --background-image 'C:\Images\studio.jpg'
.\build\Debug\cortex-receive.exe --filter ai --model '.\.deps\onnx\rvm_mobilenetv3_fp32.onnx' --background-video 'C:\Videos\studio.mp4'
.\build\Debug\cortex-receive.exe --filter chroma --transparent --virtual-alpha
.\build\Debug\cortex-receive.exe --filter none --no-output
```

Existing positional parameters remain `video-port phone-IPv4 duration-seconds control-port`; named options follow them or work by themselves. `--fps 30|60` controls render cadence. `--device 0` selects the first UnityCapture device; nonzero indices need matching registrations. Chroma key and virtual output are enabled by default. `--help` lists options.

Live console keys: **C** chroma, **A** toggle AI when a model was supplied, **O** removal off, **T** transparent compositing, **Q** quit. `FilterSettings` exposes similarity, edge softness, spill strength and solid background color for a future UI. Settings/render/output calls belong on one render thread; decoder and background workers publish latest frames independently.

## Register the virtual webcam once

The pinned UnityCapture 32-bit and 64-bit DirectShow filters are included under `third_party/unitycapture/Install`. No Unity installation is needed. Run **64-bit PowerShell as Administrator**:

```powershell
.\scripts\InstallVirtualCamera.ps1
```

Administrator rights are required for system COM/DirectShow registration. Registration is not performed automatically. The script verifies DLL hashes and registers both client architectures. Keep those files in place; unregister before moving/deleting them:

```powershell
.\scripts\InstallVirtualCamera.ps1 -Uninstall
```

Start Studio and choose **Unity Video Capture** in Zoom/Discord or an OBS Video Capture Device source. Restart the receiving application if it cached its device list. Request 1920×1080 and a matching frame rate. This is a DirectShow virtual webcam, not the OBS-owned IPC protocol or a kernel driver. Applications that restrict DirectShow/virtual devices may require another output backend.

`virtualInactive` is normal until a client opens the device. `virtualSent` counts frames accepted on demand; `virtualSkipped` records contention, unread frames and discarded readbacks. A three-slot readback ring publishes the newest completed frame, skipping under backpressure. Normal publication adds approximately one render tick rather than an unbounded queue. The sender uses bounded mutex waits.

## GPU and alpha contract

The video processor converts NV12 to RGBA entirely on GPU, preserving decoder array slices and visible dimensions. It respects range and BT.601/709/2020 metadata. Missing metadata defaults to limited range and BT.709 for HD or BT.601 for SD. P010/HDR tone mapping are not implemented.

The chroma shader keys green in chroma space, softens edges and suppresses spill. Compositing uses premultiplied alpha and draws background, main source, then PiP. Camera layers are aspect-fitted; background images/video fill the output. HLSL is embedded during CMake configuration, so runtime shader paths do not depend on the working directory.

`render()` can preserve premultiplied alpha. `exportFrame(true)` converts it to straight RGBA on GPU for alpha-aware consumers. The default export flattens transparency over black and sets alpha to one. Normal Zoom/Discord webcam negotiation generally produces opaque video; alpha should only be expected with a compatible capture format/application such as an appropriately configured OBS source.

WIC decodes static images once at load time, up to 4096×4096. Background videos currently require **8-bit H.264** in a supported FFmpeg-demuxed container such as MP4/MKV/MOV. The compressed bitstream is adapted to Annex-B and decoded with the existing shared-device D3D11VA module. Playback is timestamp-paced; EOF drains delayed B pictures before looping. Audio is ignored. Unsupported codecs or GPU formats fail explicitly. Only one latest decoded background frame is retained.

## Optional RVM on DirectML

```powershell
.\scripts\GetProcessingDependencies.ps1
cmake -S . -B build -A x64 -DFFMPEG_ROOT='C:/path/to/ffmpeg-shared' -DONNXRUNTIME_ROOT="$PWD/.deps/onnx/runtime" -DDIRECTML_ROOT="$PWD/.deps/onnx/directml"
cmake --build build --config Debug
```

The script pins/checks SHA-256 for ONNX Runtime DirectML 1.24.4, DirectML 1.15.4 and the official RVM MobileNetV3 FP32 ONNX weight. CMake copies matching DLLs beside executables. Leave `ONNXRUNTIME_ROOT` unset for a chroma/output-only build; AI then reports that the backend is unavailable. Runtime/model files remain local assets under ignored `.deps`. RVM and the selected full FFmpeg package are GPLv3; retain upstream licenses when redistributing. UnityCapture attribution and interface modifications are recorded in its third-party directory.

A compute shader prepares normalized RGB NCHW at **512×288**. RVM uses `downsample_ratio=1`; compositing upsamples its alpha mask. This trades full-resolution edge detail for reduced inference work. Recurrent states stay in DirectML memory independently per source, resetting on discontinuities/mode changes. Duplicate render ticks reuse their previous mask. DML is selected on the decoder's DXGI adapter; there is no CPU-only retry or CUDA backend.

AI currently has an explicit reduced-tensor bridge: D3D11 preprocessing → staging readback → DML upload, followed by alpha readback/upload. Pixel conversion stays in shaders, but this is **not zero-copy AI I/O**. ORT runs small shape/control operators on CPU; matting computation uses DirectML. UnityCapture likewise requires the final GPU RGBA frame to be read back and copied into shared memory. Chroma mode avoids AI transfers.

A profiled five-frame check on this machine recorded a roughly **1.67 s cold run** and **42–56 ms warm model runs**, excluding additional pipeline work. AI therefore does **not** promise 30/60 fps on this host. Rendering uses latest frames instead of accumulating backlog. Enable placement/timing traces with `CORTEX_ORT_PROFILE` set to a writable filename prefix before running:

```powershell
$env:CORTEX_ORT_PROFILE="$PWD/build/rvm-profile"
.\build\Debug\processing_tests.exe '.\.deps\onnx\rvm_mobilenetv3_fp32.onnx'
```

## Verification

CTest includes real GPU pixel checks for NV12 array-slice conversion, green removal, preserved foreground, alpha/opaque export, solid/image replacement, PiP and UnityCapture-compatible IPC. Optional processing-test arguments run actual RVM and a local video fixture. The video test expects a short blue H.264 clip and verifies decode, composition and looping, including B-frame draining:

```powershell
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\processing_tests.exe '.\.deps\onnx\rvm_mobilenetv3_fp32.onnx' '.\build\background-test.mp4'
```

IPC tests do not register the filters. Registration and end-to-end behavior inside Zoom/Discord/OBS have not been exercised here. Physical phone/Wi-Fi operation, human-edge quality, prolonged thermals and glass-to-glass latency still require device testing.

References: [UnityCapture](https://github.com/schellingb/UnityCapture), [RVM model interface](https://github.com/PeterL1n/RobustVideoMatting/blob/master/documentation/inference.md), [ORT DirectML](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html), [ORT device tensors](https://onnxruntime.ai/docs/performance/device-tensor.html).

# Source B: Media Foundation adapter contract

Implement a separate `UsbWebcamSource : IVideoSource` under `capture/`, not inside the UDP receiver or manager. It must use the manager's `shared_ptr<GpuDevice>` and publish the existing `VideoFrame` contract.

1. Enumerate Media Foundation video-capture devices and let the controller select a symbolic link. Do not silently open a different camera. Negotiate supported dimensions/frame rate independently of the phone's 1080p mode.
2. Use an `IMFDXGIDeviceManager` reset to the shared D3D11 device and associate it with the source reader. Prefer native NV12; normalize supported alternative camera formats to NV12 on the GPU. Request hardware transforms and validate actual sample residency, rather than assuming a request guarantees GPU output.
3. Extract textures with `IMFDXGIBuffer`, retaining the `IMFSample` or an owned GPU surface until downstream use completes. Preserve subresource/array slice and visible dimensions. System-memory samples need an explicit adapter policy; never pass CPU pointers disguised as GPU frames.
4. Use GPU conversion for YUY2/RGB sources, and a separate decoder for compressed camera formats when needed. Declare unsupported combinations clearly. CPU frame conversion is not part of the shared contract.
5. Carry camera colorimetry/range and normalize its 100 ns timestamp to source-relative 90 kHz. Use local monotonic times for freshness, not for cross-source capture-time synchronization.
6. Publish only the newest frame; keep the device running across manager selection changes. Implement stop/cancellation, device unplug, permission denial and reconfiguration without callbacks into UI objects.

The manager already supports `SourceId::UsbWebcam`; register this adapter to enable actual camera switching and two-camera PiP scene selection. Pixel compositing will be implemented separately in the GPU filter/output pipeline.

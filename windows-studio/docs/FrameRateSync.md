# Frame-rate synchronization

Cortex Studio 0.8.1 and Sender 0.9 support two-way camera frame-rate control. The top-right connection button is the single entry point for connection settings. The Windows inspector shows pending requests, confirmed camera FPS, legacy-app incompatibility and hardware fallback separately from measured receive FPS.

## Wire contract

Discovery and video statistics retain the CORTEX/1 contract. A capable phone sends:

```text
CORTEX/2 HELLO <nonce> H264 1920 1080 <phone-fps> <acknowledged-command-id>
CORTEX/2 READY <nonce> <video-port> H264 1920 1080 <requested-fps> <command-id>
```

Only the currently leased peer receives commands. Nonces, dimensions, rates, ports, message length and numeric command identifiers are validated. Rates are limited to 30 and 60. Connected UDP sockets on Android bind replies to the selected endpoint.

A Windows action creates a new command identifier. The phone acknowledges it and reconfigures Camera2/MediaCodec while retaining the discovery lease. Windows adopts the reported effective rate only after acknowledgement. If 60 is unsupported, the phone reports its 30 fps fallback. Completed commands are retired, preventing retries and preventing an old Windows setting from overwriting a later phone-side change or a reconnect.

Phone-side changes update the existing lease rather than restarting discovery. Replies overtaken by a local frame-rate change are discarded. Pipeline transitions wait for the previous CameraDevice.onClosed callback before reopening the camera, including consecutive changes while a device is already closing.

New phones fall back to CORTEX/1 for older PCs. New PCs accept older phones but expose a clear update requirement instead of pretending to control their camera FPS.

## Checks

- `handshake_tests`: validation, lease contention, command acknowledgements and legacy compatibility.
- `studio_autoconnect_tests`: UDP integration covering requests, acknowledgement, phone-side changes, command retirement and capability fallback.
- Android `HandshakeProtocolTest`: matching v2 messages and rejection of unrelated or malformed commands.
- `CortexStudio.LayoutTests`: four window sizes, focus/tab state, sync feedback presence and the 3320-byte managed/native state ABI.

Changing camera FPS briefly reopens the capture pipeline. It does not guarantee that processing, especially AI matting, delivers the same measured FPS. Physical-device acceptance requires the updated APK; a passing protocol test alone is not a hardware-camera test.

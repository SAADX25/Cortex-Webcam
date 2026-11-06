# Cortex Webcam

**Cortex Webcam** is a high-performance, low-latency GPU webcam streaming pipeline that turns Android devices into professional virtual webcams for Windows.

---

## Architecture Overview

```
+---------------------------+             RTP / UDP (H.264)             +----------------------------------+
|      Android Sender       |  ======================================>  |          Windows Studio          |
|  (Camera2 -> HW Encoder)  |        Auto-Discovery / Handshake         |   (HW D3D11VA -> GPU Filters     |
|   UDP Control Port: 5005  |  <------------------------------------->  |    -> UnityCapture Virtual Cam)  |
+---------------------------+                                           +----------------------------------+
```

### Components

1. **[Android Sender](android-sender/)**
   - Pure hardware pipeline using Android `Camera2` feeding MediaCodec AVC encoder directly via Surface.
   - Low-latency RTP packetization over UDP.
   - Zero-configuration automatic connection via mDNS, LAN broadcast, and handshake protocol.
   - Full camera controls: exposure, white balance, zoom, resolution, focus, and camera flipping.

2. **[Windows Studio](windows-studio/)**
   - Native C++20 backend utilizing hardware FFmpeg D3D11VA decoding.
   - GPU-accelerated compositing and filtering (Direct3D 11): Chroma Key, PiP (Picture-in-Picture), AI Background Matting (DirectML / RobustVideoMatting), and custom HLSL shaders.
   - DirectShow Virtual Camera integration via UnityCapture.
   - Modern WPF Desktop Studio user interface (`CortexStudio`).

3. **[Documentation](docs/)**
   - Protocol and handshake specifications (`docs/discovery.md`).
   - Frame rate synchronization and output contracts.

---

## Project Layout

Open **`Cortex Webcam.code-workspace`** in VS Code to see each project and the libraries as separate roots, with build outputs and caches hidden.

```text
Cortex Webcam/
├── android-sender/        Android app (Kotlin): capture, encoding, transport, discovery, ui
├── windows-studio/        Windows Studio (C++20 + WPF)
│   ├── include/cortex/    Public headers: core, gpu, transport, decoding, sources,
│   │                      discovery, tracking, avatar, processing, output, ui
│   ├── src/               Implementations (same folders as include/)
│   ├── apps/              Console executables (cortex-receive, cortex-motion)
│   ├── ui/                WPF desktop app (CortexStudio)
│   ├── shaders/           HLSL
│   ├── tests/             C++ tests + fixtures
│   ├── scripts/           Build, packaging and dependency scripts
│   ├── third_party/       Bundled libraries
│   ├── .deps/             Downloaded libraries (git-ignored)
│   └── patches/           Historical patches
├── vrm-viewer/            Unity VRM avatar viewer
├── assets/                Shared avatar assets (Live2D Ren)
├── docs/                  Protocol and system docs
└── dist/                  Release builds (git-ignored)
```

---

## Getting Started

### Android Sender Setup
- **Requirements**: Android 10+ (API 29+), JDK 17+, Android SDK 35.
- Open `android-sender/` in Android Studio or build with:
  ```powershell
  cd android-sender
  .\gradlew.bat assembleDebug
  ```

### Windows Studio Setup
- **Requirements**: Windows 10/11 x64, Visual Studio C++ desktop tools, CMake 3.25+, .NET 8.0 SDK, and FFmpeg shared development libraries.
- Build instructions:
  ```powershell
  cd windows-studio
  cmake -S . -B build -A x64 -DFFMPEG_ROOT="C:/path/to/ffmpeg-shared"
  cmake --build build --config Release
  ```
- Refer to [windows-studio/README.md](windows-studio/README.md) for detailed build and packaging scripts.

---

## License

Refer to component licenses in their respective directories.


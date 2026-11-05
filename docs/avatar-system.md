# Cortex Avatar System

The Avatar system allows Cortex Studio to replace the camera feed with either an asset-free, GPU-rendered procedural avatar or a Live2D Cubism model, driven by either an animated test pose or real-time camera face tracking.

## Architecture

```
                  ┌──────────────────────┐
                  │    Camera Feed       │ (Phone / Webcam)
                  └──────────┬───────────┘
                             │
                             ▼
┌────────────────────────────────────────────────────────┐
│                   VideoProcessor                       │
│                                                        │
│  [Avatar Disabled]                                     │
│     Camera Frame ──► Chroma / AI Matting ──► Composite │
│                                                        │
│  [Avatar Enabled]                                      │
│     Motion Input:                                      │
│       • Test Motion ──► AvatarFilter::testPose()       │
│       • Face Tracking ──► BuiltinFaceTracker::track()  │
│                                                        │
│     Rendering Path:                                    │
│       • Mode 0 (Test): AvatarFilter (HLSL GPU Shader)  │
│       • Mode 1 (Cubism): Live2D Cubism D3D11 Model     │
└────────────────────────────┬───────────────────────────┘
                             │
                             ▼
               ┌───────────────────────────┐
               │ Direct3D 11 Render Target │
               └─────────────┬─────────────┘
                             │
                 ┌───────────┴───────────┐
                 ▼                       ▼
      ┌────────────────────┐  ┌─────────────────────┐
      │  Studio Preview    │  │ UnityCapture Output │
      │  (DirectX SwapChain│  │ (Virtual Camera IPC)│
      └────────────────────┘  └─────────────────────┘
```

## Key Components

### 1. `AvatarFilter` (Built-in GPU Procedural Avatar)
- **Shader-based**: Rendered entirely through DirectX 11 via the `AvatarPreview` pixel shader in `pipeline.hlsl`.
- **Zero Assets Required**: Operates without any external files, model packages, or network assets.
- **Dynamic Features**: Draws an anti-aliased elliptical head, eye sockets, pupils tracking horizontal and vertical gaze, and an animated mouth opening/closing based on pose parameters.
- **Isotropic Coordinates**: Automatically adapts to portrait or landscape orientations without distortion.

### 2. Live2D Cubism Integration (`CubismAvatar`)
- **Native SDK Hook**: Integrates with the Live2D Cubism Native SDK when compiled with `CORTEX_HAS_CUBISM` via CMake (`-DCUBISM_SDK_ROOT=...`).
- **Parameter Mapping**: Automatically translates `AvatarPose` properties into standard Cubism parameter IDs:
  - `yaw` → `ParamAngleX`
  - `pitch` → `ParamAngleY`
  - `roll` → `ParamAngleZ`
  - `eyeOpen` → `ParamEyeLOpen` & `ParamEyeROpen`
  - `gazeX` / `gazeY` → `ParamEyeBallX` & `ParamEyeBallY`
  - `mouth` → `ParamMouthOpenY`
- **Graceful Fallback**: When the Cubism SDK is not compiled in or no model file is loaded, the pipeline falls back to the built-in GPU test avatar with an informative status banner.

### 3. Motion Sources
- **Procedural Test Animation**: Generates continuous trigonometric cycles (sine/cosine waves) simulating natural head sway, breathing, blinking, and mouth movements. Allows verifying the rendering and virtual camera pipelines without a physical camera or phone connected.
- **Camera Tracking**: The packaged ONNX tracker analyzes reduced-size upright camera frames with OpenSeeFace face/pupil models and MediaPipe upper-body models. Face and body inference run on separate workers. Perspective head fitting and speed-adaptive smoothing drive `AvatarPose`; the built-in tracker remains a fallback when ONNX support is unavailable.

### Independent camera tracking samples (Desktop 0.10.1)

The ONNX face and upper-body workers complete independently. Each result carries
its camera capture timestamp, and the render pipeline filters face and body on
their own sample clocks. Reusing a cached face must neither discard a new arm
measurement nor repeatedly advance the face filter. Losing the face only eases
the face toward neutral; visible torso and arms remain driven by their own fresh
measurements. Disabling body tracking clears its sample/filter state immediately.
Torso confidence is preserved even when elbows or wrists are outside the frame.
These changes run entirely on Windows and require no Android sender update.

### 4. Seamless Mode Transitions
- **Instant Camera Restoration**: Disabling Avatar mode immediately restores the original video feed, background video/image compositing, and chroma key / AI matting without requiring a stream restart.
- **Virtual Camera Output**: Works transparently with the UnityCapture virtual camera driver (`UnityCameraOutput`), streaming avatar visuals to Discord, OBS, Zoom, or Google Meet.

## UI Controls

In Cortex Studio's **Avatar** tab:
1. **Use test avatar** (`AvatarEnabled`): Toggles avatar mode on/off.
2. **Character Model Selection**:
   - **GPU Test Avatar**: Built-in procedural rendering.
   - **Live2D Cubism**: External model loading with file browser (`.moc3`, `.model3.json`).
3. **Motion Source**:
   - **Animate test pose** (`AvatarMotion`): When checked, runs procedural sine loop. When unchecked, camera face tracking drives movement.
4. **Monitor HUD**:
   - Cyan badge `TEST AVATAR` for procedural GPU mode.
   - Purple badge `LIVE2D AVATAR` for Cubism model mode.
   - Green badge `LIVE INPUT` when avatar mode is turned off.


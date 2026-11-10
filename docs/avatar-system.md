# Cortex Avatar System

Cortex Studio can replace the camera feed with a Live2D Cubism model driven by camera face tracking or demo animation.

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
│       • Face Tracking ──► FaceTracker::track()         │
│                                                        │
│     Rendering Path:                                    │
│       • Live2D Cubism D3D11 Model                     │
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

### 1. Live2D Cubism Integration (`CubismAvatar`)
- **Native SDK Hook**: Integrates with the Live2D Cubism Native SDK when compiled with `CORTEX_HAS_CUBISM` via CMake (`-DCUBISM_SDK_ROOT=...`).
- **Parameter Mapping**: Automatically translates `AvatarPose` properties into standard Cubism parameter IDs:
  - `yaw` → `ParamAngleX`
  - `pitch` → `ParamAngleY`
  - `roll` → `ParamAngleZ`
  - `eyeOpen` → `ParamEyeLOpen` & `ParamEyeROpen`
  - `gazeX` / `gazeY` → `ParamEyeBallX` & `ParamEyeBallY`
  - `mouth` → `ParamMouthOpenY`
- **Loading**: The camera preview displays an indeterminate progress bar while the model is prepared. The scene keeps its background and any current model during the load.

### 2. Motion Sources
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

### 3. Mode Transitions
- **Instant Camera Restoration**: Disabling Avatar mode immediately restores the original video feed, background video/image compositing, and chroma key / AI matting without requiring a stream restart.
- **Virtual Camera Output**: Works transparently with the UnityCapture virtual camera driver (`UnityCameraOutput`), streaming avatar visuals to Discord, OBS, Zoom, or Google Meet.

## UI Controls

In Cortex Studio's **Avatar** tab:
1. **Enable avatar** activates Live2D and loads the bundled Ren model if no model is selected.
2. **Choose Live2D model** opens a `.moc3` or `.model3.json` file.
3. **Motion Source**:
   - **Animate test pose** (`AvatarMotion`): When checked, runs procedural sine loop. When unchecked, camera face tracking drives movement.
4. **Preparing Live2D character** appears above the preview during loading.


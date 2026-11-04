# OpenSeeFace face tracking

Cortex uses the official OpenSeeFace MobileNetV3 detector and 66-landmark model 1.
The tensor decoding in `src/filters/FaceTracker.cpp` follows the BSD-2-Clause reference tracker.
Copyright (c) 2019 Emiliana. See LICENSE.

Source: https://github.com/emilianavt/OpenSeeFace
Pinned commit: 50c09741897010faad721a218b2bbe14ecad6251

Run `scripts/GetFaceTrackingModels.ps1` to download and verify the two ONNX models.
`GetProcessingDependencies.ps1` also runs it. Assets remain under `.deps/face-tracking`
and are included in the desktop package under `models/face-tracking`.

Inference uses ONNX Runtime CPU (two worker threads) on a dedicated processing thread.
Frames are rotated and reduced to at most 512 pixels on the long edge on D3D11 before
bounded readback. Only the latest pending frame is retained. No camera imagery is
written to disk or transmitted to a service. Rendering and virtual camera output stay
on the existing D3D11 pipeline. The current input is Cortex Sender, not a USB webcam.

Camera tracking maps relative head rotation, independent eyelid openness and inner
mouth aperture. Head angles now use a perspective 3D nose/jaw fit. Optional MediaPipe arm tracking is documented in ../MediaPipePose; pupil gaze and finger tracking are not implemented. Use Set neutral
face while looking forward to center head angles; mouth and blink are unaffected.
When a face or camera is lost, the avatar eases toward neutral rather than inventing
landmarks. Missing model files are reported in the UI without stopping rendering.

Tests use the NASA public-domain Eileen Collins portrait distributed by scikit-image;
fixture provenance is in tests/fixtures/astronaut.source.txt.

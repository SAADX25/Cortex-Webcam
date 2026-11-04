# OpenSeeFace face tracking

Cortex uses the official OpenSeeFace MobileNetV3 detector, quality 66-landmark
model 3 (model 1 fallback), and the dedicated 32-pixel pupil localization model.
The tensor decoding in `src/filters/FaceTracker.cpp` follows the BSD-2-Clause reference tracker.
Copyright (c) 2019 Emiliana. See LICENSE.

Source: https://github.com/emilianavt/OpenSeeFace
Pinned commit: 50c09741897010faad721a218b2bbe14ecad6251

Run `scripts/GetFaceTrackingModels.ps1` to download and verify the ONNX models.
`GetProcessingDependencies.ps1` also runs it. Assets remain under `.deps/face-tracking`
and are included in the desktop package under `models/face-tracking`.

Face inference uses ONNX Runtime CPU (two intra-op threads) on a dedicated worker.
Optional body inference has its own worker so it does not delay publishing a face.
Frames are rotated and reduced to at most 768 pixels on the long edge on D3D11 before
bounded readback. Each worker retains only its latest pending frame. No camera imagery is
written to disk or transmitted to a service. Rendering and virtual camera output stay
on the existing D3D11 pipeline. The current input is Cortex Sender, not a USB webcam.

Camera tracking maps face position, relative head rotation, independent eyelid
openness, brows and inner mouth aperture. Head angles use a confidence-weighted
robust perspective fit of the nose, jaw and eye corners. Physical angles are
preserved until neutral calibration. Optional MediaPipe arm tracking is documented
in ../MediaPipePose; finger tracking is not implemented. Pupil localization uses
the upstream affine eye crops and heatmap/offset decode rather than eyelid shape.
Missing/failed eye inference leaves face tracking usable. Use Set
neutral face & position while looking forward to center head angles and movement;
mouth opening and blink are unaffected. The first face also establishes a neutral
baseline automatically. Source and result freshness are limited to 250 ms. Input
rotation/size changes invalidate results from the previous camera geometry.
The perspective fit also exports relative translation/depth. Neutral depth ratios
drive optional camera-distance following; these are not calibrated metric units.
PoseSmoother independently implements the 1 Euro Filter equations for adaptive
movement response: https://gery.casiez.net/1euro/ . No camera imagery is retained.
When a face or camera is lost, the avatar eases toward neutral rather than inventing
landmarks. Missing model files are reported in the UI without stopping rendering.

Tests use the NASA public-domain Eileen Collins portrait distributed by scikit-image;
fixture provenance is in tests/fixtures/astronaut.source.txt.

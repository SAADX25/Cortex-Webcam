# Tracking references for 0.10.0

This update improves the current Ren model. It does not claim exact reproduction
of a person's motion or parity with another tracking application.

* OpenSeeFace, BSD-2-Clause, pinned commit
  `50c09741897010faad721a218b2bbe14ecad6251`:
  https://github.com/emilianavt/OpenSeeFace . The quality model 3 and dedicated
  pupil network extend the existing 66-point tracker. Model download hashes and
  provenance are in `scripts/GetFaceTrackingModels.ps1` and the packaged SOURCE.json.
* Casiez, Roussel and Vogel, 1 Euro Filter (CHI 2012):
  https://gery.casiez.net/1euro/ and https://github.com/casiez/OneEuroFilter .
  PoseSmoother is an independent implementation of the published filter equations
  with exponential time discretization, sample timestamps and reset handling.
* Kalidokit, MIT: https://github.com/yeemachine/kalidokit . Its Live2D and arm
  solver examples informed the review of separate head axes and separate arm
  joints. No JavaScript runtime/dependency was added. Ren's actual parameter
  ranges and shipped motion files establish shoulder and elbow directions.
* MediaPipe Face Landmarker:
  https://github.com/google-ai-edge/mediapipe and
  https://developers.google.com/edge/mediapipe/solutions/vision/face_landmarker/android .
  Reviewed as an Android alternative. It was not integrated into this update;
  Android continues to send hardware-encoded video for Windows-side inference.

Validation includes independent projected 6-DOF poses, stale input/sample
handling, adaptive jitter/lag trajectories, real ONNX fixture inference, and
rendered Ren pixel checks for head axes, translation, depth and independent elbow
motion. These checks do not establish a percentage accuracy on physical S24 Ultra
motion. The authored 2D rig still limits head turns and joint deformation.

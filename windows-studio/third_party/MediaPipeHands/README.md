# MediaPipe Hands model integration

Models and reference preprocessing: OpenCV Zoo, pinned commit
`48b10fa505aeb51fbd127fc60a5c660040e823b7`, Apache-2.0:
https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/handpose_estimation_mediapipe
and sibling `palm_detection_mediapipe`.

Run scripts/GetHandTrackingModels.ps1. It verifies the SHA-256 identifiers from
the pinned Git LFS pointers and ships LICENSE and SOURCE.json beside the models.
HandTracker.cpp implements RGB NHWC [0,1] preprocessing, 2016 palm anchors,
IoU suppression, upright palm crop and inverse coordinate rotation independently.
The direct affine crop avoids allocating a full intermediate rotated image;
border clipping/interpolation can differ from OpenCV's integer padded crops.

The model returns 21 screen and relative world landmarks. Its inferred depth is
not depth-sensor data. Camera image coordinates have Y down, while the motion
solver uses Unity Y up and Z toward the camera. Handedness is associated to Pose
wrists when visible, otherwise corrected for unmirrored camera input. Occlusion
or crossing hands may reduce confidence/identity stability. No accuracy percentage
or latency guarantee is implied by the model name.

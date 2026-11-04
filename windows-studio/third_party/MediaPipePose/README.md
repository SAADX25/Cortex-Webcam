# MediaPipe upper-body tracking

Official OpenCV Zoo models, pinned at 48b10fa505aeb51fbd127fc60a5c660040e823b7:
- models/person_detection_mediapipe/person_detection_mediapipe_2023mar.onnx
- models/pose_estimation_mediapipe/pose_estimation_mediapipe_2023mar.onnx

Source: https://github.com/opencv/opencv_zoo (Apache-2.0; LICENSE included).
`BodyTracker.cpp` implements RGB normalization, generated 2254 detector anchors,
person-centered rotated crop, landmark unprojection, visibility/presence gating and
relative shoulder/elbow/wrist geometry. No pixels are persisted or uploaded.

This controls arm lift and elbow bend, not finger landmarks. Haru's A/B arm variants
are approximations constrained by its 2D rig, not a full skeletal model. Arm tracking
is sampled at most every 80ms on the inference thread. A missing wrist does not drive
an invented pose; the corresponding arm eases back to rest. Turn off Track arm lift
and bend to keep face-only inference. Other models need compatible parameter IDs.

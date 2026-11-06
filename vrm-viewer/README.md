# Cortex VRM viewer

## Hand loss and hands switch

**Disable hands** at the top stops camera and preset arm/finger motion. Both arms
ease into their relaxed pose while head/face tracking continues. **Enable hands**
waits for fresh stable observations; it does not replay a cached gesture.

Each hand must be visible for several actual detector frames before its arm
reactivates. Wrists outside the image are rejected. With Hands installed, an
absent palm invalidates that entire 3D arm chain even if Pose still guesses an
elbow/wrist and reports `Arms 2/2 visible`. The receiver independently requires
stable complete arm/palm/finger observations. On loss it holds the last good
chain for 220 ms, then eases once to rest over 380 ms. Isolated detection flashes
cannot restart the arm. Reacquisition is smoothed and angular speed is bounded.

The main launcher now runs without joint diagnostics. Frequent `HAND_SAMPLE`
lines were coordinate/debug output, not a video recording or extra gestures.
`CortexMotion-x64/Start-Hand-Diagnostics.cmd` explicitly enables that output;
missing-hand frames no longer produce empty joint-sample lines. The normal
console still prints a brief tracking status approximately once per second.

## Ren inspired 3D appearance

### Mouth and smile (teeth removed)

The bundled base's enamel triangles are now removed at runtime at the user's
request, including when switching back to its original appearance. The source
VRM file is unchanged. Its authored custom
JawOpen, MouthSmileLeft/Right and lip-raise channels now receive aperture and
smile separately. Whole-face Happy/Relaxed and additive A/O vowels no longer
compete with these channels, and blinking does not suppress smiling. Avatars
without those custom channels use a bounded standard-expression fallback.

The Ren style retains a shaded oral interior and gentler lip raising. The added
enamel material, tooth alignment offsets and visibility animation have been
removed. Jaw opening and smiling still use separate channels.

`CortexPreview.Render` also writes Smile, Grin, MouthOpen, MouthClosed and
SmileSide renders. Tests include tooth removal in both appearances,
smile/wink independence, exact neutral restoration and
the standard-expression fallback on Alicia. Live camera smile accuracy remains
dependent on the measured face signal and framing.

### Head tracking and centering

Keep your face straight and still for roughly two seconds once face tracking
appears. Startup centering waits 1.5 seconds from the first valid fitted face
head sample, not from application launch or coarse Body Pose output. Manual
**Center head** waits one second after clicking before collecting the stable
window. A face with expressions but no valid head fit no longer publishes an
invented zero-angle head. The receiver establishes neutral from multiple stable
head samples, ignoring duplicate
packets and restarting the window after a pose outlier. **Center head** repeats
this without requiring an arm T-pose. The old first-frame sender pitch offset
has been removed. Physical pitch direction is corrected in the native solver.

The receiver reconstructs the sender's world head rotation and compensates the
actual displayed chest/neck hierarchy; splitting an already chest-relative
rotation into local head/neck rotations used to add unwanted torso tilt.
Head motion has a small deadband, adaptive smoothing, a maximum angular speed,
and bounded pitch/yaw/roll. Missing head tracking eases back to neutral.
Rebuild/package both the motion backend and viewer together for this change.

The display camera is positioned at the imported avatar's eye level while
aiming at the torso for framing. The previous fixed camera was below the eyes,
showing the underside of the chin even at zero tracked pitch. Runtime and main
offline previews now share `AvatarCameraFraming`.

The bundled male base now has an optional original procedural outfit inspired by
the supplied Ren screenshot: a long pearl/charcoal coat, gold collar lining and
trim, geometric dark shirt, triangular pendant, layered charcoal hair and blue
iris tint. The coat, sleeves and hair are actual skinned meshes attached to the
imported humanoid skeleton. The face has a narrower jaw and reduced eye height;
the same continuous deformation is applied to every facial morph target so
blinks and mouth expressions retain their names, indices and frames. Long
trousers are weighted to both leg chains. Rounded shoulder caps, curved lapels,
sleeve folds/insets, fitted trim and textured skin shading replace the initial
flat outfit. The textured fringe is retained rather than covered with a row of
flat hair blades. This is a stylized approximation, not the original Ren 3D asset
and not a conversion of the `.moc3` file.

`dist/Start-3D-Avatar.cmd` selects the bundled base with `--appearance ren`.
The **Ren inspired 3D outfit** checkbox restores the imported mesh/materials when
disabled. An unrelated `--vrm PATH` keeps its own appearance unless explicitly
given `--appearance ren`; `--appearance original` disables automatic styling.
The original VRM file on disk is not modified or exported with this outfit.

Cover eyes now accounts for the facial surface in the imported mesh rather than
using buried eye rotation pivots. Palms are separated and follow head rotation;
crossed arms use a gentle finger curl. Face tracking suppresses small brow/gaze
noise and softens mouth closing; optional idle blinks run only without measured
face data and when idle motion is enabled.

For offline visual verification, run Unity in batch mode with
`-executeMethod CortexPreview.Render` (without `-quit`). It exits after writing
front, three-quarter, full-body, face/expression and pose previews to
`windows-studio/build/avatar-preview`. Unity EditMode
tests exercise the actual imported model, face clearance, coat bone weights,
expression morph preservation, actual knee deformation of the trousers and
reversible appearance switching. The shared base face/hair still differ from
Ren; matching the reference exactly requires a purpose-built character mesh.

## Crossed arms and covering the eyes

The viewer toolbar now offers **Camera**, **Cross arms**, and **Cover eyes**.
The two manual poses use the imported avatar's actual arm lengths and a smooth
two-bone solve. Cross arms places the forearms at slightly different depths;
Cover eyes raises open palms in front of the eye bones (or a scaled head estimate
when eye bones are absent). Select Camera to release the preset and resume live
arm/finger tracking. These are pose controls, not a new camera gesture classifier.

Live tracking now assigns both hand detections jointly using handedness, Pose
wrist locations and short motion history, avoiding duplicate identities during
crossings. A confidently tracked pair of palms covering the last known eye
positions can bridge a face dropout for up to 750 ms; it does not measure an
invisible head or hold it indefinitely. Camera input changes clear the history.

Windows camera/pose/hand inference → analytic two-bone IK + local quaternions →
792-byte UDP datagrams on 127.0.0.1:39539 → UniVRM normalized ControlRig.

This is a separate Unity renderer with a Ren-inspired outfit. Ren's `.moc3` is
Live2D and has no VRM skeleton. Alicia is the official UniVRM test model requested
by the user; its source/hash/usage terms are in StreamingAssets/Alicia.SOURCE.json.
Model rights are separate from UniVRM's MIT license.

## Build and run

1. Run `windows-studio/scripts/GetHandTrackingModels.ps1` and build Windows Studio
   Release. Models go in `models/hands` alongside `models/upper-body` and face models.
2. Install and activate Unity Editor. `Build-Viewer.ps1 -UnityEditor PATH` runs the
   editor's batch build and writes `dist/CortexVRM-Viewer/CortexVRM.exe`. The project
   pins UniVRM 0.131.4 via UPM; Unity imports its packages on the first build.
3. Close Cortex Studio before running `cortex-motion.exe --diagnostics`. Both own
   the same phone video/control ports. The CLI prints measured L/R joint XYZ at
   most 10 Hz and publishes motion every completed body/hand inference sample.
4. Run `CortexVRM.exe --vrm PATH_TO_MODEL.vrm`, or omit the argument for the
   bundled male base (Alicia is used if that file is absent).
   Look at the viewer's measured bone count, sample age and sequence-gap counter.
   Use **Calibrate in T-pose with open hands** for your framing/model setup.

The desktop can also publish while camera avatar tracking is enabled by setting
`CORTEX_VRM_PORT=39539` before launching it. Publishing is off by default.
`CORTEX_HAND_DIAGNOSTICS=1` enables raw joint output (stdout and debugger output).
Diagnostics include body/hand coordinates only; camera images are not recorded.

## Contract and practical limits

Each little-endian datagram contains ASCII `CXVRM001`, uint32 version=1, uint32
boneCount=38, uint64 sequence and uint64 capture timestamp (monotonic microseconds),
then 38 records of float32 quaternion XYZW and confidence. Stable bone order is
defined in HumanoidMotion.h and CortexVrmViewer.BuildMap. Quaternions are local
rotations for a normalized T-pose; the importer ControlRig maps to actual model
rest axes. The receiver validates the whole packet, ignores reordered/duplicate
sequences, and allows sequence reset after one second of silence. There is one
latest sample, no unbounded application queue. Lost head/body samples ease to
neutral; arms use the grouped hold/fade policy described above. Packet age shown is reception age, not
end-to-end camera latency. UDP has no guaranteed delivery.

The solver reconstructs arm bend using measured wrist target, elbow pole and
segment lengths, handles antiparallel/zero-length directions, and then computes
parent-relative arm/hand/finger rotations. Missing bones/joints remain at rest;
head orientation uses Pose ear/nose landmarks; legs are not exported by this first motion solver. Hand depth
is inferred from a single RGB camera, not measured by a depth sensor. Occlusion,
crossing hands, palm handedness, forearm twist and thumb rest axes need physical
acceptance/calibration. No 100% accuracy, loss-free transport, or live FPS claim
is established by unit tests. The original Live2D rendering path remains available.

References: pinned [OpenCV Zoo Hands](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/handpose_estimation_mediapipe),
[UniVRM runtime import](https://vrm.dev/en/api/runtime-import/UniVRM10_Vrm10/).

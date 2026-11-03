# Phone studio interface (0.10.0)

The Android interface uses native Views and vector icons. No web view, remote assets or new runtime dependencies are required.

## Responsibilities

- `ui/CameraScreen.kt`: responsive preview, status and primary camera controls. Reflow moves controls but retains the same SurfaceView.
- `ui/CameraOptionsSheet.kt`: scrollable orientation and frame-rate choices. Receives confirmed selection changes from the screen, including changes made in Windows.
- `ui/CameraUi.kt`: shared palette, typography, rounded surfaces, ripple and short press animations. Animations honor the system animator setting and never transform the preview.
- `res/values/strings_camera.xml` and `res/values-ar/strings_camera.xml`: English and Arabic camera-interface copy. Arabic follows the platform RTL layout direction.
- `ui/MainActivity.kt`: owns safe system-bar insets through the screen, forwards existing service actions, and preserves the existing camera/discovery lifecycle.

## Layout and accessibility

Portrait uses a bottom control dock; landscape uses a compact side dock. Both expose the same actions and settings. Touch targets are at least 48 dp and have descriptive accessibility labels. Selected options expose checkable/checked semantics. Connection details remain accessible through the status chip; connection setup uses the header button.

The options dialog scrolls on small screens, and closes on a change between portrait and landscape so stale window dimensions are not retained. System bars and display cutouts have a single inset owner, including on Android 15 edge-to-edge windows.

## Verification

Run `testQualityUnitTest assembleQuality lintQuality`, then `assembleRelease` for the distributable build. The test suite covers protocol, orientation and encoding behavior as well as repeated UI rotation, action reachability in Arabic/English, system insets with larger text, and remote FPS selection reflected in an open options sheet.

Native Android rendering tests write static, camera-free review images to `app/build/ui-review`. These images validate UI appearance; they do not verify physical-device camera output. A device was not connected through ADB during this release's verification.

Install the signed 0.10.0 APK over the existing app. The Windows 0.8.1 synchronization protocol is unchanged. Camera frame-rate availability still depends on the physical camera.

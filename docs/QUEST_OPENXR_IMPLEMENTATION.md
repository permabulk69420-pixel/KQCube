# Quest OpenXR prototype implementation

## Prototype boundary

This is the first KQCube OpenXR vertical slice. Dolphin's existing Android UI remains on the
ordinary Android surface. Selecting a GameCube game starts the normal emulation flow, then a
Quest-only activity requests OpenXR once Dolphin's OpenGL ES renderer has produced its first XFB.

The prototype presents a fixed virtual screen in `LOCAL` space. Headset orientation therefore
changes the user's view of the screen, but it does not yet alter the emulated game camera. Audio is
untouched and continues through Dolphin's existing Android audio backend.

Normal phones and Android TV devices still launch `EmulationActivity` and never instantiate the
OpenXR presenter. A device must advertise `android.hardware.vr.headtracking` to select
`QuestEmulationActivity`.

## Files changed

Android packaging and lifecycle:

- `Source/Android/app/build.gradle.kts`
  - enables Prefab and adds Khronos' Android OpenXR loader `1.1.61`;
- `Source/Android/app/src/main/AndroidManifest.xml`
  - makes VR head tracking optional and declares the isolated Quest activity plus Meta metadata;
- `Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/activities/EmulationActivity.kt`
  - remains the stock implementation, but is open for the thin Quest subclass and selects that
    subclass only on VR-headtracking devices;
- `Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/activities/QuestEmulationActivity.kt`
  - requests and releases the native OpenXR path around the stock activity lifecycle;
- `Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/NativeLibrary.kt`
  - exposes the three small lifecycle/status JNI calls;
- `Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/fragments/EmulationFragment.kt`
  - stops Android surface callbacks from detaching the EGL context after OpenXR owns it;
- `Source/Android/jni/MainAndroid.cpp`
  - forces OGL/GLES plus layered stereo only for a Quest boot, bridges lifecycle JNI, handles
    fallback and runtime exit, and releases the retained Android window after video shutdown.

Renderer integration:

- `Source/Core/VideoCommon/AbstractGfx.h`
  - adds an optional no-op `PresentToOpenXR` backend hook;
- `Source/Core/VideoCommon/Present.cpp`
  - offers the unflattened XFB texture to that hook before the Android backbuffer is presented;
- `Source/Core/VideoBackends/OGL/OGLGfx.h` and `OGLGfx.cpp`
  - own the isolated presenter and suppress stale Android swaps while its EGL context is parked;
- `Source/Core/VideoBackends/OGL/KQCubeOpenXR.h` and `KQCubeOpenXR.cpp`
  - contain loader, EGL handoff, OpenXR lifecycle, swapchains, frame submission, stereo-layer copy,
    diagnostics, Touch actions and teardown;
- `Source/Core/VideoBackends/OGL/CMakeLists.txt`
  - links the Prefab OpenXR loader and Android GLES/input dependencies only on Android.

## Loader and extensions

The app consumes `org.khronos.openxr:openxr_loader_for_android:1.1.61` through Gradle Prefab and
links `OpenXR::openxr_loader` into Dolphin's OGL backend.

The presenter calls `xrInitializeLoaderKHR` with the active Java VM and Quest activity, then creates
an OpenXR 1.0-compatible instance with:

- `XR_KHR_android_create_instance`;
- `XR_KHR_opengl_es_enable`.

It queries `xrGetOpenGLESGraphicsRequirementsKHR` before creating the session and rejects a Dolphin
GLES version outside the runtime's supported range.

## GL context ownership and thread model

All OpenXR session, frame and GL work happens on Dolphin's existing GPU/render thread. The Java UI
thread only records or clears a global activity reference and reads an atomic presentation status.

On the first XR-bound frame, the presenter:

1. verifies that Dolphin's exact EGL display, config, context and Android window surface are current;
2. finishes and swaps one ordinary Android frame so Quest does not remain in the loading transition;
3. moves that same EGL context to a 16x16 pbuffer;
4. falls back to `EGL_KHR_surfaceless_context` if the selected EGL config cannot make a pbuffer;
5. marks OpenXR presentation active so later `SurfaceView` destruction cannot pause Dolphin or
   detach the graphics context;
6. creates the OpenXR instance and session using that exact current context.

If any initialization step fails, the presenter destroys partial OpenXR state, restores the Android
window binding, reenables normal backbuffer swaps and asks the Android host thread to turn stereo
back off. This keeps the stock monoscopic Android path usable instead of leaving a side-by-side
fallback frame.

The OGL member order keeps the parking surface alive until ordinary OGL framebuffer resources have
been destroyed. OpenXR GL objects, swapchains, spaces, session and instance are torn down on the
render thread before the pbuffer is released and the main GL context is destroyed.

## Stereo interception and swapchain copy

Quest boots use current-run configuration overrides only:

- backend `OGL`;
- prefer GLES;
- stereo mode `SideBySide`, which activates Dolphin's existing two-layer stereo renderer;
- full per-eye stereo resolution.

`Presenter::Present` calls `PresentToOpenXR` with `m_xfb_entry->texture` before Dolphin flattens the
texture array for its normal side-by-side backbuffer. The OGL implementation passes the underlying
GL texture to `KQCubeOpenXR`.

The runtime gets one single-sample GLES swapchain per eye at the recommended Quest dimensions. Each
frame acquires and waits for both images, attaches the XFB texture layer to a read FBO, attaches the
OpenXR image to a draw FBO and copies with `glBlitFramebuffer`:

| OpenXR eye | Dolphin XFB layer |
| --- | --- |
| Left | 0 |
| Right | 1 |

If an XFB source contains only one layer, layer 0 is copied to both eyes and a one-time mono fallback
warning is written to logcat. The code does not split a flattened side-by-side screenshot.

The two eye images are submitted as eye-specific `XrCompositionLayerQuad` layers on the same
2.4-metre-wide screen, two metres forward in `LOCAL` space. `xrLocateViews` validates and logs live
head tracking; the compositor supplies the orientation-relative view of the fixed screen.

## Touch controller path

The module creates one OpenXR action set, suggests bindings for
`/interaction_profiles/oculus/touch_controller`, attaches it before `xrBeginSession`, and feeds
values directly into `ciface::Touch` for GameCube controller 1. A radial 0.15 deadzone is applied to
both sticks; trigger analogue values are preserved and their digital clicks use a 0.45 threshold.

| Quest Touch input | GameCube input |
| --- | --- |
| Left thumbstick | Main stick X/Y |
| Right thumbstick | C-stick X/Y |
| A / B / X / Y | A / B / X / Y |
| Left trigger | L analogue + digital |
| Right trigger | R analogue + digital |
| Right grip | Z |
| Left grip or left menu | Start (pause/back) |

When Touch actions are inactive, their overrides are cleared so an Android or paired controller can
continue supplying input. The GameCube override is unregistered during render-thread teardown.

## Session exit and diagnostics

Session state transitions are polled every presented frame. `STOPPING` ends the session;
`EXITING`, `LOSS_PENDING`, instance loss, or Java activity destruction signal Dolphin's Android host
loop to call `Core::Stop`. `Core::Shutdown` then owns orderly video/OpenXR destruction.

All prototype diagnostics use the tag `KQCube-OpenXR`. A useful filtered stream is:

```sh
adb logcat -s KQCube-OpenXR:D DolphinEmuNative:I '*:S'
```

Expected milestones include the Android bootstrap swap, EGL parking method, runtime version, GLES
requirements/session creation, two swapchain sizes, session state changes, valid tracked views,
stereo or mono source detection, first submitted quad pair, active Touch input and render-thread
shutdown.

## Build and artifact

Local debug build:

```sh
cd Source/Android
./gradlew :app:assembleDebug --no-daemon --stacktrace
```

The APK is written under `Source/Android/app/build/outputs/apk/debug/`.

`.github/workflows/kqcube-android-baseline.yml` performs the same build with Java 17, SDK 36, NDK
`29.0.14206865` and CMake 3.22.1. It verifies an ARM64 library exists, writes a SHA-256 checksum and
uploads both files as `KQCube-android-debug-<commit>`.

## Validation status

Validated locally:

- non-Android compilation of the isolated module's stub path;
- Android/OpenXR compilation syntax against Khronos OpenXR headers with GLES, EGL, JNI and Android
  interfaces present;
- warnings check with `-Wall -Wextra -Wpedantic -Werror` except OpenXR's conventional partial
  aggregate initialization warnings;
- whitespace and staged-diff checks for each implementation commit.

Not yet validated in this workspace:

- the complete Gradle/NDK APK build, because no Android SDK/NDK is installed locally and the branch
  has not yet been pushed to Actions;
- installation or runtime behaviour on physical Quest 3 hardware;
- game-specific stereo compatibility.

## Known limitations and next steps

- Only OpenGL ES is supported by this prototype; Vulkan remains untouched.
- The Android library UI and overlays are not composited into XR. OpenXR begins after a game frame
  reaches the presenter.
- The screen is fixed in `LOCAL` space. Full game-camera orientation/position, world scale, HUD
  handling and culling fixes are later milestones.
- Touch currently feeds GameCube controller 1 only. Wii Remote motion, hand tracking and haptics are
  intentionally out of scope.
- Frame timing is reached through Dolphin's existing presentation cadence. Physical testing should
  verify smooth behaviour at Quest refresh rates and guide any later decoupling.
- Some games or XFB paths may produce a single-layer texture even with stereoscopy enabled; those
  remain visible through the logged mono duplication fallback.
- The next gate is a successful GitHub Actions ARM64 APK, followed by Quest logcat capture through
  session entry, first stereo frame, Touch input and clean exit.

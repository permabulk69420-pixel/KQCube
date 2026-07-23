# KQCube OpenXR bootstrap

## Pinned baseline

KQCube starts from official Dolphin release **2606**, commit:

`6094cfcf7b8fba733b3116fdf3414d51c1c0e4a4`

The fork's `master` branch was already 199 upstream commits beyond 2606 when this branch was created. Keep the first OpenXR prototype pinned to the release commit so emulator and build-system changes do not move underneath the work.

## Verified Android build inputs

Dolphin 2606 uses:

- Java 17
- Gradle 9.4.1
- Android compile/target SDK 36
- Android NDK `29.0.14206865`
- CMake 3.22.1 or newer
- Android ABIs `arm64-v8a` and `x86_64`

The baseline workflow builds `Source/Android` with `:app:assembleDebug`, verifies that the APK contains ARM64 native libraries, writes a SHA-256 checksum and uploads both files.

## What modern Dolphin already provides

Do not implement stereoscopy from scratch.

When stereoscopy is enabled, Dolphin 2606:

1. creates EFB colour and depth textures as two-layer `Texture_2DArray` resources;
2. renders the generated geometry once for each eye and selects the eye with `gl_Layer`;
3. applies separate left/right stereo offsets and convergence;
4. exposes layer 0 and layer 1 separately to the presenter;
5. normally flattens those layers into side-by-side, top-and-bottom or another desktop presentation mode.

The useful OpenXR interception point is therefore **before Dolphin flattens the two layers into the Android backbuffer**. The first prototype should consume texture layer 0 as the left-eye source and texture layer 1 as the right-eye source.

Relevant modern files:

- `Source/Core/VideoCommon/FramebufferManager.cpp`
- `Source/Core/VideoCommon/GeometryShaderManager.cpp`
- `Source/Core/VideoCommon/GeometryShaderGen.cpp`
- `Source/Core/VideoCommon/Present.cpp`
- `Source/Core/VideoCommon/PostProcessing.cpp`
- `Source/Core/VideoBackends/OGL/`

## How to use the donor projects

### dolphinEV

Repository: `https://github.com/coccofresco/dolphinEV`

Use it as a reference for:

- Android OpenXR loader and manifest setup;
- OpenXR instance, system, session and reference-space lifecycle;
- GLES graphics binding;
- swapchain creation;
- `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` sequencing;
- Quest/Pico packaging and lifecycle lessons;
- the point where VR frame timing was moved into Dolphin's presenter.

Do **not** merge the fork wholesale. It is based on an older Dolphin tree and includes large unrelated upstream changes. Also do not copy its later geometry-shader line that simply divides `gl_Position.xy` by 4; that is a presentation hack, not the desired architecture.

### Dolphin VR / VR-Hydra

Repository: `https://github.com/CarlKenner/dolphin`, branch `VR-Hydra`

Use it later as a behavioural and mathematical reference for:

- head-pose integration with the game camera;
- world scale and units;
- positional tracking;
- HUD depth and detached HUD handling;
- culling and skybox fixes;
- game-specific VR profiles.

Do not transplant its old renderer wholesale into Dolphin 2606.

### KQ64

Repository: `https://github.com/permabulk69420-pixel/KQ64`

Use its Quest diagnostics, lifecycle failure handling and clean-exit lessons where they still apply. Do not copy the N64-specific projection and framebuffer architecture into Dolphin.

## First implementation mission

Work directly in this branch. Do not stop after writing another plan.

### Required result

Produce a debug-installable Android ARM64 APK that, on Meta Quest 3:

1. launches Dolphin's existing Android library/file-selection UI;
2. starts a GameCube game normally;
3. enters an OpenXR immersive session when emulation starts;
4. enables Dolphin's existing stereoscopic rendering path;
5. presents texture layer 0 to the left eye and layer 1 to the right eye on a comfortable virtual screen;
6. responds to headset orientation so the user can look around the VR presentation;
7. maps basic Quest Touch input to one GameCube controller;
8. preserves game audio;
9. exits OpenXR and emulation cleanly without leaving the Quest stuck in a loading environment;
10. builds through GitHub Actions and uploads the APK.

### Scope and order

1. Preserve the working stock Android build in its own commit.
2. Add a small, isolated Quest/OpenXR module rather than spreading OpenXR calls throughout Dolphin.
3. Start with the OpenGL ES backend only.
4. Add extensive logcat diagnostics around OpenXR loader, instance, session state, swapchain, frame loop, GL context and shutdown.
5. Keep normal Android presentation as a fallback when OpenXR initialization fails.
6. First get a mono virtual screen working if necessary to validate lifecycle and frame timing.
7. Immediately after that, use Dolphin's existing two-layer stereo texture; do not re-render fake depth and do not split a flattened screenshot if the layers are directly accessible.
8. Implement Quest controls as a dedicated input source feeding Dolphin's controller interface. Avoid manufacturing fragile Android `KeyEvent` objects if native controller values can be submitted directly.
9. Keep Wii motion controls, polished VR menus, environments, hand tracking and game-camera positional tracking out of the first pass.

### Initial Quest mapping

- left thumbstick -> GameCube main stick
- right thumbstick -> GameCube C-stick
- A/B/X/Y -> GameCube A/B/X/Y
- left trigger -> L
- right trigger -> R
- right grip -> Z
- left grip or left menu action -> pause/back

Preserve analogue stick and trigger values and add modest deadzones.

### Success definition

The prototype is successful when a user can install the generated APK, choose a GameCube game, enter VR, see genuine left/right Dolphin stereo on a virtual screen, control the game with Touch controllers and exit cleanly.

Full game-camera head tracking is a later milestone. Do not sacrifice the working stereo-screen prototype while attempting it.

## Required implementation notes

Update this document or add `docs/QUEST_OPENXR_IMPLEMENTATION.md` with:

- exact files changed;
- OpenXR loader and extensions used;
- GL context ownership and thread model;
- where the Dolphin stereo texture array is intercepted;
- how texture layers are copied or rendered into OpenXR swapchains;
- controller input path;
- build commands;
- Quest logcat tags;
- tested and untested hardware behaviour;
- known blockers and next steps.

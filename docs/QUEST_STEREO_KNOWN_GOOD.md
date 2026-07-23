# Quest stereo known-good checkpoint

## Status

Physical hardware validation completed on **23 July 2026** using a Meta Quest 3S.

The OpenXR stereo path is now confirmed working correctly:

- both eyes render correctly;
- genuine left/right stereoscopic output is visible;
- screen scale and presentation look correct;
- the previous broken/black right-eye issue is resolved;
- the build is stable enough to preserve as the working stereo baseline.

## Preserved checkpoint

- Source branch at validation: `agent/kqcube-bootstrap`
- Known-good source commit: `b55357016693cf2c0f5a1f595d5da6a9508acb2f`
- Permanent checkpoint branch: `checkpoint/quest-stereo-working`
- Successful GitHub Actions run: `29989135571`
- Validated artifact: `KQCube-right-eye-fix-b553570.apk`

The right-eye fix assigns `gl_Layer` before every emitted geometry-shader vertex so Adreno GLES does not lose the target layer after `EmitVertex()`.

## Next active milestone

Add and validate user-facing controller mapping controls.

The immediate goal is to make GameCube controller bindings usable and configurable for Quest Touch input without disturbing the known-good stereo renderer. Preserve the checkpoint branch while developing input work on `agent/kqcube-bootstrap` or a dedicated controls branch.

Initial mapping target:

- left thumbstick -> GameCube main stick;
- right thumbstick -> GameCube C-stick;
- A/B/X/Y -> GameCube A/B/X/Y;
- left trigger -> L;
- right trigger -> R;
- right grip -> Z;
- left grip or menu action -> Start/pause.

Treat stereo rendering as locked working behaviour. Any controller work should be tested for regressions in both eyes, OpenXR session entry, game launch, and clean exit.

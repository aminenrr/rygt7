# AGENTS.md — aminenrr's RyGT7 fork

Context for any agent (human or AI) working on this fork. Written 2026-11-12.

## What this is
- Fork of RyGT7 (Gran Turismo 7 PS4 -> Windows): upstream gitlab.com/rygt-team/rygt7, mirrored at github.com/RyGT-Team/rygt7.
- Renderer: github.com/RyGT-Team/shadgt7 (fork: github.com/aminenrr/shadgt7).
- Reference implementation for most features: RyGTS (GT Sport sibling), gitlab.com/rygt-team/RyGTS.
- Owner moved from GT Sport (RyGTS) to GT7 because RyGT7 runs better and more stably.
- Owner's PC: i7-12700H, RTX 3070 Ti laptop GPU (8 GB). No DLSS frame gen (needs RTX 40+); FSR frame gen only.

## Goals (ranked cheapest -> most expensive)
1. Precompile quick win: make `compile_pack` actually work (needs shader objects).
2. Trophies.
3. UI at output resolution (port RyGTS UI-HIRES).
4. Shareable precompile packs.
5. DualSense HD haptics.
6. Real temporal DLSS / FSR.

Build order chosen: UI-hires + precompile quick win first (branch `ui-hires-precompile` in both forks).

## Workflow
- Branch per feature in both forks. Never push to upstream; no public comments/PRs upstream without the owner's say-so.
- No local Windows build: use the fork's GitHub Actions CI (`.github/workflows/rygt7-ci.yml`, `ci/rygt7/*`). A full run takes ~75 min.
- On feature branches, CI must build the renderer from `aminenrr/shadgt7` (same branch name), not upstream (`ci/rygt7/registry_ci.py`, `build-windows.ps1`).
- Every new option is off by default and a no-op when disabled.

## Findings (source-checked 2026-11-12)

### UI resolution
- RyGT7 has no UI-hires option. Only route to a 4K UI today is the PS4 Pro checkerboard mode (3200x1800 scene), which is expensive.
- RyGTS has `GPU.ui_hires` ("Draw the UI at screen resolution"): renderer patch 0062 (`vk_ui_hires.h` ~724 lines + `vk_native_ui.h`) and game patch GTS-009 (forces 4K UI image flag at 0xe548e2 and 0xf387b2 — GT Sport addresses, do not reuse).
- How it works: scene stays 1920x1080; the scene composite is scaled (DLSS or blit) into an output-size proxy; later UI draws go into the proxy with scaled viewport. Much lighter than Pro checkerboard (2D work, some extra VRAM).
- GT7 equivalent of the 4K UI asset flag is still to be found (future step).

### Upscalers
- Game always renders 1920x1080. Quality presets (1x, 1.5x, 1.72x, 2x, 3x) only pick output size; they never lower render res.
- FSR 1 = shadGT's own spatial pass + RCAS.
- FSR 3.1 and DLSS get the finished frame with zero motion vectors, flat depth, history reset each frame -> spatial only, not temporal.
- FSR frame gen uses optical flow only -> HUD/edge ghosting.
- RyGTS has real temporal FSR (patch 0092, NATIVE-FSR.md) and depth/motion frame gen (0093, FRAMEGEN-SCENE-INPUTS.md): port targets for goal 6.

### Precompile
- Shader store + driver cache persist, so only new shaders compile after the first run.
- `shader_pretranslate` (PERF-GT7-111/117, launcher toggle, commit 42c458a, 2026-11-08): re-translates recorded manifests in the background, only after a renderer update wipes the store.
- `compile_pack` (PERF-GT7-108, toggle in d5a73e7): driver-compile recipes after a driver update; only records with `RYGT_SHADER_OBJECT=1`, off by default since FIX-GT7-147, and the launcher doesn't set it -> toggle does little. Quick win: set it when the toggle is on.
- Remaining gap: first visit to each track/car/weather still stutters. Proposed fix: shareable code-free manifests + `RYGT_MANIFEST_CODE=scan` (pull code from the running game's memory).
- Deleting the shader cache would rebuild via manifests in principle; track-specific shaders probably compile on that track's load (unverified).

### Haptics
- PS4 build sends `scePadSetVibration` (2 motors); SDL maps it to basic DualSense rumble.
- Adaptive triggers exist: telemetry hook on GT7's Simulator Interface packet at service+0x1dc (`InstallTriggerTelemetryGt7`, from xacidr4inx) feeds `Input::TriggerFeedback`.
- Ryty has `AudioOut2PadMix` (PS5 pad vibration channels) that can carry a haptic synth.
- Plan: telemetry-driven synth (RPM tone, gear shifts, kerbs/bumps, tyre slip, G impacts), USB only, strength slider. Bluetooth HD haptics needs a separate report path.
- RyGTS reference: ADAPTIVE_TRIGGERS.md (strength slider, trigger-only DualSense reports, Xbox rumble fallback).

### Trophies
- shadgt7 compiles shadPS4 `np_trophy.cpp`, registers contexts from `npbind.dat` + `trophyNN.trp`.
- Nothing extracts the TRP to `TrophyDir/<commid>/Xml`; needs the owner's own trophy key (KeyManager).
- Pop-up `trophy_ui.cpp` is excluded; unlocks only log "Trophy unlocked". Unlocks likely don't persist (inferred).

### PS4 Pro mode (upstream PR #33 + shadgt7 PR #37, merged 2026-11-12)
- Kernel `RYGT_NEO=1`; launcher Graphics page with 5 modes. Renderer has NEO-GT7-001..015 fixes; kernel only reports Pro when the renderer exports support (guards the black-screen/GPU-loss crash).
- Tested only on an RTX 4070: ~5 GB VRAM, checkerboard races can drop below 30 fps. Untested on AMD/Intel and on the owner's 3070 Ti. Suggest 1080p TV mode (no checkerboard) first.
- Owner reports Pro mode was broken on GT Sport (unverified).

## Upstream state at fork time
- rygt7 main 1b48f3e (2026-11-12), shadgt7 main 5709ff8.
- Open upstream PR #34: Advanced page for the renderer's switches (launcher 0.1.2).
- Latest release: RyGT7-2026-11-07 (no shader-cache toggles; those landed 2026-11-08).

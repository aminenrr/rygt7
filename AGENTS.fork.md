# AGENTS.fork.md — branch `pr-tests` (aminenrr's RyGT7 fork)

Context for any agent (human or AI) working on this branch. Written 2026-11-14.

## What this branch is
- **Test-only.** `pr-tests` exists only to test the main RyGT7 project with specific upstream pull
  requests stacked on top of it, and to get a Windows CI build of that combination.
- It is **never** merged anywhere, never opened as a PR, never pushed upstream. Nothing here is
  meant to ship; it can be thrown away and rebuilt at any time.
- Do not touch the fork's other branches (`main`, `round2*`, `ui-hires-precompile`) from work on
  this branch. Their own notes live in their own `AGENTS.fork.md`.
- See `AGENTS.md` (upstream) for the project itself. Owner's PC: i7-12700H, RTX 3070 Ti laptop (8 GB).

## How it is built
- Base: **upstream** `RyGT-Team/rygt7` `main` (not the fork's `main`, which lags behind).
- Renderer: `aminenrr/shadgt7` branch `pr-tests` = upstream `RyGT-Team/shadgt7` `main` + the
  renderer PRs below. Same rules there: test-only, other branches untouched.
- CI: `.github/workflows/rygt7-ci.yml`. Always start it by hand (workflow_dispatch, ref `pr-tests`):
  push-triggered runs fail at "Resolve exact RyGT7 main revision". If that step fails with an
  HTTP 403 rate limit, re-run the failed jobs. Doc-only pushes use `[skip ci]`.
  It builds this fork's source with the head of `aminenrr/shadgt7` `pr-tests`
  (`FORK_*` env; `build-support/renderer.json` pins the same). A full run takes ~75 min; the
  installer is the run's artifact. Same CI change as `ui-hires-precompile` (commit dfe0249).

## What is in the current build (2026-11-14)
| Repo | PR | What | Status upstream |
|---|---|---|---|
| shadgt7 | #51 | FIX-GT7-184: GPU device lost on a junk texture (race start, rygt7#54) | merged, already in shadgt7 main |
| shadgt7 | #48 | FEAT-GT7-179: DualSense telemetry HD haptics, dynamic adaptive triggers, live feel settings | open |
| rygt7 | #49 | FEAT-GTS-265: launcher HD rumble & triggers window with Test buttons (pairs with shadgt7#48) | open |
| rygt7 | #18 | Launcher HD UI toggle on the Graphics page (`GPU.hd_ui`, off by default) | open |
| shadgt7 | #28 | HD UI renderer side for rygt7#18 (the toggle does nothing without it) | open |
| shadgt7 | (ours) | `pr-tests HDUI+TDLSS` commit f2e26c3e: temporal DLSS under HD UI | pr-tests only |

What to test: the GPU-device-lost fix (race starts), HD rumble + adaptive triggers (USB DualSense),
and the HD UI toggle. New options are off by default; turn them on in the launcher.

## Conflicts resolved by hand
- rygt7#49 vs main: both took launcher version 0.1.4. `loader/Version.cs` is 0.1.6 here and #49's
  CHANGELOG entry sits in a 0.1.6 section (CI checks the two agree).
- shadgt7#28 vs main (`ryty_game_patch.cpp`, `vk_presenter.cpp`, `vk_rasterizer_native.h`): both
  sides kept. With `GPU.hd_ui` on, #28 runs DLSS on the scene into its output-size proxy before the
  UI draws, and the presenter shows that proxy as-is (its own DLSS/FSR pass is skipped). Main's DLSS-GT7-005 4K-UI patch hits the same
  game bytes as #28's HD UI assets patch; it checks the bytes first, so with HD UI on it logs
  "bytes differ, not applied" and stands aside.

## Temporal DLSS + HD UI (the goal of this branch)
- Main's temporal DLSS (DLSS-GT7-001, on by default with Streamline DLSS and no frame gen) runs in
  the presenter with depth, motion vectors and jitter, and splits the race HUD out at 2x.
- #28 alone broke it: its single-frame DLSS call marks DLSS usable, so the game's AA was replaced
  with jittered colour that never got the inputs (shimmer).
- Commit `pr-tests HDUI+TDLSS` (shadgt7): #28's DLSS call now gets the frame's temporal inputs in a
  race frame whose AA pass was replaced; no HUD split / 2x HUD under HD UI (the proxy draws the UI
  after the scene, at the output size, with the 4K UI images). Menus stay single-frame DLSS.
- Untested. If it misbehaves: RYGT_DLSS_TEMPORAL=0 (launcher Advanced page) gives #28's plain DLSS.

## Changing the PR set
1. Rebuild both `pr-tests` branches from upstream `main` (reset, don't stack on old merges).
2. `git fetch upstream pull/<N>/head` and merge each PR with `--no-ff`, message
   `pr-tests: merge RyGT-Team/<repo>#<N> (...)`. Note any conflict resolution in the commit.
3. Re-apply the CI commit (branch name `pr-tests`), update the table above, push shadgt7 first,
   then rygt7 (the push starts CI).

## 2026-11-16 update
- rygt7 pr-tests now also has upstream main up to #57 (FEAT-GTS-269, DLSS plugin package) and #58 (trophies page, launcher 0.1.7), merged via #58. Conflicts resolved by union with #49 (feel test + DLSS plugin both packaged; FeelForm.cs + Trophies.cs both built; CHANGELOG keeps 0.1.7, then both 0.1.6 notes).
- shadgt7 pr-tests: + upstream main (#50 replay crash, #53) + #54 (trophies) + d0d01781 (single DLSS frames beside temporal ones on Streamline viewport 2, RYGT_DLSS_SIDE_VIEWPORT=0 off; HDUI-SINGLE log; RYGT_HD_UI_3D_MENU_SAFE and RYGT_HD_UI_SCENE_DEPTH now off unless =1).

## Temporal DLSS + HD UI in 3D menus (solved 2026-11-16)
- Bug: with HD UI and temporal DLSS both on, the 3D shook / edges shimmered in menus (cafe book
  videos, garage Scapes, used car purchase, post-race rewards, world map licence icon). Races were
  fine, and so was HD UI off.
- Causes found, in order:
  1. DLSS alternated temporal / single every frame on one Streamline history (4,653 switches).
     Fixed in `d0d01781`: single calls go to Streamline viewport 2 (`RYGT_DLSS_SIDE_VIEWPORT`, on;
     =0 off). Licence icon fixed.
  2. The HD UI proxy scaled the un-resolved, jittered colour as a single DLSS frame. `57200074`
     (stopgap): outside races keep the game's resolved picture (`HDUI-MENU` log;
     `RYGT_HD_UI_MENU_TEMPORAL=1` restores temporal). `dcfd4f5b`: `RYGT_HD_UI_MENU_TDLSS` gives
     the proxy the frame's depth, motion and jitter in menus (`HDUI-SRC` log, 60-frame fallback).
  3. Late scene: in some menus something (e.g. a subtitle) draws into the display before GT7's
     AA pass, so the proxy scales early and the scene composite (ps 0xdf75f23b) lands in it with
     no DLSS. `6cba106d`: the proxy is copied back down, the composite drawn natively, and the
     proxy scales at the next display draw with temporal inputs (`HDUI-SRC ... late scene`).
     `RYGT_HD_UI_LATE_SCENE=1` alone = late scene + stopgap; on with MENU_TDLSS anyway.
     `RYGT_HD_UI_MENU_TDLSS=2` also gives the proxy up on display-size depth draws after the
     composite.
- Owner test 2026-11-16 (CI 43963104510): shimmer gone everywhere with both LATE_SCENE=1 and
  MENU_TDLSS=1; MENU_TDLSS sharper and steadier (cafe, garage).
- `f70fee41`: **`RYGT_HD_UI_MENU_TDLSS` is on by default** (=0 off, owner's decision). HD UI +
  DLSS in the launcher is all that's needed. CI 43980072900 (2026-11-16 23:34 GMT).
- Ruled out: texture mip bias (`RYGT_DLSS_TEMPORAL_MIP_BIAS=0`, no change).
- Dead ends (kept, off unless =1): `RYGT_HD_UI_3D_MENU_SAFE`, `RYGT_HD_UI_SCENE_DEPTH`.
- Untested leftovers: `7c8e9ced` temporal retry after a VRAM refusal, Option A `4960ed4a`. The
  race-HUD detector (`hd_race_frame`) flickers in menus; a hold was planned, not built.

## Other fixes on shadgt7 pr-tests
- `c5569738` FIX-GT7-187 (from RyGT-Team/shadGT `a3a071d6`, in shadGT #50): a partial fill of a
  3D picture copied the whole depth and read past the 1 MB pattern buffer: device lost on the
  title screen / in races while many pipelines build. Conflict with FIX-GT7-031 resolved by hand:
  `ApplyRewrite` keeps FIX-GT7-031's whole-layer count, and for a 3D picture filled to full height
  it becomes the whole slices covered. The same bug is in shadgt7 main.

## Shader precompile / pipeline prewarm (planned)
- No Polyphony keys or decrypting game files (RyGTS removed that, FIX-GTS-293/294). GTToolsSharp
  5.3.5 can't open the owner's GT7 1.71 anyway ("Failed to decrypt volume"); dead end.
- What exists now: the shader store (PERF-032, kept between runs), the driver cache
  (`<serial>.vkpipelinecache`), and the manifest pretranslate (`RYGT_MANIFEST_RECORD` /
  `RYGT_MANIFEST_PRETRANSLATE`, `PERF-GT7-117`), which rebuilds translations at boot. Owner test
  2026-11-16: 25,226 translations in 52 s; worst catch-up 52.6 s down to 8.8 s.
- What's left: VkPipelines (shaders + state: vertex/colour formats, blend...) are still built
  mid-race while draws are skipped (`DIAG-GT7-012`); new cars pop for a moment.
- The precompile was removed in shadgt7 `da66c3a8e3` (2026-10-09, Lance's request): PipelineKey
  recording, `WarmUp`, `LoadGraphicsPipeline` / `LoadComputePipeline` / `LoadPipelineStage`,
  `PreloadQueue`, PERF-024 background build. The serialization helpers were kept.
- Plan: re-port it, adapted to today's code (per-thread `S()` state, async build workers,
  permutation listings): record each new pipeline's key and sdata to the store; at the title,
  load them and build them on low priority in the background, paused in races; one log line
  (built / skipped / failed / time). New switch, off by default. Test: race the same cars twice.
  Watch for stale entries and VRAM. Never-loaded content still compiles the first time.
- The full shadGT #40/#42/#44 precompile port was handed to the other fork agent on 2026-11-14.
- **Built 2026-11-17 (owner OK 2026-11-16 23:56): shadgt7 `d0235932`, PRT-PREWARM.**
  `RYGT_PIPELINE_PREWARM=1` (off by default): each new graphics pipeline's key is recorded once in
  the shader store (`prewarm-<hash>.key`; stages = stored translations by permutation hash). Next
  start, after the pretranslate, the recorded pipelines build from the store on low-priority
  threads (hw/4, 1..4), paused while `g_game_in_play`. A draw takes a prewarmed pipeline
  (`PipelineForCurrentKey`, before the async-skip path); read-ahead skips those keys; prewarm
  modules are handed to programs in `LoadStoredModule`. Log: `PRT-PREWARM: N recorded pipelines
  in T s on K threads: B built, S skipped (game first, not buildable), F failed`, then
  `PRT-PREWARM: N prewarmed pipelines taken by draws` (1st and every 250).
  Not prewarmed: geometry stages, mip-count (DynamicIndex) image bindings, translations
  LoadStoredModule would redo; whole prewarm off without dynamic vertex input, with shader
  objects or shader patches (logged). Prewarm build skips `GetSharp` (`t_prewarm_build`).
  Test: run once with it on (records), restart, race the same cars: no pop. Watch VRAM/RAM.
- **Splash progress, PRT-SPLASH (`d0235932`):** `RYGT_SPLASH_SHADER_PROGRESS=1` (off by default),
  needs Show Splash. Centred bar near the bottom of the splash (RyGTS PERF-098 style), drawn in
  `ryty_entry.cpp` `DrawFpsCounter` (present thread) from `Vulkan::BootProgress` counters
  (build_queue.h): "Building shaders X / Y" (pretranslate), then "Building pipelines X / Y"
  (prewarm). Only if the work started while the splash showed; stays till done, hidden in
  races, then "Shaders ready" fades. The game is not held on the splash (RyGTS PERF-102 is).
- Planned after the prewarm (owner OK 2026-11-16): **shared manifest, load-screen translate.** A
  manifest record has no shader code (inputs, guest reads, SPIR-V hash); the code stays in the
  local capture, so a shared manifest alone can't translate content the player never loaded. Idea:
  GT7 puts a car's or track's code in guest memory during the loading screen. Find it by hash, then
  translate the shared manifest's records for it on low-priority background threads (like
  PRETRANSLATE's 4), not on the command thread. What isn't done by race start keeps going in the
  background. New switch, off by default. Unverified: when the code really lands in memory, and
  how long loading screens give us.

## Fix log (keep this up to date)
Every new commit on either `pr-tests` branch gets a line here: date, repo, commit, fix or switch,
default, CI run, test result.
| Date | Repo | Commit | What | Default | CI | Tested |
|---|---|---|---|---|---|---|
| 2026-11-16 | shadgt7 | d0d01781 | single DLSS on side viewport (`RYGT_DLSS_SIDE_VIEWPORT`) | on | 43778170083 | licence icon fixed |
| 2026-11-16 | shadgt7 | 57200074 | HD UI menus keep resolved picture (stopgap) | on (superseded) | 43860628121 | still shimmer |
| 2026-11-16 | shadgt7 | dcfd4f5b | `RYGT_HD_UI_MENU_TDLSS` temporal inputs for the proxy | off then | 43867936183 | no change alone |
| 2026-11-16 | shadgt7 | 6cba106d | late-scene restart (`RYGT_HD_UI_LATE_SCENE`), MENU_TDLSS=2, trace fixes | off then | 43963104510 | shimmer gone |
| 2026-11-16 | shadgt7 | c5569738 | FIX-GT7-187 3D partial fill device loss | always | 43980072900 | pending |
| 2026-11-16 | shadgt7 | f70fee41 | MENU_TDLSS on by default | on | 43980072900 | pending |
| 2026-11-17 | shadgt7 | d0235932 | PRT-PREWARM pipeline record + start prewarm (`RYGT_PIPELINE_PREWARM`), PRT-SPLASH splash progress (`RYGT_SPLASH_SHADER_PROGRESS`) | off | pending | pending |
| planned | shadgt7 | - | shared manifest, load-screen background translate | off | - | - |

- Untouched: trophies upstream PRs (RyGT-Team/shadgt7#54, rygt7#58, open).

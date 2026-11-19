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

- **Plan v2 (owner "go" 2026-11-17 18:42 GMT): everything on by default, launcher only.**
  Approved on-by-default exceptions now also: `RYGT_PIPELINE_PREWARM`, `RYGT_SPLASH_SHADER_PROGRESS`,
  `RYGT_MANIFEST_RECORD`, `RYGT_MANIFEST_PRETRANSLATE` (each `=0` off), launcher Show Splash and
  "Rebuild translated shaders" checked by default (unchecked rebuild sends `=0`). Compile pack
  (`RYGT_COMPILE_PACK`) stays off: it needs shader objects; the prewarm does that job.
- **PRT-KEEP (shadgt7 `50721c9b`):** SHD-010 clears the shader store when the renderer commit
  changes, so each new CI build started cold. Now prewarm records are files in
  `shaders\<serial>.prewarm\<gpu>\<keyhash>.pwk` (header magic, file v2, key size,
  `PrewarmKeyLayout` 1; bump the layout when GraphicsPipelineKey fields change). Old in-store
  records (d0235932) are read once and written out as files. Each manifest record gets a `.idx`
  (its store index); pretranslate puts it back at that index when free (<256), so recorded keys
  match again. Log: `PRT-KEEP: N at their old index` in the pretranslate line; `PRT-KEEP: a new
  pipeline has a recorded one's stages but another key (differing: [a..b) ...)` for the first 20
  near-misses (owner run 2: 305), then every 100. Next: read those byte ranges and fix the key.
- **FIX-PRT-DLSS (`c6066056`):** owner log 2026-11-17: at 19:28 Streamline "Exceeded VRAM budget",
  DLSS result 39; `proxy.dlss_failed` (vk_ui_hires.h) and `Pass::failed` (ryty_dlss.cpp) were
  permanent, so HD UI DLSS and DLSS stayed off for the rest of the run. Now `dlss_retry_in` /
  `retry_in` = 120 frames, then DLSS again. Temporal refusals pause 600 calls (3600 from the 8th),
  never for the run.
- **Splash bar fix (`50721c9b`):** work that starts or ends before the splash shows is noted; the
  bar shows from the splash's first frame, "Shaders ready  N pipelines" held 2 s then fades.
- Dropped by owner 2026-11-17: compile-width ceiling #45, shared manifest / splash precompile
  #52 + rygt7 #53, launcher/naming PRs, wheel PRs, perf A/B.

## Perf plan (owner OK 2026-11-17 22:44 GMT)
Owner machine: i7-12700H (6P+8E), 32 GB DDR5, RTX 3070 Ti mobile 8 GB, laptop. Owner rule: batch
changes so each CI build carries several; two builds for this plan. Baseline (owner log 19:44,
cold cache): median frame >16.7 ms in 280 of 337 ten-second windows; recorder busy p50 76%, p90
88%; 21 compile catch-ups (442 s); 3,180 FIX-GT7-002 sampler-limit errors (3937/4000).
- **Build A (`c54af2d0`, PRT-PERF-A):**
  - PRT-ECORE `RYGT_ECORE_DRIVER_COMPILE` (on): on hybrid CPUs `compile_ids` = efficiency cores and
    `ConfineDriverCompileThreads` runs there too (was uniform CPUs only), so nvgpucomp64's threads
    leave the P-cores. Widening unchanged: every core only while the game waits, never in a race.
    Log: `PRT-ECORE: driver compiles on the N efficiency logical processors`,
    `PERF-GT7-024: N driver compiler threads moved to the compile cores`.
  - PRT-SAMPLER `RYGT_SAMPLER_DEDUPE` (on): LOD quarter steps from limit/2 (was 3/4), resolution
    bias 1/16 steps, custom border colour 8-bit steps in the key; at the limit the first sampler
    with the same coarse state (filters, clamps, aniso, compare, border type) stands in, not the
    last made. Log: `PRT-SAMPLER: N samplers ... for M coarse states` once at half the limit.
  - PRT-PERF `RYGT_PERF_LINE` (on): `PRT-PERF 10.0 s: GPU busy X% ..., N submits; CPU process ...`
    every 10 s in builds without diagnostics (busy = submitted GPU work unfinished, 1 ms samples).
  - P2 (merge mid-frame submits) dropped: fences already batch at 2 ms
    (`RYGT_NATIVE_FENCE_BATCH_MS`), the rest are waits the game needs; risk of stalls.
- **Build A+ (`ff9c84d7`, owner handoff 2026-11-18, built on Build A, CI 44391891394 green):**
  - PRT-LATE `RYGT_HD_UI_LATE_TEMPORAL` (on, owner-approved default; `=0` old behaviour): after a
    late-scene restart, and when an idle proxy is first composited, the HD UI proxy scales right
    after its composite in the same frame with that frame's depth/motion/jitter (GT Auto shimmer:
    before, it scaled at the next display draw one frame later, single DLSS). Log in HDUI-SRC:
    `late scene scaled temporal/single`, `composites scaled temporal right after/single`; each
    single logs `late scene scaled single` with the reason.
  - HD race hold `RYGT_HD_RACE_HOLD` (30 frames, `0` off): hd_race stays on 30 frames after the
    raw sign drops in 3D menus. Counter `HD race held`.
  - PRT-BINK `RYGT_DIAG_BINK=1` (off): pre-rendered cutscene diagnostic, one line/s while Bink
    threads submit GPU work (submits, time in submit call, frames presented, top compute
    programs, no-pipeline dispatches, fence latency, pipeline skips).
  - Cutscene frame skips (13-30 changed frames/s, Bink) predate DLSS, HD UI and prewarm (owner,
    2026-11-18), so those are not suspects. Test: baseline + `RYGT_NATIVE_FENCE_BATCH_MS=0`, both with
    `RYGT_DIAG_BINK=1`, same cutscene; fix C planned from the PRT-BINK lines, on only with owner OK.
- **Build C (`f0c493f9`, 2026-11-18, owner go 23:03; on Build A+):** owner test 23:00: HD UI off =
  GT Auto sharp, so the HD UI proxy path made GT Auto's 3D aliased/low-res.
  - G3 `RYGT_HD_UI_LATE_GIVE_UP` (on, owner-approved): a late-composite screen (GT Auto) drops the
    HD UI proxy for the frame (at the late restart, then already at the early display draw while
    the screen keeps compositing late); the presenter's temporal DLSS takes the display, UI at
    native res there only.
  - G1 `RYGT_HD_UI_LATE_SKIP_EARLY=1` (off, only with G3=0): draws before the composite stay native,
    one temporal scale right after the composite.
  - G2 `RYGT_HD_UI_OWN_VIEWPORT=1` (off): HD UI temporal DLSS on Streamline viewport 3.
  - G4 logs: `PRT-GTAUTO` every 600 frames on a late screen; `PRT-DLSS-STATS` every 600 DLSS calls.
  - V1 `RYGT_DLSS_VRAM_RESERVE` (384 MB, 0 off): on a Streamline "Exceeded VRAM budget" or usage
    within the reserve of the budget, PERF-GT7-005 idle kept textures halve (max every 300 frames,
    down to 1/16); back a step after 3600 calm frames. Log `PRT-VRAM`.
  - V2 `RYGT_DLSS_FREE_SIDE` (on): slFreeResources on the side viewport after 600 idle calls.
  - V3 `RYGT_DLSS_RETRY_CALLS` (120): temporal retry after refusal (was 600, 3600 after 8).
  - V4 PRT-PERF line adds `VRAM used of budget MB`.
- **Build B2 (2026-11-19, owner go 13:51 GMT; on Build B):** from the b0 log. (1) PRT-AVSYNC, rygt7
  `libSceAvPlayer.native/Export.cpp`: championship cutscenes are AvPlayer MP4s (not Bink); MediaTime
  snapped the video clock to audio past 80 ms and DeliverDecodedFrame dropped frames over 50 ms late
  (3 to 6 skips per 2 s). Now a rate-based slew (at most 25% fast/slow, anchor moves by whole ms),
  hard snap past 500 ms, drop past 100 ms. `RYGT_AVP_SMOOTH_CLOCK=0` = old. (2) `RYGT_BINK_30HZ` off by
  default (world map's BinkT04 video locked the map to 30). (3) PRT-JITTER (log only, shadgt7 fb672cb):
  jitter min/max, zero and repeated jitter frames, motion vector scale range, resets, every 600 menu
  temporal frames, to tell why GT Auto is still aliased. Bats: b0-default, b1-avsync-off, b2-bink30-on.
- **Build B2 results (owner test 2026-11-19 15:10 to 15:16 GMT):** cutscene Championship_02.mp4 is
  60 fps, title takes 120 frames per 2 s, flips 60/60, submit-to-flip 18 to 50 ms, queued up to 3, yet
  the owner still sees skips. New report: audio crack/stutter only at the video start, on b0 AND
  b1, so not AVSYNC; most likely old. GT Auto: no aliasing, UI still not steady.
- **Build B3 plan (2026-11-19, handed to the other agent; not built yet; owner OK needed):**
  (1) AvPlayer frame hold `RYGT_AVP_HOLD=3`: a used frame buffer is not reused for 3 more takes
  (lead: at 60 fps the decoder overwrites a buffer the GPU still reads). (2) Forced upload: mark the
  decoder's guest range dirty on write so the texture cache re-uploads it the same frame. (3) Log
  PRT-AVTEX every 2 s: takes vs re-uploads vs held buffers. (4) ~100 ms audio prefill before the
  video clock starts. (5) Log PRT-AVAUDIO: short/empty audio pulls in the first 5 s + audio vs
  decode thread cores. (6) `RYGT_DLSS_JITTER_SCALE=2` A/B (off) + log of the game's projection
  offset vs jitter passed to DLSS. Bats: b0 = 1 to 5 on, b1 = b0 + jitter x2, b2 = all off.
- **Build B (shadgt7 `64deefc0`, `56139d6d`, `54ab0de3`, 2026-11-19, owner go 00:56 GMT; on Build C):**
  - PRT-GTAUTO2 `RYGT_HD_RACE_NO_READBACK` (on, =0 old): with HD UI the race HUD flag is never set
    (`hud_depth_drawn` needs `HudSplitOn()`, off under HD UI), so `HdRace()` took GT Auto's 3D car
    for a race map. Owner logs (2026-11-18 race + GT Auto, 2026-11-19 GT Auto): every race frame
    `read-back false`, every GT Auto frame `read-back true`. A display read-back in the last 30
    frames now refuses the race sign; GT Auto takes the 3D menu path (MENU_TDLSS). Hold unchanged.
    Log `PRT-GTAUTO2` every 600 frames while refusing (+ sign picture address/size/samples, backup
    discriminator data); HDUI-SRC adds `PRT-GTAUTO2: N frames refused as race`.
    Handoff's `RYGT_HD_RACE_NEEDS_HUD` renamed (owner OK): it would have refused every race.
  - PRT-BINK30 `RYGT_BINK_30HZ` (on, =0 off): PERF-075 only raises the flip rate (guest vblank
    events stay at 60; ReportFlipStats counts only flip-eligible ticks). Bink threads mark activity
    from `LookupArea` (once per 64 lookups; BinkT00..02 seen there in owner logs); PresentThread
    locks flips to 30 while the last mark is < 1 s old, switching on an even vblank; nothing when
    the user's lock is <= 30. Log `PRT-BINK30: on (Bink thread X active, via ...)` / `off after N s`.
    Risk: Bink menu background videos would hold 30 (visible in the log).
  - PRT-KEYNORM `RYGT_PIPELINE_KEY_NORMALIZE` (on, =0 old key): key layout from the logs
    (color_buffers @184, 12 B each; blend @280; write_masks @312; samples @352; mrt @364; bitfields
    @368/@372; 376 B). The 22 PRT-KEEP near-misses are mostly baked state (cb0 format+swizzle,
    blend0, sample counts, prim type @372): real different pipelines. Normalised: colour targets
    past `num_color_attachments` (first pass fills every bound target, e.g. miss 14 = cb2/cb3
    only) and BlendControl (alpha fields only with separate alpha, unnamed bits/disable_rop3
    dropped). Loaded records normalised too, `PrewarmKeyLayout` stays 1 (no wipe). PRT-KEEP lines
    now name fields; every-100 line tallies them; PRT-PREWARM splits "not buildable" (936 in the
    warm log) by reason. Expect a modest drop only; the next lever is likely prim type (dynamic
    topology) or the not-buildable records, decided from the new lines.
    Note: DIAG-051 `padding` (2224 of 5250) = draws re-entering a pending async build, not dupes.
  - PRT-TESS (log only, rides `RYGT_PERF_LINE`): why tessellated draws find no pipeline key
    (unsupported / no program / hull constants / not translated / geometry), every 10 s. Tess
    batching itself is next, from these numbers.
- **Next build candidates (after the owner tests Build B; pick from its log lines, owner decides):**
  - Dynamic topology: primitive type is baked into the pipeline, so the same shaders drawn as list
    vs strip/fan are two pipelines (11 of 20 PRT-KEEP misses touch byte 372). Make topology dynamic
    state (VK 1.3 / EDS1) and drop it from the key for list/strip/fan; RectList/QuadList keep
    their own path. Touches the native draw path, own switch. Go if PRT-KEEP field tally still
    shows prim_type high.
  - Not-buildable prewarm records: 936 of 15137 records skipped in the warm log, built at runtime
    (stutter). Fix depends on the largest bucket in Build B's PRT-PREWARM split (colour targets,
    geometry, no vertex, stage/shader missing from the store).
  - Tess batching: FIX-227 LDS barrier runs are short (2 to 17 draws per run on average, up to
    ~144k draws per window). Batch more draws per barrier or drop unneeded barriers. First fix
    whatever PRT-TESS shows as skipped draws.

## Fix log (keep this up to date)
Every new commit on either `pr-tests` branch gets a line here: date, repo, commit, fix or switch,
default, CI run, test result.
| Date | Repo | Commit | What | Default | CI | Tested |
|---|---|---|---|---|---|---|
| 2026-11-16 | shadgt7 | d0d01781 | single DLSS on side viewport (`RYGT_DLSS_SIDE_VIEWPORT`) | on | 43778170083 | licence icon fixed |
| 2026-11-16 | shadgt7 | 57200074 | HD UI menus keep resolved picture (stopgap) | on (superseded) | 43860628121 | still shimmer |
| 2026-11-16 | shadgt7 | dcfd4f5b | `RYGT_HD_UI_MENU_TDLSS` temporal inputs for the proxy | off then | 43867936183 | no change alone |
| 2026-11-16 | shadgt7 | 6cba106d | late-scene restart (`RYGT_HD_UI_LATE_SCENE`), MENU_TDLSS=2, trace fixes | off then | 43963104510 | shimmer gone |
| 2026-11-16 | shadgt7 | c5569738 | FIX-GT7-187 3D partial fill device loss | always | 43980072900 green | owner test pending |
| 2026-11-16 | shadgt7 | f70fee41 | MENU_TDLSS on by default | on | 43980072900 green | owner test pending |
| 2026-11-17 | shadgt7 | d0235932 | PRT-PREWARM pipeline record + start prewarm (`RYGT_PIPELINE_PREWARM`), PRT-SPLASH splash progress (`RYGT_SPLASH_SHADER_PROGRESS`) | off then | 43985665386 green | prewarm works (5573 in 1.5 s); bar not shown; 305 near-miss keys |
| 2026-11-17 | rygt7 | a604a0a | merge upstream main FIX-GTS-270/271 (launcher 0.1.7, key removal) | - | 44163135699 green | pending |
| 2026-11-17 | shadgt7 | d3f0f826 | FIX-GT7-GHOST ghost car tyres (port of RyGT-Team/shadgt7#43, 7b170cbd) | always | 44163135699 green | pending |
| 2026-11-17 | shadgt7 | 50721c9b | PRT-KEEP shaders kept across builds; prewarm, splash progress, manifest record + pretranslate on by default; splash bar arms early, 2 s hold; near-miss key diagnostic | on (=0 off) | 44163135699 green | pending |
| 2026-11-17 | shadgt7 | c6066056 | FIX-PRT-DLSS: DLSS / HD UI DLSS never off for the run after a failure (VRAM-budget drop at 19:28 in owner log): ~120-frame pause then retry; temporal never refused for good | always | 44163135699 green | pending |
| 2026-11-17 | rygt7 | ecb2ec8 | launcher: shader rebuild + Show Splash checkboxes on by default; unchecked rebuild sends =0 | on | 44163135699 green | pending |
| 2026-11-17 | shadgt7 | c54af2d0 | PRT-PERF-A: PRT-ECORE driver compiles on E-cores (`RYGT_ECORE_DRIVER_COMPILE`), PRT-SAMPLER sampler dedupe + coarse stand-in (`RYGT_SAMPLER_DEDUPE`), PRT-PERF 10 s GPU/CPU line (`RYGT_PERF_LINE`) | on (=0 off) | 44193422305 green | pending |
| 2026-11-18 | shadgt7 | ff9c84d7 | PRT-LATE HD UI proxy scales right after its composite (`RYGT_HD_UI_LATE_TEMPORAL`), HD race hold (`RYGT_HD_RACE_HOLD`, 30), PRT-BINK cutscene diagnostic (`RYGT_DIAG_BINK=1`) | on (=0 off); BINK off | 44391891394 green | pending |
| 2026-11-18 | shadgt7 | f0c493f9 | Build C: PRT-GTAUTO GT Auto to presenter DLSS (`RYGT_HD_UI_LATE_GIVE_UP`), G1 `RYGT_HD_UI_LATE_SKIP_EARLY`, G2 `RYGT_HD_UI_OWN_VIEWPORT`; PRT-VRAM `RYGT_DLSS_VRAM_RESERVE` 384, `RYGT_DLSS_FREE_SIDE`, `RYGT_DLSS_RETRY_CALLS` 120; PRT-PERF VRAM | G3/V on (=0 off); G1/G2 off | CI 44418528531 green (artifact 14481664715) | pending owner test |
| 2026-11-19 | shadgt7 | 64deefc0 | Build B: PRT-GTAUTO2 GT Auto not an HD race when the display is read back (`RYGT_HD_RACE_NO_READBACK`) | on (=0 off) | 44434285472 green (artifact 14490302887) | b0 tested 2026-11-19: works (3835 frames refused, sharper); B2 test: no aliasing in GT Auto, UI still not fully steady |
| 2026-11-19 | shadgt7 | 56139d6d | Build B: PRT-BINK30 flips locked to 30 while Bink decodes (`RYGT_BINK_30HZ`) | on (=0 off) | 44434285472 green (artifact 14490302887) | b0 2026-11-19: wrong target (cutscenes are AvPlayer MP4; locked world map to 30), off in B2 |
| 2026-11-19 | shadgt7 | 54ab0de3 | Build B: PRT-KEYNORM key normalise + field-named PRT-KEEP (`RYGT_PIPELINE_KEY_NORMALIZE`), PRT-TESS skip reasons (log) | on (=0 off) | 44434285472 green (artifact 14490302887) | pending owner test |
| planned | shadgt7 | - | dynamic topology (prim_type out of the key), from Build B PRT-KEEP field tally | - | - | - |
| 2026-11-19 | rygt7 | c976977 | Build B2: PRT-AVSYNC AvPlayer video clock slews to audio (max 25% rate), snap only past 500 ms, late drop 50 to 100 ms, drift + snaps in the AVP 2 s line (`RYGT_AVP_SMOOTH_CLOCK`) | on (=0 off) | 44537997225 green (artifact 14541255275) | tested 2026-11-19: works, 0 to 1 late per 2 s, drift under 6 ms, 0 snaps (off: 3 to 6 late + 1 to 2 snaps). Keep on. Cutscene still skips visibly (cause elsewhere, see Build B3 plan) |
| 2026-11-19 | shadgt7 | fb672cb | Build B2: `RYGT_BINK_30HZ` now off by default (=1 on); PRT-JITTER log every 600 menu temporal frames (`RYGT_PRT_JITTER`) | BINK30 off, JITTER on | 44537997225 green (artifact 14541255275) | tested 2026-11-19: world map back to 60; GT Auto jitter x/y about -0.25..+0.25 px, 0 zero/repeated, 0 to 1 resets per 600 (maybe half scale vs DLSS +-0.5) |
| planned | shadgt7 | - | not-buildable prewarm records, from Build B PRT-PREWARM split | - | - | - |
| planned | shadgt7 | - | tess batching, from Build B PRT-TESS lines | - | - | - |
| planned | shadgt7 | - | shared manifest, load-screen background translate | off | - | - |

- Untouched: trophies upstream PRs (RyGT-Team/shadgt7#54, rygt7#58, open).

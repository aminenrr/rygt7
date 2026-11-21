# Handoff: Round 3 test build (written 2026-11-21)

You are a new agent with GitHub access and no history on this project. Read all of this before touching anything. If something here is unclear, ask the owner. Do not guess.

## 1. Who and what

- Owner: Amine, GitHub `aminenrr`. He tests builds of GT7 (Gran Turismo 7, serial CUSA24769) running on Windows through RyGT.
- Owner PC: Alienware m15 R7, RTX 3070 Ti laptop 8 GB, 1440p 240 Hz, Windows.
- Owner style: short informal English, absolute dates (2026-11-21, never "tomorrow"), no em or en dashes, results only, no filler.
- "Wait" from the owner means hold, do nothing.
- No code until the owner says go for a round. He has NOT yet said go for Round 3. Your first message to him is your plan, then wait for go.
- Plan before coding when scope changes. If you have any question, ask, do not guess.

## 2. Repos and branches (read carefully)

| Repo | Role | Branch you may push |
|---|---|---|
| aminenrr/shadGT | Renderer (libSceVideoOut.prx). Fork of upstream ryty-gpu. | `pr-tests` only |
| aminenrr/rygt7 | Emulator core, launcher (loader/RyGTLoader.cs), CI workflow, AGENTS.fork.md | `pr-tests` only |
| aminenrr/shadgt7 | Older renderer fork. Port source only. Read, never push. | none |

- Never push to `main`, `round2*`, `ui-hires-precompile`, or any branch other than `pr-tests`.
- Never push, comment, open PRs, or review on any RyGT-Team/* repo without asking the owner first. Reading is fine.
- Leave shadgt7#54 and rygt7#58 alone.
- Current heads (2026-11-21): shadGT pr-tests `b026dbf3`, rygt7 pr-tests `ff090f1` (doc commits may be newer, check).

## 3. Commit, push, CI rules

- Author and committer on every commit: `aminenrr <61759445+aminenrr@users.noreply.github.com>`. Set GIT_AUTHOR_* and GIT_COMMITTER_* env vars, do not change global git config.
- Push order: shadGT first, then rygt7. CI builds rygt7 and pulls the renderer from shadGT pr-tests head, so shadGT must be pushed before you start CI.
- Doc-only rygt7 commits end the message with `[skip ci]`.
- One feature per commit where files allow, message starts with the fix id, e.g. `PRT-STORECLEAN: ...`.
- One CI run per batch: workflow_dispatch of `rygt7-ci.yml` on aminenrr/rygt7, ref `pr-tests`. It takes about 60 to 75 min. Do not dispatch more than once per batch.
- A 403 at the step "Resolve exact RyGT7 main revision" is a GitHub rate limit, not your bug. Rerun failed jobs, max 2 reruns.
- Any other failure: read the job log, find the first compile error, fix only that, push, one new run.
- When green: the artifact to give the owner is `rygt7-windows-<rygt7 sha>-renderer-<shadGT sha>`. Put CI run id and artifact id in the Fix log (doc commit, `[skip ci]`).
- Round 2 reference: CI 44826767667 green, artifact 14676573189.

## 4. Building and checking

- There is no local Windows build. CI is the only real compile.
- Optional syntax check on Linux: `python3 -m ziglang c++ -std=c++23 -fsyntax-only --target=x86_64-windows-gnu` with shadGT externals submodules checked out (`git submodule update --init`, never stage or commit submodule changes). Expect environment-only errors (libc++ `std::atomic<std::shared_ptr>`, mingw headers, imgui config). Only errors in lines you changed matter.
- Launcher loader/RyGTLoader.cs is compiled with csc v4.0.30319 = C# 5. No C# 6+ syntax: no `$"..."`, no `?.`, no `=>` members, no `nameof`, no `out var`, no `??=`.
- Watch quotes inside C++ string literals (Round 2 broke one switch line with unescaped `"`).

## 5. Mandatory for every change

1. Its own `RYGT_*` environment switch so the owner can turn it off.
2. Register the switch in shadGT `src/video_core/rygt_switches.cpp`, one line, format:
   `"RYGTSW1|NAME|kind|group|default|values|description",`
   No duplicate NAME. No unescaped `"` inside the description.
3. A Fix log row and a section in rygt7 `AGENTS.fork.md` (table near the end, rows go above the `| planned | ...` rows). Row form: `| date | repo | commit | what (switches) | default | CI | test |`.
4. Log lines with a fixed prefix (e.g. `PRT-STORECLEAN:`) so the owner's stdout.log shows what happened.
5. Do not change behavior for non-GT7 titles (GT Sport is CUSA03220) unless the item says so.

The launcher has a custom environment variables setting, so the owner sets switches there. No .bat files needed unless you add a new required one (then say so).

## 6. Round 2 state (what the owner has now)

Round 2 items, all on by default, all tested 2026-11-20:
- PRT-PREWARM port (shadGT vk_pipeline_cache.cpp, ryty_entry.cpp, sdl_window.cpp, thread.cpp, vk_scheduler, perf_monitor): records pipelines while playing, builds them on the splash screen and in a launcher pass. Switches RYGT_PIPELINE_PREWARM, RYGT_PIPELINE_KEY_NORMALIZE, RYGT_SPLASH_SHADER_PROGRESS, RYGT_PREWARM_ONLY (launcher only), RYGT_PREWARM_DIRTY_MARK, RYGT_PERF_LINE. Works, no device loss.
- PRT-WARMSKIP (rygt7 loader/RyGTLoader.cs): launcher skips its pass if prewarm.stamp matches and no prewarm.dirty. Switch RYGT_PREWARM_WARM_SKIP.
- PRT-SRTUSE (srt_walker_storage.h, flatten_extended_userdata_pass.cpp): log only. RYGT_SRT_USE_LOG.
- PRT-VRAMAUTO (vk_instance.cpp/.h): Auto = device local minus 1024 MB. Shows 6993 MB on the owner card. RYGT_VRAM_AUTO_RESERVE_MB.
- PRT-TROPHY (ryty_app0, ryty_trophy, trophy_toast_layer): works, pop-up on by default.
- PRT-AVDEF (rygt7 libSceAvPlayer.native/Export.cpp): cutscene fixes default on. Works.
- PRT-HOMEBTN (launcher Play/gear buttons).

Owner launcher settings: "Precompile shaders on the splash screen" ON, "Precompile graphics shaders in the launcher" ON, the ryty_prewarm.exe toggle OFF, "When new shaders compile" = Keep going.

Round 2 test results (logs were in the owner's upload; summary in AGENTS.fork.md "Round 2 owner test"):
- Trophy pop-up and cutscenes work.
- First run on a partly compiled track: texture pop in and out. New track: texture anomalies while compiling. Both gone on the second run of the same track.
- Freezes with a lower right notice "Compiling shaders (n)  X s" or "Loading  X s". Lower left is a different, older notice.
- Shader folder grows fast in PS4 Pro mode: 4.47 GB after 2 or 3 tracks.
- Audio crackle and stutter in game.
- Prewarm records grew 995 -> 1177 -> 4924 across sessions. Warm skip never fired.

## 7. Round 3 items (1 to 8). Plan each, send the plan, wait for go.

### 1. PRT-STORECLEAN: delete stale shader stores
Owner shaders folder (game folder `shaders\`), 4467 MB total:
- `CUSA24769-direct-sets.v1` 2437 MB: live store (14534 .spv, 14534 .meta, 7164 .key).
- `CUSA24769-direct-sets` 815 MB: old store. shadGT `src/video_core/cache_storage.cpp` (FIX-271) keeps an incompatible old store and opens a sibling with a version suffix. So the old one is never used again.
- `CUSA24769-10de-24e0-9a4a8000-<hash>.vkpipelinecache` 728 MB: live driver pipeline cache (name built in vk_pipeline_cache.cpp around line 3447).
- `CUSA24769.vkpipelinecache` 387 MB: old name from earlier builds. Likely unused, verify in code that nothing still reads it.
- Rest (capture 49 MB, manifest 32 MB, warm_compute 15 MB, prewarm 4 MB): keep.
Do: once the renderer has opened its live store, delete sibling stores that FIX-271 set aside and the old generic vkpipelinecache. Log each deletion with size. Never delete the store in use, and never delete stores of other GPU families if that could break a GPU swap; ask the owner if unsure. Switch e.g. RYGT_SHADER_STORE_CLEANUP (on, 0 = keep). Saves about 1.2 GB.

### 2. PRT-STORECOMPRESS: compress the shader store
The .spv files are uncompressed SPIR-V (median 17 KB). Compress on write (miniz is in shadGT externals), read both compressed and plain so old files still work, or bump the store version (one-time retranslate, tell the owner). Estimate 3 to 5x smaller, measure it and log the ratio. Watch load time; decompression must not add stutter on the render thread. Switch e.g. RYGT_SHADER_STORE_COMPRESS.

### 3. Driver cache trim (investigate first)
The live vkpipelinecache is 728 MB. Find out if it keeps dead entries across sessions (e.g. merged and never pruned) and whether it can be capped or rebuilt from the prewarm records. Report findings to the owner before coding.

### 4. PRT-STALL: freezes with "Compiling shaders" / "Loading"
- The lower right notice is shadGT FIX-GT7-100, `src/imgui/compile_notice_layer.cpp` (switch SHADGT_COMPILE_NOTICE). It appears when a frame is stuck 500 ms+. It marks the freeze, it does not cause it.
- "Compiling shaders (n)" = frame waits on pipeline builds. In vk_pipeline_cache.cpp look at `wait_for_shaders && OneOffBounded()` (around lines 5963 and 6045) and `LearnedOneOffWait` (around 8573): draws into one-off targets wait for their pipeline even in Keep going mode.
- "Loading" = stuck with zero build jobs. Find what it waits on (texture upload, readback, fence).
- Goal: no multi-second freeze in race. Options: do not block on one-off targets in race, or cap the wait. Do not reintroduce the 185 s one-frame first load FIX-GT7-100 was made for. Present options to the owner first.

### 5. PRT-AUDIOMIX: port upstream RyGT-Team/rygt7#70
- PR #70 "fix(audio): mix SDL output in a single callback" by GustavoPeetry, open, head 1cd4348772, 2 files, +1391/-213. One SDL output callback mixes all guest audio ports; Windows audio and DSP threads get priorities.
- Our fork's audio work is only for cutscenes (AvPlayer). #70 is different and could fix in-game crackle.
- Port it onto rygt7 pr-tests as our own commit with a RYGT_* switch that restores the old per-port path. Do not comment on or touch the upstream PR. Check it does not fight the PRT-AVDEF cutscene audio prefill.

### 6. Texture pop in and out / anomalies on first runs
Find whether these are draws skipped while pipelines compile (Keep going) or texture streaming. Likely shrinks with item 4. Investigate and report; code only after go.

### 7. Warm skip check
The renderer writes `<exe folder>\prewarm.dirty` the first time a session records a pipeline not seen before (vk_pipeline_cache.cpp around lines 511 and 7230). In Round 2 every session drove new tracks, so the skip correctly never fired. First ask the owner to launch twice with nothing new in between. Only change code if it still reruns (e.g. dirty set by pipelines the prewarm already has).

### 8. Item 4: not-buildable prewarm records
Records that fail to build in the prewarm get retried every launch. Mark them not buildable (with renderer and driver version, so a driver update retries them) and skip them. Port idea from shadgt7 if it has one, check its history. Switch e.g. RYGT_PREWARM_SKIP_UNBUILDABLE. Log how many were skipped.

## 8. Parked (do not start; mention to the owner, he may pick one for Round 4)
- Tess: tessellation stage not translated + FIX-227 barrier batching.
- Dynamic topology (low).
- shadgt7 #45.
- rygt7 #65 (upstream: Mbus shadowing SaveData exports).
- Pro mode sharpen and checkerboard resolve check.
- Garage nvoglv64.dll crash (watch only).
Dropped for good: DLSS, HD UI, GT Auto, race name tags at 2x HUD.
Remind the owner of the parked list each time he reports a test.

## 9. Known risks
- shadGT's own GT7 prewarm (PERF-038, SHADGT_PREWARM) stays OFF on GT7: it lost the device at the title screen on an RTX 4070. Do not turn it on.
- 40 "read-write alias of shared guest memory" throws in stderr in earlier runs (watch).
- Pro mode is the heaviest: check VRAM peak in PRT-PERF lines.

## 10. Delivering a round to the owner
1. Plan message, wait for go.
2. Code, commit (author above), push shadGT, push rygt7, one CI run.
3. On green: Fix log CI and artifact ids (doc commit [skip ci]).
4. Send the owner: artifact name and id, one line per change, a numbered test checklist, the kill switch for each change, ask for stdout.log and stderr.log, remind the parked list.

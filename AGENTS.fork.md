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
- CI: `.github/workflows/rygt7-ci.yml` runs on every push to `pr-tests` (or by hand, Run workflow).
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

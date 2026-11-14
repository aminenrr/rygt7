# AGENTS.fork.md — branch `trophies` (aminenrr's RyGT7 fork)

Context for any agent (human or AI) working on this branch. Written 2026-11-14.

## What this branch is
- Feature branch for the **trophy system**, stacked on `pr-tests` (rygt7 1168071, shadgt7 7c8e9ced),
  so its CI build also carries HD UI, temporal DLSS under HD UI, DualSense HD haptics and FIX-GT7-184.
- Renderer: `aminenrr/shadgt7` branch `trophies`. CI (`rygt7-ci.yml`) runs on pushes to `trophies` and
  builds with that renderer branch head (`build-support/renderer.json` pins the same).
- Trophy work is kept in its own commits so it can be rebased onto upstream `main` for a PR later.
  Never push, comment or open PRs upstream; the owner posts upstream.
- Next: `precompile-port` (GT Sport shadGT #40/#42/#44 + RyGTS#46 port) stacked on this branch, only
  after the owner confirms trophies work. Not started.
- Retired 2026-11-14: `round2*`, `ui-hires-precompile` (superseded by pr-tests: HD UI replaced UI-HIRES,
  black cars fixed upstream, temporal DLSS upstream). The compile pack / shader-object quick win and
  the -neo / track-first precompile drafts are dropped; the precompile port replaces them.

## Trophy plan (feature table)
| Step | ID | What | Status |
|---|---|---|---|
| 1 | FIX-GT7-175 | npbind.dat, trophy/*.trp, icon0.png, param.sfo resolved through the app0 layers (update=, game= folders); cause of "No npCommId found". On by default, RYGT_SCE_SYS_LAYERS=0 restores app0 only | planned |
| 2 | FEAT-GT7 (new) | Trophy key (launcher, DPAPI) -> temp file in RYGT_TROPHY_KEY_FILE; TRP extract to user\trophy\<commId>; TROPCONF check; per-user list seeded; user id 0x10000000+slot -> 1000+slot | planned |
| 3 | FEAT-GT7 (new) | Unlocks persist across restarts; Platinum unlocks itself | planned |
| 4 | FEAT-GT7 (new) | In-game toast in Trophy-Notifier style (github.com/aminenrr/Trophy-Notifier src/toast.c), assets imported from the user's Trophy-Notifier install, RYGT_TROPHY_TOAST=0 hides | planned |
| 5 | FEAT-GT7 (new) | Launcher Trophies section: key field, Test, earned / not earned list with icons, asset import, toast toggle; launcher 0.1.7 | planned |
Nothing trophy-related happens without a key, except step 1.

## pr-tests base (as inherited)

### How pr-tests is built
- Base: **upstream** `RyGT-Team/rygt7` `main` (not the fork's `main`, which lags behind).
- Renderer: `aminenrr/shadgt7` branch `trophies` = upstream `RyGT-Team/shadgt7` `main` + the
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

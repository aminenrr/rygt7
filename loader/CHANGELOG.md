# ryGT7 launcher changelog

The launcher (RyGT7.exe) is versioned on its own with semantic versioning
(MAJOR.MINOR.PATCH), separately from the game, emulator and release packages.
The version lives in `loader/Version.cs`. To release a change, bump
`LauncherVersion.Value` there and add a section here with the same number;
CI fails when the two disagree.

## 0.1.8 - 2026-11-19

* Shaders page: adds an opt-in "Precompile shaders before starting game" toggle (`GPU.precompile_before_play`)
  that runs `tools\ryty_prewarm.exe` before launching `GT7.exe` with live progress, clean cancellation,
  incremental SPIR-V store auditing, `.meta` OS page-cache pre-warming, and monotonic `VkPipelineCache`
  compute pipeline warming.
* PRT-B5 (pr-tests): Shaders page toggles "Precompile shaders on the splash screen" (`GPU.splash_prewarm`,
  on by default; off sends `RYGT_PIPELINE_PREWARM=0`) and "Precompile graphics shaders in the launcher before
  starting the game" (`GPU.launcher_graphics_prewarm`, off by default): GT7.exe runs hidden with
  `RYGT_PREWARM_ONLY=1`, builds the recorded pipelines on every core with "Precompiling graphics shaders (n/N)"
  and Cancel, then the game starts.

## 0.1.7 - 2026-11-16

* FIX-GTS-271: GT7's online-service keys are no longer in ryGT7. RyGTServer reads them from your own
  GT7 eboot.bin, which the Online page passes when it starts hosting; without it, GT7's online
  service stays off.
* FEAT-GT7-187: a Trophies page. Enter your own 32-character trophy key once (Show / Hide); it is kept
  encrypted for your Windows account (DPAPI, user\trophy.key.dpapi) and never written in plain text or
  logged. Test checks it against the game's sce_sys\trophy\trophy00.trp (ryGT7's app0, then the update and
  game folders), accepting Sony's <!--Sce-Np-Trophy-Signature--> comment before <trophyconf>
  (FIX-GT7-188), and says what it tried when a key really fails. A passing Test shows the trophy pop-up as
  the game draws it, with its sound, for Bronze, Silver, Gold or Platinum (also from Pop-ups > Preview);
  "Show this test pop-up the next time I play" shows it once in game.
* The page lists every trophy with its icon, name, description and grade, hidden ones until earned, whether
  it is earned and when, with "N of M earned" and Refresh. It is read in the background ("Loading
  trophies...") so the launcher never freezes, and icons are kept small in
  user\trophy\<npCommId>\launcher_icons after the first read (FEAT-GT7-189).
* At Play the key reaches the game through a temporary file only your account can read
  (RYGT_TROPHY_KEY_FILE), deleted when GT7 exits. "Show trophy pop-ups" is on by default.
* ryGT7 ships its own pop-up files: original cups and chimes and the Inter font (SIL OFL 1.1), embedded in
  RyGT7.exe and written to user\trophy_builtin at Play. "Open custom trophy images and sounds folder" opens
  user\trophy_assets: files put there (bronze/silver/gold/platinum.png, trophy.wav, platinum.wav, font.ttf or
  font.otf; README.txt there lists sizes and formats) replace the built-in ones, one by one.
* Trophy progress is kept next to the saves: _sd\trophies_progress\<npCommId>.xml (other players in
  1000+slot folders). An older user\home\<1000+slot>\trophy file is copied once and kept as a backup;
  RYGT_TROPHY_PROGRESS_SD=0 keeps the old place.
* Renderer: trophy extraction, unlocks and pop-ups (shadgt7 FIX-GT7-175, FEAT-GT7-185/186/189, FIX-GT7-188).

## 0.1.6 - 2026-11-15

* FEAT-GTS-265: Controls > Controller has an HD rumble & triggers window (it replaces the Feedback slider,
  which moved into it). A slider per effect, 0-200% with 0 turning it off: HD rumble overall, engine, road
  and kerbs, wheel slip, gear changes, rev limiter and impacts; trigger strength, brake and accelerator
  resistance, how much the resistance follows speed, braking and revs, ABS (wheel-lock) and wheelspin
  vibration. Changes apply while playing (renderer shadgt7 FEAT-GT7-179). Each effect has a Test button
  that plays it on the DualSense through tools\ryty-feel-test.exe, game running or not.

* FEAT-GTS-269: Settings > About carries NVIDIA's DLSS attribution, as the NVIDIA RTX SDKs License
  asks of software shipping its NGX loader: the package now includes the DLSS plugin
  (dlss\ryGT_dlss.dll), which contains it. NVIDIA's runtime still comes from Download NVIDIA DLSS.

## 0.1.5 - 2026-11-14

* FEAT-GT7-181: Graphics page, Upscaling. With NVIDIA DLSS, a "Texture detail" list sets how much finer the
  renderer's temporal DLSS reads the game's textures: Full (the default: as much as DLSS enlarges the picture
  plus one level, as NVIDIA advises, so 2 levels to 3840 x 2160 and about 1.4 to 2560 x 1440), Reduced (one
  level less) or Off. The note gives the amount for the chosen quality. The row only shows with NVIDIA DLSS
  through Streamline, no frame generation, and an installed renderer that has the setting; it applies at the
  next game start.

## 0.1.4 - 2026-11-14

* FEAT-GT7-180: Graphics page. NVIDIA DLSS and the PS4 Pro 3200 x 1800 picture can no longer be turned on
  together: the game rebuilds that picture itself, so picking one while the other is on is refused with a
  message, and a configuration that already has both shows a warning in both sections. NVIDIA DLSS frame
  generation shows a line saying it is not recommended for now (in our tests, on an RTX 4070, the game ran
  smoother without it). The 60 fps race frame rate is no longer labelled experimental. The PS4 Pro mode
  section loses its last paragraph (the test notes).

## 0.1.3 - 2026-11-13

* FEAT-GT7-161: Photos the game's gallery exports (Export on a Scapes or race photo) are written as
  JPEGs to a `Photos` folder next to RyGT7.exe. Set up creates the folder, updates never touch it, and
  About > Folders has a row that opens it.

## 0.1.2 - 2026-11-12

* FEAT-GTS-254: An Advanced page (last in the settings sidebar) for the renderer's experimental `RYGT_*`
  and `SHADGT_*` switches, ported from the RyGTS launcher. The launcher reads them from
  `libs\libSceVideoOut.prx` itself, with the renderer's own descriptions as hover tooltips; on/off
  switches are toggles, others text boxes. Search, sort by column, Reset all, Refresh after a renderer
  rebuild, and a table for any other environment variable. Saved as `env=` lines in `RyGT7.ini` and set
  for `GT7.exe` after the launcher's own variables. The page is hidden until About > Logging >
  "Show the Advanced page" is turned on; switches set there keep applying when it is hidden again.

## 0.1.1 - 2026-11-11

* FEAT-GTS-251: Set up no longer links the game's files into `app0` (Explorer counted the links,
  and those in every `app0.previous-*` folder, as more copies of the game): the game reads them
  from the game and update folders. Links left by earlier versions are removed, and a previous
  `app0` holding nothing else goes with them; copies are kept. ryGT7 can be on any drive.

## 0.1.0 - 2026-11-11

* First tracked launcher version.
* The version is in the exe's file properties (File version, Product version),
  in the launcher window, and on the first line of `setup.log`.
* CI packages record it as `launcher_version` in their build information.

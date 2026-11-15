# Built-in trophy pop-up files (FEAT-GT7-189)

Embedded in RyGT7.exe as `trophy.<file>` resources (see `build.cmd`) and written to
`user\trophy_builtin` at Play. The game uses them for any file the user's custom folder
(`user\trophy_assets`) does not have.

| File | What | Origin / licence |
|---|---|---|
| bronze.png, silver.png, gold.png, platinum.png | 256 x 256 cups | Original, drawn for ryGT7 by `tools/gen_cups.py` (same licence as this repository) |
| trophy.wav, platinum.wav | 48 kHz 16-bit mono chimes | Original, synthesised by `tools/gen_sounds.py` (same licence as this repository) |
| font.ttf | Inter SemiBold | SIL Open Font License 1.1, see `font-LICENSE.txt` |

No Sony or Trophy-Notifier files are included.

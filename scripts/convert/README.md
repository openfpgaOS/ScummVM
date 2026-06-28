# Game-data conversion toolkit

Pure-Python tools that turn original/Steam/GOG game data into the slot-4
containers the ScummVM port loads (cooked ISO9660 images, `.cue/.bin` pairs,
and the `CDDA.SOU` CD-audio file). No `mkisofs`/`xorriso`/`bsdtar`/`pycdlib`
needed; only `numpy` (for audio transcodes) and, for the Monkey SE music decode,
`ffmpeg`.

Run any tool with `-h`/`--help`. Outputs go to `<repo>/build/<game>/` by
default; nothing is written into tracked locations.

## Reusable libraries (`lib_*` — import or use as a CLI)

| Module | What it does | Key API |
|--------|--------------|---------|
| `lib_iso9660.py` | Read **and** write cooked ISO9660 level-1 images (the format `of_iso_mount` and the app-side parser expect). | `build_iso(out, [(name, src), …], volid)` · `read_iso(iso)` · `extract_iso(iso, dir)` · CLI: build, `--extract` |
| `lib_cdda.py` | Transcode raw 16-bit stereo PCM ⇄ ScummVM's `CDDA.SOU` block format (Loom CD audio). | `encode(src, out, start_byte, n_sectors)` · `decode_block(block)` |
| `lib_cuebin.py` | Assemble a single-file `MODE1/2352 + CDDA` `.bin/.cue` the port's cue parser accepts. | — |
| `lib_lpak.py` | Extract LucasArts Special-Edition `KAPL`/LPAK `.pak` archives. | — |
| `lib_xwb.py` | Extract XACT3 `.xwb` wave banks (SE speech/music). | — |
| `lib_mi1_trackmap.py` | Authoritative MI1 CD-audio track map (Red-Book +1 offset). | data table |

These are dependency-light and game-agnostic where possible — reuse them when
adding a new game rather than re-implementing ISO/CDDA handling.

## End-to-end packagers (`build_*` — run directly)

| Script | Game(s) | Produces |
|--------|---------|----------|
| `build_loom_iso.py` | Loom VGA/CD | a slot-4 ISO with the data files **plus** `CDDA.SOU` transcoded from a raw full-disc rip, at the exact frame origin the engine expects (`--raw loom.iso --data loom_2048.iso`). |
| `build_monkey_iso.sh` | MI1 / MI2 Special Edition | slot-4 ISOs with SE voices (+ MI1 CD music) from a Steam SE install. Override the source dirs with `--mi1-dir` / `--mi2-dir` (defaults to `$HOME/.local/share/Steam/...`, or set `$STEAM_COMMON`). |
| `build_mi1_music.py` | MI1 SE | decode the SE music bank → port-playable CD-audio WAVs (needs `ffmpeg`). |
| `build_mi1_speech.py` | MI1 SE | repack/shrink `Speech.xwb` (MS-ADPCM or downsampled PCM). |
| `build_min_pak.py` | MI1 / MI2 SE | a minimal KAPL/LPAK `.pak` with only the classic data files. |

## Worked examples

```bash
# Loom CD — bake the orchestral CD audio into the data iso as CDDA.SOU
python3 scripts/convert/build_loom_iso.py \
    --raw  /path/to/loom.iso          \  # raw MODE1/2352 full-disc rip (data + audio)
    --data /path/to/loom_2048.iso        # cooked data-only iso (LFL/LEC/EXE)
# -> build/loom/loom.iso   (keep loom_os.ini variant=VGA)

# Monkey Island 1 & 2 Special Edition — voices + music from a Steam install
scripts/convert/build_monkey_iso.sh all \
    --mi1-dir "/path/to/The Secret of Monkey Island Special Edition" \
    --mi2-dir "/path/to/Monkey2"
# -> build/monkey/monkey1.iso, build/monkey/monkey2.iso

# Make a plain cooked ISO from loose game files (any engine)
python3 scripts/convert/lib_iso9660.py samnmax.iso SAMNMAX path/to/*.LFL path/to/MONSTER.SOU
```

See the repository [README](../../README.md) (“Game data” and “Music” sections)
for how these containers slot into a game instance.

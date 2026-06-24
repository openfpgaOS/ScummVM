# ScummVM for Analogue Pocket (openfpgaOS)

A native port of [ScummVM](https://www.scummvm.org/) to the Analogue Pocket
running [openfpgaOS](https://github.com/ThinkElastic/openfpgaOS-SDK) — a
bare-metal RISC-V game runtime (VexiiRiscv rv32imafc @ 100 MHz, 64 MB SDRAM,
320×240 video, 48 kHz stereo audio). It plays the classic LucasArts SCUMM
point-and-click adventures, with MIDI + CD-style music, save slots in
non-volatile storage, and one launcher entry per game.

Each game is a separate **instance** that shows up directly in the Pocket's
core menu. Pick *Monkey Island 1* and you boot straight into the game — there
is no on-device ScummVM launcher to navigate.

---

## Games supported

This build compiles the **SCUMM engine, versions 0–6 only**. That covers the
classic LucasArts catalogue; it deliberately excludes SCUMM v7/v8 (Curse of
Monkey Island, The Dig, Full Throttle — 640×480, compressed resources),
Humongous Entertainment titles, and every non-SCUMM engine (Sierra SCI,
Beneath a Steel Sky, GrimE, …). See [Limitations](#limitations).

| Game | gameid | Music | Speech | Status |
|---|---|---|---|---|
| The Secret of Monkey Island (SE) | `monkey` | ✓ (CD audio, offline-decoded) | ✓ SE voices | **Tested** |
| Monkey Island 2: LeChuck's Revenge (SE) | `monkey2` | ✓ iMUSE MIDI | ✓ SE voices | **Tested** |
| Day of the Tentacle | `tentacle` | ✓ MIDI | ✓ digital | **Tested** |
| Indiana Jones and the Fate of Atlantis | `atlantis` | ✓ MIDI | ✓ digital | **Tested** |
| Loom | `loom` | ✓ MIDI | — | Tested |
| Maniac Mansion, Zak McKracken, Indy 3, … | (various) | ✓ MIDI | — | Should work (SCUMM v0–v6); untested |

"SE" instances run the Steam **Special Edition** data in *classic* mode: the
original pixel-art game with the remastered SE voice-over mixed in. The SE
*remastered* graphics/UI are not used (320×240, 8-bit). See
[Special Edition: MI1 / MI2](#special-edition-mi1--mi2) for how the audio is
built.

---

## Quick start

If you already have the SD-card-ready ZIP from a release, just unzip it to the
root of your Pocket's SD card, add your game data (below), and boot. To build
from source:

```sh
# 1. Build the core (host toolchain) and copy it to the SD card
make build CORE=scummvm
make copy  CORE=scummvm          # auto-detects the SD card; or POCKETDEV=/path

# 2. Add a game's data file next to its instance, e.g.
#    <SD>/Assets/scummvm/ThinkElastic.ScummVM/monkey1.iso

# 3. Insert SD, boot Pocket → Cores → ThinkElastic.ScummVM → pick a game
```

No RISC-V toolchain installed? Use the **container build** instead — it needs
only Docker:

```sh
cd src/scummvm
make container-all               # compiles scummvm.elf + SD tree inside Docker
cd ../..
make copy CORE=scummvm           # rsync to SD card (host-side, no toolchain)
```

See [Building from source](#building-from-source) for both paths in full.

---

## How a game is laid out

The Pocket's launcher binds files to numbered **data slots** defined in
`Cores/ThinkElastic.ScummVM/data.json`. An **instance JSON** says which file
goes in which slot for one game; the port reads them at boot by scanning the
mounted root.

| Slot | Contents | Filename | Notes |
|---|---|---|---|
| 0 | Instance manifest | `<Game>.json` | The launcher entry itself |
| 1 | OS kernel | `os.bin` | Shared, shipped by the core |
| 2 | OS / launch config | `<game>_os.ini` | Per-game; `[os]` + `[scummvm]` (see below) |
| 3 | Engine binary | `scummvm.elf` | Shared, built from source |
| 4 | **Game data** | `<game>.iso` / `.zip` / `.cue` | **You provide this** |
| 5 | MIDI sound bank | `bank.ofsf` | Shared soundfont for the synth |
| 7 | Raw disc image | `<game>.bin` | Only for `.cue`/`.bin` games (paired with slot 4) |
| 9 | ScummVM settings | `<game>.ini` | Non-volatile, 256 KB |
| 10–18 | Save slots 0–8 | `<game>_0.sav` … `<game>_8.sav` | Non-volatile, 256 KB each — **9 saves** |

Slots 9–18 are non-volatile flash regions, written transparently as you play.
Delete the `.sav` files to wipe saves.

---

## Adding a game

A game needs three things on the SD card: a **launch config** (slot 2), an
**instance JSON** (slot 0), and a **data file** (slot 4). The five instances
this core ships are the templates — copy one and change the names.

### 1. Write the launch config — `<game>_os.ini`

Plain INI, lives in `Assets/scummvm/common/`. The `[os]` section tells the
launcher which ELF to run; the `[scummvm]` section is read by the port at boot
(`main.cpp`) to pick the game and how to mount its data.

```ini
[os]
ELF=scummvm.elf
ARGS=monkey

[scummvm]
gameid=monkey
engineid=scumm
description=The Secret of Monkey Island
platform=pc
language=en
music=openfpga
variant=SE
data_file=monkey1.iso
cd_track_offset=0
voices=true
```

**`[scummvm]` keys** (max lengths in parentheses):

| Key | Required | Default | Meaning |
|---|---|---|---|
| `gameid` (31) | **yes** | — | ScummVM canonical id: `monkey`, `monkey2`, `tentacle`, `atlantis`, `loom`, `maniac`, `zak`, … |
| `engineid` (31) | no | `scumm` | Always `scumm` in this build |
| `description` (95) | no | `gameid` | Name shown while booting |
| `platform` (15) | no | `pc` | `pc`, `amiga`, `mac` — affects resource detection |
| `language` (7) | no | `en` | ISO 639-1 code: `en`, `de`, `fr`, `es`, `it`, `ja`, … |
| `music` (15) | no | `openfpga` | `openfpga` = hardware-PCM sample synth (`bank.ofsf`) |
| `variant` (15) | no | *(auto)* | Pin a variant from ScummVM's table: `SE`, `VGA`, `EGA`, `""` = first match |
| `data_type` (7) | no | `""` | `""` = ISO/ZIP; `cue` = raw BIN/CUE disc |
| `data_file` (159) | no | — | Slot-4 filename: must match what you drop on the SD |
| `cue_file` (159) | no | — | `.cue` member path when `data_file` is a ZIP wrapping a CUE/BIN pair |
| `subdir` (63) | no | `""` | Subdirectory for compilation discs (e.g. one ISO, many games) |
| `cd_track_offset` | no | `0` | Added to every CD track number (compilation discs renumber tracks) |
| `voices` | no | `false` | Enable SE remastered speech (MI1/MI2 only; needs `variant=SE` + `Speech.xwb` on the disc) |

The port does **not** run ScummVM's MD5 auto-detection (it would stall the
boot link). The config above is authoritative — a wrong `gameid`/`variant`
picks the wrong game or fails to start.

### 2. Write the instance JSON — `<Game>.json`

Lives in `Assets/scummvm/ThinkElastic.ScummVM/`. The filename is what the
launcher shows. It just maps slots to filenames:

```json
{
    "instance": {
        "magic": "APF_VER_1",
        "variant_select": { "id": 666, "select": false },
        "data_slots": [
            { "id": 1,  "filename": "os.bin"         },
            { "id": 2,  "filename": "monkey1_os.ini" },
            { "id": 3,  "filename": "scummvm.elf"    },
            { "id": 4,  "filename": "monkey1.iso"    },
            { "id": 5,  "filename": "bank.ofsf"      },
            { "id": 9,  "filename": "monkey1.ini"    },
            { "id": 10, "filename": "monkey1_0.sav"  },
            { "id": 11, "filename": "monkey1_1.sav"  },
            { "id": 12, "filename": "monkey1_2.sav"  },
            { "id": 13, "filename": "monkey1_3.sav"  },
            { "id": 14, "filename": "monkey1_4.sav"  },
            { "id": 15, "filename": "monkey1_5.sav"  },
            { "id": 16, "filename": "monkey1_6.sav"  },
            { "id": 17, "filename": "monkey1_7.sav"  },
            { "id": 18, "filename": "monkey1_8.sav"  }
        ]
    }
}
```

For a `.cue`/`.bin` game, point slot 4 at the `.cue` and add slot 7 for the
`.bin`:

```json
{ "id": 4, "filename": "monkey1.cue" },
{ "id": 7, "filename": "monkey1.bin" },
```

### 3. Provide the game data (slot 4)

Three delivery formats are supported. Drop the file alongside the instance JSON
at `Assets/scummvm/ThinkElastic.ScummVM/`.

- **ISO 9660** (`.iso`) — the kernel mounts it at `/cd` and the engine reads it
  as a normal directory. Best for CD games. **Recommended.**
- **CUE + BIN** (`.cue` + `.bin`) — a `MODE1/2352` data track plus Red Book
  audio tracks, for games whose music is CD audio (see
  [Creating game ISOs](#creating-game-isos)).
- **ZIP** (`.zip`, **STORE / `-0`**) — a no-compression archive read in place
  via ScummVM's `SearchMan`. Good for loose-folder distributions (GOG/Steam
  extracts). Entries must be stored, not deflated. Files ≤ 16 MB are pulled
  into RAM on first access; larger files are streamed from the SD.

> **Size cap:** a single slot file must stay under ~2 GB. If a game is bigger,
> split it across a second data slot or trim unused assets.

---

## Creating game ISOs

The port ships pure-Python ISO/CUE builders (no `mkisofs`, `xorriso`, or
`pycdlib` needed) plus a turnkey script for the Monkey Island Special Editions.

### Roll your own from a folder of game files

```sh
# Bare ISO 9660 image (keeps full, untruncated filenames):
scripts/lib_iso9660.py monkey1.iso SCUMMVM  monkey1.000 monkey1.001 ...

# Or a CUE/BIN disc with CD-audio tracks (track2.wav, track3.wav = Red Book PCM):
python3 - <<'PY'
from scripts.lib_cuebin import build_cuebin
build_cuebin("monkey1.bin", "monkey1.cue", "monkey1.iso",
             [(2, "track2.wav"), (3, "track3.wav")])
PY
```

`lib_iso9660.py` writes a sector-aligned ISO 9660 Level 1 image and preserves
long names (`speech.info` → `SPEECH.INF;1`, not truncated), which the engine
needs. `lib_cuebin.py` wraps that ISO as track 1 (`MODE1/2352`) and appends
44.1 kHz / 16-bit stereo PCM audio tracks.

If you already have a real CD, a plain rip works too:

```sh
dd if=/dev/sr0 of=mygame.iso bs=2048      # data-only games
```

Verify any image in **desktop ScummVM** before deploying.

### Special Edition: MI1 / MI2

`scripts/build_monkey_iso.sh` turns the **Steam Special Edition** install into
port-playable ISOs with SE voices — no transcoding of speech, because the
on-device decoders handle the SE banks directly (MI1 `Speech.xwb` = PCM,
MI2 = MS-ADPCM):

```sh
scripts/build_monkey_iso.sh all \
    --mi1-dir "/path/to/Monkey Island 1 SE" \
    --mi2-dir "/path/to/Monkey Island 2 SE" \
    --out ./build/monkey
# → build/monkey/monkey1.iso, build/monkey/monkey2.iso
```

What it does:

- Slims each `monkey?.pak` DoubleFine container down to the classic data
  (~5 MB) and ships it whole — the `variant=SE` flag makes the engine read
  resources from the `.pak` and inject the SE speech over the classic scripts.
- **MI1 music:** the SE music bank is xWMA, which the port **cannot decode at
  runtime** (the WMA Pro path is a stub). The script decodes it **offline** with
  `ffmpeg` into `track%d.wav`, and the engine plays those as classic CD audio.
  Without `ffmpeg` the ISO still builds — just music-less.
- **MI2 music:** untouched — it is iMUSE AdLib/MT-32 MIDI through the openfpga
  synth, independent of the SE audio.

Match the build with `variant=SE` and `voices=true` in the game's
`<game>_os.ini` (the shipped `monkey1_os.ini` / `monkey2_os.ini` already do).

---

## Controls

The Pocket has no keyboard or mouse, so the gamepad drives a virtual cursor and
the common adventure-game keys:

| Input | Action |
|---|---|
| D-pad / analog stick | Move mouse cursor |
| Hold **L1** / **R1** while moving | Slow / fast cursor |
| **A** | Left click (walk / interact) |
| **B** | Right click (default verb / inventory) |
| **X** | Enter / Return |
| **Y** | Space (skip cutscene / pause) |
| **START** | F5 (in-game save/load menu) |
| **SELECT** (hold) + D-pad ↑/↓ | Master volume |
| **SELECT** (hold) + D-pad ←/→ | Music volume |
| **SELECT** (tap) | Toggle numeric keypad (for typed copy-protection codes) |

In **keypad mode**, the buttons map to digits (D-pad ↑↓←→ = 1/2/3/4, A/B/X/Y =
5/6/7/8, L1/R1 = 9/0, START = Enter) so games that demand a typed code are
playable. Copy protection is disabled by default (`copy_protection=false`); set
it to `true` in the game's `.ini` to re-enable the original dial/code screens.

---

## Audio

- **MIDI (iMUSE / AdLib):** music routes through a sample-based synth driven by
  the `bank.ofsf` soundfont (slot 5) and the Pocket's 32-voice hardware PCM
  mixer. General MIDI / GS programs, percussion (channel 10), volume, pan,
  expression, sustain, pitch bend, and reverb/chorus sends are honoured. If no
  bank is present, ScummVM falls back to its built-in OPL3 (AdLib) emulator.
- **CD audio:** Red Book tracks from `.cue`/`.bin` images (or the offline-decoded
  SE `track%d.wav` files) stream through the mixer at 44.1 kHz, resampled to
  48 kHz.
- **Digital speech:** standard SCUMM `.SOU` / `MONSTER.SOU` (DOTT, Atlantis) and
  the SE speech banks decode on-device.
- **Mixing:** ScummVM's software mixer feeds a 48 kHz stereo stream out through
  the SDK audio ring. To avoid heap corruption in malloc-heavy games, audio is
  pumped from the main thread (during engine sleeps and screen updates) rather
  than an interrupt, which trades ~120 ms of latency for gapless, crash-free
  playback.

**Not supported:** runtime **xWMA / WMA Pro** decoding (the SE music banks —
decode them offline as above), and **MT-32** emulation (MT-32-specific SysEx is
ignored; games fall back to GM/AdLib).

---

## Saves and config

- **Saves:** 9 slots, one per non-volatile flash region (slots 10–18, 256 KB
  each). Save/load from inside the game (START → F5, or game-specific menus).
  Each `.sav` carries a small header (`'SVMS'` magic, original ScummVM filename,
  length) followed by the payload; max ~262 KB per save.
- **Config:** the per-game `<game>.ini` (slot 9) holds ScummVM's settings.
  **Runtime persistence is disabled** on this port (writing config mid-session
  starved the launcher's UART pump), so volume/option changes you make in-game
  last for the session only. To change a setting permanently, edit the `.ini`
  on the SD card before booting.

---

## Limitations

- **Engines:** SCUMM **v0–v6** only. No v7/v8 (Curse of Monkey Island, The Dig,
  Full Throttle), no Humongous Entertainment, no Sierra SCI, no GrimE. Those
  sources are dropped from the build.
- **Resolution:** fixed **320×240, 8-bit indexed**. 320×200 games are
  letterboxed; anything larger (640×480 hi-res SCUMM) is clamped and unplayable.
  No aspect-ratio correction, no scalers, no overlay/in-game GUI theme.
- **Compressed resources:** zlib/inflate is stubbed — games that store data in
  compressed archives won't load.
- **SE remastered audio:** the *graphics* are classic-mode only; SE *music*
  must be decoded offline at build time (no runtime xWMA).
- **MT-32:** not emulated.
- **No on-device launcher / save dialog:** game choice happens in the Pocket's
  core menu (one instance per game); the original ScummVM save UI is disabled
  (it depends on theme assets this build doesn't ship) — use F5 from inside the
  game.
- **Config not persisted** across sessions (edit the `.ini` on the SD instead).
- **Memory:** 64 MB SDRAM with fixed per-subsystem pools; the tested LucasArts
  classics fit comfortably, but there is no graceful out-of-memory recovery.
- **2 GB** maximum per slot file.

---

## Building from source

Prerequisite: a RISC-V toolchain (`riscv64-elf-gcc`) **or** Docker. The build
also needs the vendored ScummVM submodule — the Makefile fetches it
automatically on first build (one-time, needs network), or run
`git submodule update --init src/scummvm/scummvm` yourself.

**With a host toolchain** — `make setup` (in the parent SDK) installs one on
Arch / Debian / Fedora / macOS / NixOS / MSYS2:

```sh
make build CORE=scummvm
make copy  CORE=scummvm          # POCKETDEV=/path to target a specific SD card
```

**With Docker (no host toolchain)** — prefix the port's build target with
`container-`; the wrapper runs `make` inside the `openfpgaos-firmware` image
(toolchain + musl pre-installed), building the image once on first use:

```sh
cd src/scummvm
make container-all               # compiles scummvm.elf and the SD tree in Docker
cd ../..
make copy CORE=scummvm           # rsync to the SD card (runs on the host)
```

The submodule fetch always runs on the host even for `container-*` targets (the
container has no network), so the first `make container-all` may pause to clone
ScummVM. `make copy` only moves files, so it never needs the toolchain — run it
on the host either way. Other useful targets: `make package CORE=scummvm`
(build a distributable ZIP) and `make release CORE=scummvm` (draft a GitHub
release, tag `scummvm-v<version>` from `core.json`).

The build expects the openfpgaOS SDK side-by-side; this tree pulls headers,
musl libc, and the `os.bin` kernel from there (update with `git pull`).

---

## Repository layout

```
src/scummvm/                    port build root
  main.cpp                      boot, slot discovery, config parse, engine launch
  config.h                      enabled engines (SCUMM v0–v6 only)
  backend/                      OSystem, mixer, MIDI synth, FS, save manager,
                                CUE/ISO archives, audio CD
  scummvm/                      vendored ScummVM tree (git submodule)
dist/scummvm/                   core layout copied to the SD by `make copy`
  Cores/ThinkElastic.ScummVM/   data.json (slots) + core/interact/input JSON + icon
  Assets/scummvm/
    common/                     shared bank.ofsf + per-game <game>_os.ini
    ThinkElastic.ScummVM/       per-game instance JSONs (and your data files)
scripts/                        packaging + ISO/CUE builders:
  build_monkey_iso.sh           MI1/MI2 SE → playable ISOs (with voices)
  lib_iso9660.py, lib_cuebin.py pure-Python ISO 9660 / CUE-BIN writers
  lib_xwb.py, build_mi1_*.py    XACT wave-bank tooling for SE audio
tools/
  docker/Dockerfile.firmware    containerized RISC-V toolchain image
  sdk-container.sh              `container-*` build wrapper
  wmapro_test/                  PC-side WMA Pro decoder test harness
```

---

## License

ScummVM is GPLv3; this port inherits it — see
`src/scummvm/scummvm/COPYING`. The openfpgaOS SDK (kernel, headers, libc) is
licensed separately; see its repository.

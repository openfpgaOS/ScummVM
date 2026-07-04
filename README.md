# ScummVM for Analogue Pocket (openfpgaOS)

A native port of [ScummVM](https://www.scummvm.org/) to the
[Analogue Pocket](https://www.analogue.co/pocket), running on
[openfpgaOS](https://github.com/openfpgaOS/openfpgaOS) — a RISC-V operating
system on the Pocket's Cyclone V FPGA. Per-engine ELFs (SCUMM / AGI / SCI) run
many classic point-and-click adventures; each game is a small **instance** (two
text/JSON config files plus the converted game data).

This README is the **game-packaging guide**: how to install the port, how to
add new games, how to convert and lay out game data, how music works, and the
tips/tricks that make a game actually run. (For hacking the C/C++ engine or the
SDK API, see the [openfpgaOS SDK](https://github.com/openfpgaOS/openfpgaOS).)

**Hardware:** VexiiRiscv rv32imafc @ 100 MHz · 64 MB SDRAM · 320×240 video ·
48 kHz stereo · 32-voice hardware PCM mixer · sample-based MIDI synth.

---

## Contents

- [What runs today](#what-runs-today)
- [Install](#install)
  - [Option A — prebuilt release](#option-a--prebuilt-release)
  - [Option B — build from source](#option-b--build-from-source)
- [How the port is organized](#how-the-port-is-organized)
  - [Data-slot map](#data-slot-map)
  - [SD-card layout](#sd-card-layout)
- [Add a new game (the whole workflow)](#add-a-new-game-the-whole-workflow)
- [Packaging a compilation disc](#packaging-a-compilation-disc-many-games-one-image)
- [Copy protection & startup gates](#copy-protection--startup-gates-sci)
- [`os.ini` reference](#osini-reference)
- [Instance JSON reference](#instance-json-reference)
- [Game data: formats & conversion](#game-data-formats--conversion)
- [Music: three paths](#music-three-paths)
- [Conversion & packaging scripts](#conversion--packaging-scripts)
- [Worked examples](#worked-examples)
- [Controls](#controls)
- [Tips, tricks & troubleshooting](#tips-tricks--troubleshooting)
- [Build & Makefile targets](#build--makefile-targets)
- [Project structure](#project-structure)
- [Updating & credits](#updating--credits)

---

## What runs today

The build ships **one ELF per engine**, all sharing a single compiled-once core:

| Engine | ELF | Covers | Status |
|--------|-----|--------|--------|
| SCUMM (v0–v6) | `scummvm_lucasarts.elf` | LucasArts: Maniac Mansion → *The Dig*-era v6 | working |
| SCI (SCI0/1/1.1; **not** SCI32) | `scummvm_sci.elf` | Sierra SCI: Leisure Suit Larry, Space Quest, Freddy Pharkas, … | working — extensively exercised by the LSL & Space Quest sets (LSL3 verified end-to-end: quiz, music, intro, save) |
| AGI | `scummvm_agi.elf` | Sierra AGI: King's Quest I–III, Space Quest I–II, … | builds clean; first packaged instance (SQ2) not yet runtime-verified |

SCUMM v7/v8 and Humongous Entertainment stay off; SCI32 (needs 16-bit RGB) is
excluded — so e.g. the Space Quest 6 disc will not run. Each game's `os.ini`
points `[os] ELF=` at the ELF for its engine, so adding an AGI or SCI game is
**config only — no rebuild**. The one exception is a game that needs a new
copy-protection skip patch (those live in the SCI ELF — see
[Copy protection & startup gates](#copy-protection--startup-gates-sci)). The
backend glue (video, audio, MIDI synth, CD audio, saves, input) lives in
`src/scummvm/backend/`; the engine is vendored under `src/scummvm/scummvm/`;
the per-engine build is `src/scummvm/Makefile` (`ENGINES = scumm agi sci`).

### Packaged games

Instances live in `dist/scummvm/Assets/scummvm/` (`common/<base>_os.ini` +
`ThinkElastic.ScummVM/<Name>.json`). Games on a compilation disc share **one**
slot-4 image and are disambiguated by `subdir=` (see
[Packaging a compilation disc](#packaging-a-compilation-disc-many-games-one-image)).

**LucasArts (SCUMM, `scummvm_lucasarts.elf`)** — one image per game:

| Game | gameid | variant | Music |
|------|--------|---------|-------|
| The Secret of Monkey Island | `monkey` | `SE` | CD audio (WAV-in-ISO) + SE voices |
| Monkey Island 2: LeChuck's Revenge | `monkey2` | `SE` | iMUSE MIDI + SE voices |
| Indiana Jones and the Fate of Atlantis | `atlantis` | — | iMUSE MIDI |
| Loom | `loom` | `VGA` | CD audio (CDDA.SOU) |
| Day of the Tentacle | `tentacle` | — | iMUSE MIDI |
| Sam & Max Hit the Road | `samnmax` | — | iMUSE MIDI |

**Leisure Suit Larry "Greatest Hits" (SCI, `scummvm_sci.elf`)** — shared
`lsl-gham.iso`, one `subdir` per game. Startup copy-protection / age gates are
patched out where present (✱):

| Game | gameid | subdir | variant | Gate removed |
|------|--------|--------|---------|--------------|
| LSL1 (VGA remake) | `lsl1sci` | `LSL1VGA` | `SCI` | ✱ age quiz + manual lookup |
| LSL2 | `lsl2` | `LSL2` | — | ✱ "little black book" phone number |
| LSL3 | `lsl3` | `LSL3` | — | ✱ age trivia quiz |
| LSL5 | `lsl5` | `LSL5` | — | ✱ mid-game ticket code |
| LSL6 | `lsl6` | `LSL6` | — | none |
| Crazy Nick's: Larry's Casino | `cnick-lsl` | `ARCADES` | — | none |
| Freddy Pharkas (demo) | `freddypharkas` | `FPFPDEMO` | `CD Demo` | none |

**Space Quest Collection (SCI + AGI)** — shared `sq-coll.iso`, one `subdir` per
game. None of them have copy protection (Sierra dropped it by this era):

| Game | gameid | engine | subdir | variant |
|------|--------|--------|--------|---------|
| Space Quest 1 (VGA) | `sq1sci` | sci | `SQ1VGA` | `SCI` |
| Space Quest 2 | `sq2` | **agi** | `SQ2` | — |
| Space Quest 3 | `sq3` | sci | `SQ3` | — |
| Space Quest 4 (CD) | `sq4` | sci | `SQ4` | — |
| Space Quest 5 | `sq5` | sci | `SQ5` | — |

You supply the game data — the port ships configs, not copyrighted assets.

You supply the game data — the port ships configs, not copyrighted assets.

---

## Install

You need an Analogue Pocket with an SD card, and **legally-obtained game data**
(original discs, or your GOG/Steam purchases). Floppy/CD dumps and the GOG
"ScummVM-ready" file sets both work.

### Option A — prebuilt release

1. Download the core ZIP from the project's GitHub Releases.
2. Extract it to the **root of your SD card** (it writes `Cores/`, `Assets/`,
   `Platforms/`).
3. Copy each game's data file into
   `Assets/scummvm/common/` (e.g. `monkey1.iso`, `loom.iso`) — see
   [SD-card layout](#sd-card-layout).
4. Boot the Pocket → open the ScummVM core → pick a game from the list.

### Option B — build from source

```bash
git clone https://github.com/openfpgaOS/ScummVM.git
cd ScummVM
make setup                         # install the RISC-V toolchain (one time)
cd src/scummvm
make                               # build all engine ELFs + assemble build/pocket/scummvm/
make copy                          # copy the assembled core to a mounted SD card
# or:  make package                # zip it into releases/pocket/
```

**Toolchain.** `make setup` detects your OS and offers to install
`riscv64-elf-gcc` (the Makefile accepts either the `riscv64-unknown-elf-` or
`riscv64-elf-` prefix):

| OS | Package |
|----|---------|
| Arch / Manjaro | `riscv64-elf-gcc` (pacman) |
| Ubuntu / Debian | `gcc-riscv64-unknown-elf` (apt) |
| Fedora | `gcc-riscv64-linux-gnu` (dnf) |
| openSUSE | `cross-riscv64-gcc14` (zypper) |
| macOS | `riscv64-elf-gcc` (brew) |
| Windows (MSYS2) | `mingw-w64-ucrt-x86_64-riscv64-unknown-elf-gcc` |

`make` in `src/scummvm/` compiles the shared core once into
`build-multi/libcore.a`, links one `scummvm_<engine>.elf` per engine, then
`platforms/pocket/image.sh` assembles the deployable SD tree under
`build/pocket/scummvm/` (runtime bitstream + `os.bin` + the per-engine ELFs +
`bank.ofsf` + every config from `dist/scummvm/`). `make copy` mirrors that tree
onto the card. Build a subset with `make ENGINES=scumm`.

> **Rebuild caveat:** the Makefile has no header dependencies. If you edit
> `config.h` or the engine's detection tables, delete the affected `.o`
> (or `make clean`) — a plain `make` won't notice the header changed.

---

## How the port is organized

There is one ELF per engine (`scummvm_lucasarts.elf`, `scummvm_agi.elf`,
`scummvm_sci.elf`), all sharing one compiled-once core. Every game is an
**instance**:

- a per-game **`<game>_os.ini`** (slot 2) that tells the launcher which game,
  version, platform, language and data file to load, and
- a per-game **instance JSON** that binds the Pocket's data slots to filenames.

At boot the launcher reads `<game>_os.ini`, fills ScummVM's config, and **skips
MD5 detection** (`openfpga_skip_detection=true`) — over the Pocket bridge the
normal "hash 1 MB of every file" scan is too slow. Instead the game version is
resolved **directly from `gameid` + `variant`** against ScummVM's variant
tables. This is why `variant` *must match your actual data* (see
[Tips](#tips-tricks--troubleshooting)).

### Data-slot map

The ScummVM core defines this slot schema (`dist/scummvm/Cores/.../data.json`):

| Slot | Name | File | Notes |
|------|------|------|-------|
| 1 | OS Binary | `os.bin` | openfpgaOS kernel (from `runtime/`) |
| 2 | OS Config | `<game>_os.ini` | the per-game config below |
| 3 | Application | `scummvm_<engine>.elf` | the engine ELF (`scummvm_lucasarts.elf` for SCUMM) — must match `[os] ELF=` |
| 4 | Game Data | `<game>.iso` / `.zip` / `.cue` | your converted game |
| 5 | Sound Bank | `bank.ofsf` | GM SoundFont for the MIDI synth |
| 7 | Disc Image | `<game>.bin` | the raw `.bin` when slot 4 is a `.cue` |
| 9 | Settings | `<game>.ini` | ScummVM's own ini, written at runtime |
| 10–18 | Save 0–8 | `<game>_N.sav` | 9 non-volatile save slots (256 KB each) |

Slots 1 and 5 are shared by every game. **All** engine ELFs are deployed to
`common/`, and each instance binds slot 3 (and `[os] ELF=`) to the one for its
engine. You provide slots 2, 4 (and 7 for cue/bin). Slots 9–18 are created on
the device at runtime.

### SD-card layout

On the card (Analogue "openFPGA" / APF layout):

```
/Cores/ThinkElastic.ScummVM/          core.json, data.json, audio.json, video.json,
                                       input.json, interact.json, bitstream, icon.bin
/Assets/scummvm/
    common/                            os.bin, bank.ofsf,
                                       scummvm_lucasarts.elf / scummvm_agi.elf / scummvm_sci.elf,
                                       <game>_os.ini   (one per game),
                                       <game>.iso / .zip / .cue / .bin  (your data)
    ThinkElastic.ScummVM/              <Display Name>.json   (one per game — the picker list)
```

The Pocket shows one menu entry per **instance JSON** in
`Assets/scummvm/ThinkElastic.ScummVM/`. Game data and `*_os.ini` files live
together in `Assets/scummvm/common/`. Big data files aren't tracked in the
repo — drop them straight into `build/pocket/scummvm/Assets/scummvm/common/`
before `make copy`, or onto the card's `common/` directly.

---

## Add a new game (the whole workflow)

Say you want to add **Sam & Max Hit the Road** (`gameid=samnmax`). All paths are
in the source tree (`dist/...`); they get copied to the card by `make copy`.

**1. Get the data and identify it.** Find the ScummVM `gameid`, version and
`variant` for your copy. The authoritative lists are in the vendored engine:

- `src/scummvm/scummvm/engines/scumm/detection_tables.h` — `gameVariantsTable`
  (gameid → variant → engine version, music device types, feature flags) and
  `gameFilenamesTable` (gameid/variant → on-disc filename pattern).
- `src/scummvm/scummvm/engines/scumm/scumm-md5.h` — every known dump, its
  `gameid`, `variant`, and index-file size (handy for telling EGA from VGA).

**2. Convert the data to a slot-4 file.** Most games: a **cooked ISO9660** image
or a **ZIP** of the game files. CD-audio games may need a `.cue/.bin` pair or an
embedded `CDDA.SOU` — see [Game data](#game-data-formats--conversion) and
[Music](#music-three-paths). Quick ISO from loose files:

```bash
python3 scripts/convert/lib_iso9660.py samnmax.iso SAMNMAX  path/to/*.LFL path/to/MONSTER.SOU ...
```

**3. Write `dist/scummvm/Assets/scummvm/common/samnmax_os.ini`:**

```ini
[os]
ELF=scummvm_lucasarts.elf
ARGS=samnmax

[scummvm]
gameid=samnmax
engineid=scumm
description=Sam & Max Hit the Road
platform=pc
language=en
music=openfpga
data_file=samnmax.iso
```

`ELF=` must be the ELF for the game's engine: **`scummvm_lucasarts.elf`** for
SCUMM (this example), **`scummvm_agi.elf`** for AGI, **`scummvm_sci.elf`** for
SCI. All three are already on the card; you don't rebuild to add a non-SCUMM
game — just name the right ELF here and in slot 3 below.

**4. Write the instance `dist/scummvm/Assets/scummvm/ThinkElastic.ScummVM/Sam and Max.json`:**

```json
{
    "instance": {
        "magic": "APF_VER_1",
        "variant_select": { "id": 666, "select": false },
        "data_slots": [
            { "id": 1,  "filename": "os.bin" },
            { "id": 2,  "filename": "samnmax_os.ini" },
            { "id": 3,  "filename": "scummvm_lucasarts.elf" },
            { "id": 4,  "filename": "samnmax.iso" },
            { "id": 5,  "filename": "bank.ofsf" },
            { "id": 9,  "filename": "samnmax.ini" },
            { "id": 10, "filename": "samnmax_0.sav" },
            { "id": 11, "filename": "samnmax_1.sav" },
            { "id": 12, "filename": "samnmax_2.sav" }
        ]
    }
}
```

(Add save slots 10–18 as desired; they're created on first save.)

**5. Set up music** (slot 5 `bank.ofsf` for MIDI games is already shared; CD
games need extra steps — see [Music](#music-three-paths)).

**6. Deploy.** Put `samnmax.iso` in the card's `Assets/scummvm/common/`, then:

```bash
cd src/scummvm && make copy        # rebuilds the tree and copies to the card
```

`make copy` writes to the **mounted SD card** and prints `Deployed!` on success.
If no card is mounted it fails at the copy step (`copy.sh` → `Error 1`) while the
build still succeeds — so if a rebuilt game behaves identically on the device,
**check the card was mounted and that `Deployed!` printed** before re-debugging.

Boot → ScummVM core → **Sam & Max Hit the Road** appears in the list.

---

## Packaging a compilation disc (many games, one image)

Multi-game discs (the LSL "Greatest Hits", the Space Quest Collection, …) hold
every game in one ISO, each in its own top-level folder. Rather than carve out an
image per game, **all the instances share that one slot-4 image and set `subdir=`
to the game's folder** inside it — the card carries the ISO once.

**1. Inspect the disc** — list the game folders and identify each:

```bash
7z l My_Collection.iso | grep -i RESOURCE.MAP   # SCI/SCUMM game folders
7z l My_Collection.iso | grep -i LOGDIR         # AGI game folders
```

A folder with `RESOURCE.MAP` (+ `RESOURCE.000/001…`) is SCI; one with `LOGDIR` +
`VOL.*` is AGI. Match each to its ScummVM `gameid`/`variant`/engine in the
vendored `detection_tables.h` (`engines/sci/…`, `engines/agi/…`,
`engines/scumm/…`). Note the per-game **engine** — a single disc can mix engines
(the Space Quest Collection has SQ2 on AGI and the rest on SCI).

**2. Give the image a short name** and reuse it as `data_file` for every game:

```bash
mv My_Collection.iso mycoll.iso        # data_file=mycoll.iso
```

**3. Write one `os.ini` + one JSON per game** — same `data_file=mycoll.iso`,
different `subdir=`, and the right `[os] ELF=` (and slot-3 ELF in the JSON) for
that game's engine:

```ini
# game1_os.ini  — an SCI game in folder GAME1 on the shared disc
[os]
ELF=scummvm_sci.elf
ARGS=game1id

[scummvm]
gameid=game1id
engineid=sci
description=Game One
platform=pc
language=en
music=openfpga
variant=VGA
data_file=mycoll.iso
subdir=GAME1
```

For a whole set, a tiny generator beats hand-editing — loop over
`(base, gameid, engine, description, subdir, variant)` and emit the `_os.ini` +
`.json` pair for each (the LSL/SQ sets were produced this way). `subdir` is
honored by the port's ISO/zip mount (`backend/openfpga_cue_archive.cpp` /
`openfpga_fs.cpp`): the engine sees only that folder's files.

**4. Deploy once** — copy `mycoll.iso` into the card's `Assets/scummvm/common/`
and `make copy`. Every game in the collection then appears in the list, all
reading the one image.

> **Skip-detection (read this for `variant`).** The launcher forces
> `openfpga_skip_detection=true`, so the engine does **not** MD5-scan the disc; it
> builds the game descriptor from your `gameid` + `platform` + `language` +
> `variant`. Get those right — especially `variant`/"extra" to pick **EGA vs VGA**
> or **floppy vs CD** — or the engine selects the wrong descriptor. The recovery
> path is the per-engine `openFPGAFindGameDesc()` + the 4-arg `createInstance`
> override in `engines/<engine>/metaengine.cpp`.

---

## Copy protection & startup gates (SCI)

Many Sierra SCI games open with a copy-protection or age gate — type a word from
the manual, a code wheel, the LSL "prove you're an adult" quiz. The launcher sets
`copy_protection=false`, which auto-passes the *anti-piracy* checks the engine
recognises (and removes most SCUMM ones outright). But two cases still block:
some SCI screens render even though the check is bypassed, and the LSL **age
quizzes** aren't classed as copy protection at all, so they hard-gate until
patched.

These skips are real **SCI script patches** compiled into `scummvm_sci.elf`
(`engines/sci/engine/script_patches.cpp`) — the same mechanism ScummVM uses
upstream. Already shipped:

| Game | gameid | Patch (in `script_patches.cpp`) |
|------|--------|---------------------------------|
| LSL1 VGA | `lsl1sci` | `larry1…SkipAgeGate` — room 720 |
| LSL2 | `lsl2` | `larry2…SkipCopyProtection` — room 10 |
| LSL3 | `lsl3` | `larry3…SkipAgeQuiz` — room 140 |
| LSL5 | `lsl5` | `larry5…SkipDestinationCode` — room 258 |

The Space Quest games need none.

**Adding a skip for a new game:**

1. **Decompile to find the gate.** Decode the game's `RESOURCE.*` (port the
   decompressor from `engines/sci/resource/decompressor.cpp` — it covers
   SCI0/1/1.1 LZW/Huffman/DCL) and scan the TEXT/MESSAGE resources for the gate
   prompt; locate the room/script that implements it.
2. **Write the patch.** Add a `SciScriptPatcherEntry` to that game's
   `larryN…`/`sqN…` table: a `SIG_MAGICDWORD` signature that occurs **exactly
   once** in the script + a `PATCH_…` replacement that redirects the gate to its
   own "passed" exit or rewrites the check to always pass. Mirror an existing one
   — `larry2SignatureSkipCopyProtection` redirects a startup `newRoom`;
   `larry1SignatureSkipAgeGate` rewrites `changeState(0)` to the success state.
3. **Keep the intro.** Land on the gate's *success* destination, never past a
   logo/title/story sequence. If the intro plays inside or right after the gate
   room, auto-pass the gate **in place** instead of jumping to gameplay (verify
   where the intro sits relative to the gate, as the LSL1/LSL2 patches do).
4. **Rebuild** `scummvm_sci.elf` (`make build`) — script patches are baked into
   the ELF, so this is the one game-add that *does* require a rebuild.

This code step is the only non-config part of packaging; games with no gate (or
SCUMM, where `copy_protection=false` strips it) never need it.

---

## `os.ini` reference

Slot-2 file, INI format, two sections. The `[scummvm]` keys are read by
`main.cpp::loadGameConfigFromOS()`.

| Key | Required | Meaning |
|-----|----------|---------|
| `[os] ELF` | yes | the engine ELF: `scummvm_lucasarts.elf` (SCUMM), `scummvm_agi.elf`, or `scummvm_sci.elf` |
| `[os] ARGS` | yes | the gameid (instance identifier) |
| `gameid` | yes | ScummVM game id (`monkey`, `loom`, `atlantis`, `tentacle`, `samnmax`, …) |
| `engineid` | — | engine; defaults to `scumm` |
| `description` | — | name shown in the boot status log |
| `platform` | — | `pc` (default), `amiga`, `mac`, `fmtowns`, `segacd`, … |
| `language` | — | ISO 639-1 (`en`, `de`, `fr`, `it`, `es`, …) |
| `music` | — | `openfpga` routes iMUSE/MIDI through the SDK synth (default) |
| `variant` | — | the ScummVM "extra"/variant (`EGA`, `VGA`, `SE`, `FM-TOWNS`, `Floppy`, `CD`, `Steam`, …). Empty = first table match. **Must match your data.** |
| `data_file` | — | slot-4 filename: `<game>.iso`, `.zip`, or `.cue` |
| `data_type` | — | `""`/`zip` (normal) or `cue` (a BIN/CUE disc inside an outer zip) |
| `cue_file` | — | `.cue` member/path when `data_file` is an outer zip |
| `subdir` | — | per-game subdirectory on a multi-game compilation disc (e.g. a Police Quest collection) |
| `cd_track_offset` | — | integer added to every CD track number before lookup (re-aligns compilation discs vs the standalone CD) |
| `voices` | — | `true` enables LucasArts **Special Edition** speech (`use_remastered_audio`); needs `variant=SE` + the SE pak & speech bank on the disc |

The launcher always forces a few ScummVM settings regardless of the ini:
`copy_protection=false`, `multi_midi=false`, `subtitles=true`,
`use_remastered_audio=<voices>`, `openfpga_skip_detection=true`, and default
music/SFX/speech volumes.

---

## Instance JSON reference

Slot-0 file, one per game, in `Assets/scummvm/ThinkElastic.ScummVM/`. Its
filename (minus `.json`) is the menu label. It only binds slots to filenames:

```json
{
    "instance": {
        "magic": "APF_VER_1",
        "variant_select": { "id": 666, "select": false },
        "data_slots": [ { "id": <slot>, "filename": "<name>" }, ... ]
    }
}
```

Use the [slot map](#data-slot-map) above. Minimum viable game = slots 1, 2, 3, 4
(+5 for MIDI). Add 7 for cue/bin, 9 for settings, 10–18 for saves.

---

## Game data: formats & conversion

Slot 4 accepts three container types. **Cooked ISO is the default and most
reliable.**

### ISO9660 (`.iso`) — recommended

A standard data-track filesystem image holding the game's files. **Cooked
(2048-byte/sector)** images are preferred — both the kernel mount and the
app-side reader expect them. Raw **MODE1/2352** images also mount, but any CD
*audio* appended after the data track is **not reachable** without a cue (see
[Music](#music-three-paths)).

Make one from loose game files with the bundled pure-Python builder (no
`mkisofs`/`xorriso` needed):

```bash
python3 scripts/convert/lib_iso9660.py  <out.iso>  <VOLID>  <file1> [file2 ...]
```

It emits ISO9660 level 1 (8.3 upper-case names, untruncated where needed,
large-file aware). Or use system tools if you have them:
`xorriso -as mkisofs -iso-level 1 -V GAME -o game.iso gamedir/`.

The engine looks files up by **bare basename** through SearchMan, and the
backend adds a flat search pass, so files in subdirectories of the ISO are still
found. Keep critical names un-truncated (e.g. `SPEECH.INFO`, `CDDA.SOU`).

### ZIP (`.zip`)

A zip of the game files; set `data_type=zip` (or leave default — `.zip` is
auto-detected). Members are read directly. A zip can also carry a raw disc image:
set `data_type=cue` and `cue_file=<member>.cue`.
Uncompressed zips (Compression Level 0, aka "Store") are recommended,
DEFLATE/DEFLATE64 will greatly increase loading times.

### CUE / BIN (`.cue` + `.bin`)

For CD games whose music is real Red Book audio tracks. Put the `.cue` in slot 4
and the `.bin` in slot 7. The cue parser recognizes
`FILE` / `TRACK <n> <TYPE>` / `INDEX <i> mm:ss:ff`, with the data track as
`MODE1/2352` and audio tracks as `AUDIO`. The backend plays audio tracks through
the hardware mixer. `scripts/convert/lib_cuebin.py` assembles a single-file
`MODE1/2352 + CDDA` `.bin/.cue` for the port.

---

## Music: three paths

Pick the path by **how that specific game version produces music** — this is the
single most common source of "game runs but no music." Check the
`gameVariantsTable` row for your gameid+variant: the music-device-types field
(`MDT_*`) and feature flags tell you which path applies.

### 1. MIDI through the synth (most floppy & early-CD games)

Games whose score is **iMUSE / Standard MIDI** (AdLib/MT-32/GM): Monkey 2,
Fate of Atlantis, Day of the Tentacle, Sam & Max, Indy 3, Loom **EGA**, etc.
Their variant row has `MDT_ADLIB|MDT_MIDI` (not `MDT_NONE`).

Setup: `music=openfpga` and ship `bank.ofsf` in slot 5 (shared — already
deployed). The engine renders MIDI through the GM synth. Nothing else to do.

### 2. CD audio via `CDDA.SOU` (e.g. Loom CD / VGA)

Some CD versions have **no sequenced music at all** — their variant row is
`MDT_NONE | GF_AUDIOTRACKS | GUIO_NOMIDI`, so `music=openfpga` does nothing.
ScummVM reads such a game's audio from a single **`CDDA.SOU`** file
(`engines/scumm/cdda.cpp`) and plays it through the mixer (the same working PCM
path the port uses for MI1 music).

Setup: put a `CDDA.SOU` at the **root of the game's ISO**. If you only have a
raw full-disc image of the CD (data track + appended audio sectors), transcode
it with:

```bash
python3 scripts/convert/build_loom_iso.py --raw loom.iso --data loom_2048.iso
# -> build/loom/loom.iso  (data files + CDDA.SOU)
```

`CDDA.SOU` format: an 800-byte header then 1177-byte blocks (1 shift byte +
588 stereo `int8` pairs decoded as `sample << shift`); **one 2352-byte CD audio
sector == one block**.

### 3. CD audio via real tracks (`.cue/.bin`, compilation discs)

Games that address music by **CD track number** (Monkey Island CD, the
*Monkey Island Madness* compilation). Provide a `.cue` (slot 4) + `.bin`
(slot 7). For compilation discs whose track numbering differs from the
standalone release, use `cd_track_offset` (renumber) and `subdir` (the game's
folder within the disc).

### Special-Edition voices (MI1/MI2 SE)

`variant=SE` + `voices=true` makes the engine inject the remastered **speech**
over the classic scripts. The SE *music* banks are xWMA (no runtime decoder), so
music comes from the classic path instead — for MI1 that's offline-decoded CD
WAVs, for MI2 it's the iMUSE MIDI synth. Build these with
`scripts/convert/build_monkey_iso.sh` (below).

---

## Conversion & packaging scripts

All in **`scripts/convert/`** (see its [README](scripts/convert/README.md)). The
`lib_*` files are importable helpers; the `build_*` files are end-to-end
packagers. Pure-Python (numpy for audio) unless noted — no mkisofs/bsdtar.

| Script | Purpose |
|--------|---------|
| `lib_iso9660.py` | Read **and** build cooked ISO9660 level-1 images. `python3 lib_iso9660.py out.iso VOLID files…` · `--extract in.iso dir` |
| `lib_cdda.py` | Transcode raw 16-bit stereo PCM ⇄ ScummVM `CDDA.SOU` (Loom CD audio) |
| `lib_cuebin.py` | Assemble a single-file `MODE1/2352 + CDDA` `.bin/.cue` the port's cue parser accepts |
| `build_loom_iso.py` | Loom CD: transcode the raw image's appended CD audio → `CDDA.SOU`, repack the data iso with it. `--raw loom.iso --data loom_2048.iso` |
| `build_monkey_iso.sh` | MI1/MI2 **Special Edition** from Steam data: SE voices + music, slim pak, into a slot-4 ISO. `build_monkey_iso.sh [monkey1\|monkey2\|all]` |
| `build_mi1_music.py` | Decode MI1's SE music bank → port-playable CD-audio WAVs (ffmpeg) |
| `build_mi1_speech.py` | Repack/shrink MI1's SE `Speech.xwb` (MS-ADPCM or downsampled PCM) |
| `build_min_pak.py` | Build a minimal KAPL/LPAK `.pak` with only the classic data files |
| `lib_lpak.py` | Extract LucasArts SE `KAPL`/LPAK `.pak` archives |
| `lib_xwb.py` | Extract XACT3 `.xwb` wave banks (SE speech/music) |
| `lib_mi1_trackmap.py` | Authoritative MI1 CD-audio track map (+1 Red-Book offset) |

Deploy / distribution helpers:

| Script | Purpose |
|--------|---------|
| `setup.sh` | Detect OS, install the RISC-V toolchain (`make setup`) |
| `copy.sh` / `sdcard.sh` | Find/mount the Pocket SD card and copy the built tree (`make copy`) |
| `package.sh` | Zip a built core into `releases/<target>/` (`make package`) |
| `release.sh` | Publish a packaged core to GitHub Releases |
| `debug.sh` | Push the ELF over UART (PHDP), reset the core, stream the console |
| `controller.sh` | Map a host keyboard to the dock for desk testing |

---

## Worked examples

**Loom (CD/VGA) — CD audio via CDDA.SOU.** Keep `variant=VGA` (it's SCUMM v4 —
the magic-id check rejects the EGA reader). Transcode the embedded audio and
repack:

```bash
python3 scripts/convert/build_loom_iso.py --raw loom.iso --data loom_2048.iso
cp build/loom/loom.iso  build/pocket/scummvm/Assets/scummvm/common/loom.iso
cd src/scummvm && make copy
```

**Monkey Island 1 & 2 (Special Edition) — voices + music from Steam.**

```bash
scripts/convert/build_monkey_iso.sh all \
    --mi1-dir "/path/to/The Secret of Monkey Island Special Edition" \
    --mi2-dir "/path/to/Monkey2"
# -> build/monkey/monkey1.iso, build/monkey/monkey2.iso
```

The matching `monkey1_os.ini` / `monkey2_os.ini` already set `variant=SE` and
`voices=true`.

---

## Controls

The Pocket gamepad drives the mouse-and-keyboard adventure UI:

| Input | Action |
|-------|--------|
| D-pad / left stick | Move the mouse cursor |
| Hold L1/L2 | Slow (precise) cursor |
| Hold R1/R2 | Fast cursor |
| A | Left mouse button (walk / use / select) |
| B | Right mouse button (examine / default verb) |
| X | Enter / confirm |
| Y | Space (pause) |
| START | F5 (ScummVM menu — save / load / options) |
| L3 | Tab (inventory / verb toggle in some games) |
| R3 | `.` (skip current line of dialogue) |
| Hold SELECT + Up/Down | Master volume |
| Hold SELECT + Left/Right | Music volume |
| Tap SELECT | Toggle on-screen numeric **keypad** (type codes with no keyboard) |

In keypad mode the face/d-pad buttons map to digits `1`–`0` and START submits
(Enter). Copy-protection screens are auto-bypassed, but the keypad is there for
any game that still asks for a number.

---

## Tips, tricks & troubleshooting

**"The magic id doesn't match (0x….)!" / FATAL on boot.** The `variant` selects
the wrong engine version for your data, so the wrong index-file reader runs.
Match `variant` to the dump: e.g. Loom **EGA** floppy (v3) vs Loom **VGA** CD
(v4) have different index formats. Cross-check the index-file size in
`scumm-md5.h` (Loom VGA's `000.LFL` is 8307 bytes; EGA's is 5748).

**Game runs but no music.** Confirm which music path the *version* uses
(§ [Music](#music-three-paths)). A CD/talkie version flagged `MDT_NONE` has **no
MIDI** — `music=openfpga` can't help; you need `CDDA.SOU` or a `.cue/.bin`.
A MIDI version with no music usually means `bank.ofsf` (slot 5) is missing.

**Wrong filename layout / "no game data".** The variant also picks the on-disc
filename pattern (`gameFilenamesTable`). EGA uses `NN.LFL`, CD/VGA uses
`NNN.LFL`. If the engine can't find resources, the variant and the data don't
agree.

**Compilation discs.** Use `subdir` to point at the game's folder inside a
multi-game ISO, and `cd_track_offset` to realign CD track numbers vs the
standalone release.

**Copy protection.** Auto-disabled (`copy_protection=false`); MI2's Dial-A-Pirate,
Indy's passport quiz, and DOTT's manual word are skipped. If a screen still
wants input, tap SELECT for the keypad.

**ISO sector size.** Prefer **cooked 2048**. A raw 2352 full-disc image mounts
for data, but its appended CD-audio tracks are invisible to the port without a
cue — transcode to `CDDA.SOU` (Loom) or ship `.cue/.bin` instead.

**Save files.** 9 slots (10–18), 256 KB each; ScummVM's own settings live in
slot 9. All are created on the device on first use.

**Engine edits don't rebuild.** No header deps in the Makefile — after editing
`config.h` or detection tables, delete the stale `.o` (or `make clean`).

**Desktop smoke test.** Convert your data, then sanity-check the same game in
desktop ScummVM before chasing port-specific issues — it isolates "bad data /
wrong variant" from "port bug."

---

## Build & Makefile targets

From `src/scummvm/`:

| Command | What it does |
|---------|--------------|
| `make` | Build every engine ELF and assemble `build/pocket/scummvm/` |
| `make copy` | Build, then copy the tree to a mounted SD card |
| `make package` | Build, then zip into `releases/pocket/` |
| `make debug` | Build, push the SCUMM ELF over UART (PHDP), stream the console |
| `make ENGINES=scumm` | Build a subset of engines (default: `scumm agi sci`) |
| `make clean` | Remove `build-multi/` and the assembled image |

The build compiles the engine-agnostic core once into
`build-multi/libcore.a`, then links one `scummvm_<engine>.elf` per engine in
`ENGINES`. From the repo root: `make setup` (toolchain), `make tools` (PHDP host
tools).

---

## Project structure

```
ScummVM/
├── README.md                         <- this guide
├── Makefile                          <- top-level: setup, tools
├── runtime/                          <- FPGA bitstream, os.bin, loader, bank.ofsf
├── scripts/                          <- deploy/SDK scripts (setup, copy, package, release, debug)
│   └── convert/                      <- game-data conversion toolkit (lib_*, build_*) + its README
├── dist/scummvm/                     <- the ScummVM core's SD-card configs (source of truth)
│   ├── Cores/ThinkElastic.ScummVM/   <- core.json, data.json, audio/video/input/interact.json
│   └── Assets/scummvm/
│       ├── common/                   <- <game>_os.ini files (+ data files on the card)
│       └── ThinkElastic.ScummVM/     <- <Display Name>.json instance/picker files
├── dist/sdk/                         <- the base openfpgaOS core (shared runtime)
├── build/                            <- assembled SD trees (make output; untracked)
└── src/scummvm/
    ├── Makefile                      <- per-engine build (core once -> scummvm_<engine>.elf)
    ├── build-multi/                  <- build objects + libcore.a + the ELFs (untracked)
    ├── main.cpp                      <- launcher: reads os.ini, configures + runs the engine
    ├── config.h                      <- base config (engines are selected per-ELF by the Makefile)
    ├── backend/                      <- openfpgaOS OSystem: video, mixer, MIDI, CD audio, FS, saves, splash
    └── scummvm/                      <- vendored ScummVM engine sources
```

**What you edit per game:** `dist/scummvm/Assets/scummvm/common/<game>_os.ini`
and `dist/scummvm/Assets/scummvm/ThinkElastic.ScummVM/<Name>.json`, plus the
converted data file you drop into `common/`. Everything else (runtime, core
JSON, the ELF) is shared and built for you.

---

## Updating & credits

```bash
git pull
cd src/scummvm && make clean && make
```

Built on [ScummVM](https://www.scummvm.org/) (GPLv3) and the
[openfpgaOS SDK](https://github.com/openfpgaOS/openfpgaOS). Game data is your
own; this project ships only the port and its configuration. Respect the
licenses of the games you convert.

# ScummVM for Analogue Pocket (openfpgaOS)

A native port of [ScummVM](https://www.scummvm.org/) to the Analogue Pocket running [openfpgaOS](https://github.com/ThinkElastic/openfpgaOS-SDK). Plays classic point-and-click adventures from the LucasArts SCUMM-engine era and beyond, with audio (MIDI + CD-style tracks), 320×240 framebuffer graphics, save slots in nonvolatile storage, and per-game instances visible directly in the Pocket launcher.

## Games supported

| Game | Source | Music | Speech | Notes |
|---|---|---|---|---|
| Monkey Island 1 (CD) | ISO | ✓ MIDI (AdLib/MT-32) | — | Tested |
| Monkey Island 2 (CD) | ISO | ✓ MIDI | — | Tested |
| Day of the Tentacle | ISO | ✓ MIDI | ✓ digital | Tested |
| Fate of Atlantis | ISO | ✓ MIDI | ✓ digital | Tested |

Engine coverage today: **SCUMM v4–v6**. v3 and v7 may work but are untested. Other ScummVM engines (Sierra AGI/SCI, Beneath a Steel Sky, etc.) are not enabled in this build but the upstream code is present and would build with small Makefile changes.

## Quick start

1. Build the core:

    ```sh
    make build CORE=scummvm
    make copy  CORE=scummvm
    ```

    `make copy` writes the core layout (binary + `data.json` + per-instance JSON + asset slots) to `$POCKETDEV` (or the path you set via `POCKETDEV=…`).

2. Drop your `.iso` or `.zip` game files into the matching slot path on the SD card (see *Adding a game* below).

3. Insert the SD, boot the Pocket, navigate to **Cores → ThinkElastic.ScummVM**. Each game you've installed shows up as a separate instance.

4. The first instance you select prompts the launcher to bind the data, save, and config slots. From that point on, that game's saves and settings live in those non-volatile slots.

## Adding a game

Each game on the Pocket is one **instance**: an instance JSON tells the launcher which data file to bind to slot 3 (the game data), which `.cfg` to bind to slot 5, which slot 9 holds the user's `.ini`, and which slots 10–19 hold save files. Ship as many instances as you want.

Two delivery modes are supported.

### Mode A — ISO (recommended for CD games)

The Pocket-side OS mounts standard ISO 9660 filesystems via `of_iso_mount`, and our backend points ScummVM's filesystem at `/cd` so the engine sees the disc as a normal directory.

1. **Get an ISO of your CD.** `dd if=/dev/sr0 of=mygame.iso bs=2048`, or any standard ripping tool. Verify on desktop ScummVM first.

2. **Write a `game.cfg`** (plain text, key=value):

    ```ini
    gameid=monkey
    engineid=scumm
    description=The Secret of Monkey Island
    platform=pc
    language=en
    music=openfpga
    ```

    `gameid` is the ScummVM canonical id (`monkey`, `monkey2`, `tentacle`, `atlantis`, etc.). `music=openfpga` selects our hardware-PCM MIDI driver.

3. **Write an instance JSON** in `dist/scummvm/Assets/scummvm/ThinkElastic.ScummVM/`:

    ```json
    {
      "instance": {
        "magic": "APF_VER_1",
        "variant_select": { "id": 666, "select": false },
        "data_slots": [
          { "id": 1,  "filename": "os.bin"        },
          { "id": 2,  "filename": "scummvm.elf"   },
          { "id": 3,  "filename": "monkey1.iso"   },
          { "id": 4,  "filename": "bank.ofsf"     },
          { "id": 5,  "filename": "monkey1.cfg"   },
          { "id": 9,  "filename": "monkey1.ini"   },
          { "id": 10, "filename": "monkey1_0.sav" },
          { "id": 11, "filename": "monkey1_1.sav" },
          { "id": 12, "filename": "monkey1_2.sav" },
          { "id": 13, "filename": "monkey1_3.sav" },
          { "id": 14, "filename": "monkey1_4.sav" },
          { "id": 15, "filename": "monkey1_5.sav" },
          { "id": 16, "filename": "monkey1_6.sav" },
          { "id": 17, "filename": "monkey1_7.sav" },
          { "id": 18, "filename": "monkey1_8.sav" },
          { "id": 19, "filename": "monkey1_9.sav" }
        ]
      }
    }
    ```

    Filename: `Monkey Island 1.json` (whatever name you want the launcher to show).

4. **Drop the assets on the SD:**

    ```
    <SD>/Assets/scummvm/ThinkElastic.ScummVM/monkey1.iso
    <SD>/Assets/scummvm/ThinkElastic.ScummVM/monkey1.cfg
    ```

    `make copy` puts the instance JSON and static assets in place automatically.

5. **Boot the Pocket** and launch the instance.

The four classic ISO instances we ship (MI1, MI2, DOTT, Atlantis) all follow this pattern. Use them as templates.

### Mode B — ZIP (for loose-folder distributions)

For games whose original media isn't a CD (GOG-extracted folders, repackaged remasters, etc.), pack the game files into a **STORE-mode** ZIP and the engine reads from inside the archive via ScummVM's `Common::makeZipArchive`.

The crucial requirements: zip with `-0` (no compression) and include a `game.cfg` at the archive root.

```sh
cd /path/to/Game\ Folder
cat > game.cfg <<EOF
gameid=tentacle
engineid=scumm
description=Day of the Tentacle
platform=pc
language=en
music=openfpga
EOF
zip -0 -r ~/dott.zip *.LFL *.LEC game.cfg
```

Instance JSON is identical to ISO mode except slot 3 points at the `.zip`:

```json
{ "id": 3, "filename": "monkey1se.zip" }
```

ZIP-mode behaviour:

- Files **larger than 16 MB** are *streamed* from the SD via a sub-stream view rather than copied into RAM (the Pocket has only 48 MB of app RAM; a single asset file can be hundreds of MB).
- Files **≤ 16 MB** are materialized into RAM on first access (faster for byte-at-a-time readers like ScummVM's text parsers).
- ZIP entries must be STORE (`-0`). Deflate is supported by the underlying decoder but no game ships compressed enough for it to be worth the CPU cost.
- Directory entries in the ZIP (the ones with trailing `/`) are tolerated.
- The current OS limits slot files to ~2 GB, so the ZIP must stay under that. If your game data is bigger, split it into two ZIPs and add a second data slot to the instance.

### Saves and config

Slots 10–19 are non-volatile and reserved for save game slots 0–9. Slot 9 is the per-instance `.ini` (ScummVM's persistent config). Both are written transparently as the engine plays. To wipe a game's saves, just delete those `.sav` files from the SD.

## Audio

- **MIDI**: AdLib OPL emulation (most music) runs through ScummVM's softsynth and out through our hardware audio mixer. MT-32 / General-MIDI cues route through `openfpga_midi`, which uses the Pocket's hardware PCM mixer + a sample-based soundfont (`bank.ofsf`, slot 4).
- **CD audio**: classic CD-era SCUMM games store music as CD-track references. Our ISO mount path doesn't wire up CDDA tracks directly; the engine falls back to MIDI for those games (which works for MI1/MI2 because the MIDI versions of every cue are still in the resource files).
- **Digital speech** (DOTT, Atlantis): standard `.SOU` / `MONSTER.SOU` files, decoded by upstream ScummVM codecs.

### Note on the Special Editions (MI1 SE, MI2 SE)

We don't ship instances for the LucasArts SE remasters because their audio is **xWMA** (a Microsoft-extended WMA Pro variant inside XACT wave banks) that uses undocumented bitstream extensions ("Reserved bit" and "Channel transform bit") that even mainline FFmpeg flags as unimplemented. With the canonical `block_align = 4459` supplied, `ffmpeg -i ...` decodes only ~10–15 % of an SE music entry before bailing.

A scaffolded WMA Pro decoder lives at `src/scummvm/scummvm/audio/decoders/wmapro.cpp` (currently gated off in `engines/scumm/soundse.cpp`). If a working xWMA decoder lands upstream — or you transcode the wave banks offline via Microsoft's `xWMAEncode.exe` — re-enabling is a one-line change. See `tools/wmapro_test/README.md` for the PC-side test harness.

For now, use the **classic CD** versions of MI1 and MI2 (the ISO instances we ship); they have MIDI music that plays fine.

## Repository layout

```
src/scummvm/                       core build root
  main.cpp                         boot, instance discovery, engine launcher
  backend/                         OSystem, mixer, MIDI, FS, save manager
  scummvm/                         vendored ScummVM submodule
dist/scummvm/                      core layout copied to SD by `make copy`
  Cores/ThinkElastic.ScummVM/      data.json + icon + variants
  Assets/scummvm/                  per-game instance JSONs + .cfg files
    ThinkElastic.ScummVM/
    common/                        shared .cfg files for ISO-based games
tools/wmapro_test/                 PC unit test for the WMA Pro decoder port
```

## Known limitations

- **LucasArts Special Editions (MI1 SE, MI2 SE) not shipped**: their xWMA audio isn't decodable today (see note above).
- **CDDA**: ISO-mounted images don't expose CDDA tracks; CD-style music falls back to MIDI.
- **No GUI launcher**: instance selection happens in the Pocket's own launcher; no in-game game-picker.
- **Save UI**: original ScummVM save dialog disabled (depends on theme assets we don't ship). Use F5 / F7 from inside SCUMM or game-specific save mechanics.
- **2 GB slot file cap**: a single zip or iso must stay under 2 GB until the OS lifts the limit.

## Building from source

Prerequisites: a RISC-V toolchain (`riscv64-elf-gcc`). The parent SDK Makefile (`make setup`) will install it on Arch / Debian / Fedora / macOS / NixOS / MSYS2 with native package managers.

```sh
make build CORE=scummvm
make copy  CORE=scummvm POCKETDEV=/run/media/$USER/POCKETDEV
```

The build expects the openfpgaOS SDK side-by-side at `../openfpgaOS-SDK/`. Update it with `git pull` there; this tree pulls headers, libc, and the `os.bin` kernel from there.

## License

ScummVM is GPLv3. This port inherits the same license — see `src/scummvm/scummvm/COPYING` for the full text. The openfpgaOS SDK (kernel, headers) is separately licensed; see its repo.

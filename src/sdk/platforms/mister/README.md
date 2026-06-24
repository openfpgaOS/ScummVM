# openfpgaOS SDK — MiSTer platform

Packaging for the MiSTer (DE10-Nano / SuperStation One) target. Unlike
the Pocket's APF JSON manifests, MiSTer needs no metadata: discovery is
by filename and folder convention.

## Artifacts

| File | Where on the MiSTer | What |
|---|---|---|
| `openfpgaOS.rbf` | `/media/fat/_Console/` | the core bitstream (`make build TARGET=mister` in openfpgaOS) |
| `boot.rom` | `/media/fat/games/openfpgaOS/` | the OS kernel (`os.bin` built with `TARGET=mister`), auto-loaded by the framework at core start |
| `openfpgaOS.vhd` | `/media/fat/games/openfpgaOS/` | FAT32 disk image with the app + assets + saves, mounted from the OSD (`Mount Disk`) |

## Disk image layout (the slot→path contract)

The OS resolves APF-style slot ids to fixed paths inside the image
(`openfpgaOS/src/firmware/os/targets/mister/file.c`):

```
/os.ini                  slot 2   OS config ([os] ELF=app.elf, ARGS=...)
/app.elf                 slot 3   default app
/bank.ofsf               slot 7   MIDI soundfont (optional)
/config/shared.cfg       slot 8   SDK shared config   (256 KB, preallocated)
/config/duke3d.cfg       slot 9   per-game settings   (256 KB, preallocated)
/saves/slot_0..9.sav     10–19    save slots          (256 KB, preallocated)
/assets/*                20+      app data, registered by directory scan —
                                  apps open these by filename as on Pocket
```

**Do not re-create the save/config files with ordinary tools.** They are
preallocated contiguously (FatFs `f_expand`) so the firmware can persist
saves by overwriting data clusters in place, never touching FAT metadata
— that is the power-cut safety guarantee. `mkimage.c` does this
correctly; a plain file copy may fragment them.

Filenames are limited to 23 characters (the OS registry's
`FILE_SLOT_NAME_MAX`), same as on Pocket.

## Usage

```sh
# build the image for an app (64 MB default; MISTER_IMAGE_MB overrides)
./mkimage.sh celeste path/to/celeste.elf assets/music.bin assets/sfx.bin

# push image + boot.rom + core to a networked MiSTer
./copy.sh celeste 192.168.1.42

# core-only bring-up (no app image yet; uses runtime/mister/ artifacts,
# synced from openfpgaOS with `make sdk DEST=path/to/sdk`)
./copy.sh core 192.168.1.42
```

`mkimage.sh` compiles `mkimage.c` on first use — a host-native image
builder linked against the same vendored FatFs the firmware mounts the
image with (`fatfs/`, `FF_USE_MKFS` + `FF_USE_EXPAND` enabled for the
host build only). No mtools/loopback/root required.

The image is a bare FAT32 volume (no MBR, `FM_SFD`) — MiSTer's mount
path accepts superfloppy images.

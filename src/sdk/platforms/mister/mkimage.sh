#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Build the MiSTer disk image for an app.
#
# Assembles build/mister/<APP>/openfpgaOS.vhd from the app's build
# output: the .elf (as /app.elf), a generated or app-provided os.ini,
# the MIDI soundfont, and any extra asset files (into /assets/).
# Save/config slots are preallocated by mkimage.c (FatFs f_expand).
#
# Usage: mkimage.sh <APP_NAME> <ELF_PATH> [extra asset files...]
#
# Platform: MiSTer (DE10-Nano / SuperStation One)
#

set -e

APP="$1"
ELF="$2"
shift 2 || true
ASSETS=("$@")

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
OUT_DIR="$SDK_ROOT/build/mister/$APP"
IMAGE="$OUT_DIR/openfpgaOS.vhd"
SIZE_MB="${MISTER_IMAGE_MB:-64}"
MKIMAGE="$SCRIPT_DIR/.mkimage"

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'
ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

[[ -z "$APP" || -z "$ELF" ]] && { echo "Usage: $0 <app_name> <elf_path> [assets...]"; exit 1; }
[[ -f "$ELF" ]] || fail "ELF not found: $ELF"

# ── Build the image tool on demand ───────────────────────────────────
if [[ ! -x "$MKIMAGE" || "$SCRIPT_DIR/mkimage.c" -nt "$MKIMAGE" ]]; then
    cc -O2 -I"$SCRIPT_DIR" -o "$MKIMAGE" \
        "$SCRIPT_DIR/mkimage.c" \
        "$SCRIPT_DIR/fatfs/ff.c" \
        "$SCRIPT_DIR/fatfs/ffunicode.c" || fail "mkimage build failed"
    ok "built mkimage tool"
fi

mkdir -p "$OUT_DIR"

# ── os.ini: app-provided or minimal default ──────────────────────────
OSINI="$OUT_DIR/os.ini"
if [[ -f "$(dirname "$ELF")/os.ini" ]]; then
    cp "$(dirname "$ELF")/os.ini" "$OSINI"
else
    printf '[os]\nELF=app.elf\n' > "$OSINI"
fi

# ── Collect payload specs ────────────────────────────────────────────
SPECS=("$OSINI=/os.ini" "$ELF=/app.elf")

# Soundfonts (bank.ofsf + any game .ofsf) → image root, opened by name.
for s in "$SDK_ROOT"/runtime/*.ofsf; do
    [[ -f "$s" ]] && SPECS+=("$s=/$(basename "$s")")
done

for a in "${ASSETS[@]}"; do
    [[ -f "$a" ]] || fail "asset not found: $a"
    SPECS+=("$a=/assets/$(basename "$a")")
done

# ── Assemble ─────────────────────────────────────────────────────────
"$MKIMAGE" "$IMAGE" "$SIZE_MB" "${SPECS[@]}"
ok "image: $IMAGE (${SIZE_MB} MB)"

# ── boot.rom alongside, for deploy convenience ───────────────────────
if [[ -f "$SDK_ROOT/runtime/mister/os.bin" ]]; then
    cp "$SDK_ROOT/runtime/mister/os.bin" "$OUT_DIR/boot.rom"
    ok "boot.rom (MiSTer-target os.bin)"
else
    echo "  note: runtime/mister/os.bin missing — sync it from openfpgaOS:"
    echo "        make os TARGET=mister && make sdk DEST=path/to/this/sdk"
fi

# ── core bitstream alongside, for a self-contained bundle ────────────
if [[ -f "$SDK_ROOT/runtime/mister/openfpgaOS.rbf" ]]; then
    cp "$SDK_ROOT/runtime/mister/openfpgaOS.rbf" "$OUT_DIR/openfpgaOS.rbf"
    ok "openfpgaOS.rbf (core bitstream)"
fi

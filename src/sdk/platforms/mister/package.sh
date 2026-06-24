#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# openfpgaOS SDK — MiSTer platform packager.
#
# Zips one built MiSTer bundle (build/mister/<label>/openfpgaOS.vhd +
# boot.rom + openfpgaOS.rbf) into a self-contained release ZIP.  Called
# by the generic scripts/package.sh dispatcher.
#
# Usage: package.sh <build_dir> <label> <releases_dir>
#
set -e
INPUT="$1"; LABEL="$2"; REL="$3"
SDK_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"
GREEN='\033[92m'; RESET='\033[0m'

[ -f "$INPUT/openfpgaOS.vhd" ] || exit 0   # not a MiSTer image bundle — skip

# Self-contained bundle: backfill kernel/bitstream from runtime/mister/.
[ -f "$INPUT/boot.rom" ]       || cp "$SDK_DIR/runtime/mister/os.bin"        "$INPUT/boot.rom"       2>/dev/null || true
[ -f "$INPUT/openfpgaOS.rbf" ] || cp "$SDK_DIR/runtime/mister/openfpgaOS.rbf" "$INPUT/openfpgaOS.rbf" 2>/dev/null || true
[ -f "$INPUT/boot.rom" ]       || { echo "Error: no boot.rom for $LABEL — sync runtime/mister (openfpgaOS: make sdk DEST=...)"; exit 1; }
[ -f "$INPUT/openfpgaOS.rbf" ] || { echo "Error: no openfpgaOS.rbf for $LABEL — sync runtime/mister"; exit 1; }

# Version from the core's dist metadata (custom cores); SDK demo apps
# have no per-app core.json — fall back to 1.0.0.
GAME_VERSION=$(python3 -c "
import json, glob
js = glob.glob('$SDK_DIR/dist/$LABEL/Cores/*/core.json')
print(json.load(open(js[0]))['core']['metadata']['version'] if js else '1.0.0')
" 2>/dev/null || echo "1.0.0")

OUTPUT="$REL/${LABEL}-v${GAME_VERSION}.zip"

cat > "$INPUT/INSTALL.txt" << EOF
$LABEL for openfpgaOS (MiSTer)

Version: $GAME_VERSION

Installation:
1. openfpgaOS.rbf  -> /media/fat/_Console/
2. boot.rom        -> /media/fat/games/openfpgaOS/
3. openfpgaOS.vhd  -> /media/fat/games/openfpgaOS/
   (rename to $LABEL.vhd if you keep several games side by side)
4. Launch openfpgaOS from the Console menu, then "Mount Disk" in the
   OSD and pick the image.

Saves live INSIDE the image — never recreate /saves or /config with
ordinary tools (they are preallocated for power-cut safety).
EOF

(cd "$INPUT" && rm -f "$OUTPUT" 2>/dev/null; \
 zip "$OUTPUT" openfpgaOS.vhd boot.rom openfpgaOS.rbf INSTALL.txt >/dev/null)

echo -e "${GREEN}Package created: $OUTPUT${RESET}"
echo "  Size: $(du -h "$OUTPUT" | cut -f1)"

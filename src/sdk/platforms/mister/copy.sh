#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Deploy to a MiSTer over the network.
#
# Same calling convention as platforms/pocket/copy.sh so a scaffolded
# app's `make copy TARGET=mister` works unchanged — the third argument
# is the destination (an IP/hostname here, an SD path on Pocket).
#
#   copy.sh <APP> <ELF_PATH> [MISTER_IP]   app mode: build the disk image
#                                          (mkimage.sh: ELF + the asset
#                                          files next to it) and push it
#                                          with boot.rom + core
#   copy.sh core [MISTER_IP]               core-only bring-up: push the
#                                          runtime/mister/ artifacts
#                                          (synced from openfpgaOS with
#                                          `make sdk DEST=...`)
#
# MISTER_IP also honors the environment (default mister.local).
# Mount the .vhd once from the OSD; MiSTer remembers it.
#
# Platform: MiSTer (DE10-Nano / SuperStation One)
#

set -e

APP="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
RUNTIME="$SDK_ROOT/runtime/mister"

CORE_DIR=/media/fat/_Console
GAMES_DIR=/media/fat/games/openfpgaOS

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'
ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

[[ -z "$APP" ]] && { echo "Usage: $0 <app_name> <elf_path> [mister_ip]  |  $0 core [mister_ip]"; exit 1; }

# ── Parse the pocket-compatible argument shapes ──────────────────────
ELF=""
if [[ "$APP" == "core" ]]; then
    MISTER_IP="${2:-${MISTER_IP:-mister.local}}"
else
    ELF="$2"
    MISTER_IP="${3:-${MISTER_IP:-mister.local}}"
    [[ -f "$ELF" ]] || fail "ELF not found: $ELF"
fi

OUT_DIR="$SDK_ROOT/build/mister/$APP"

# ── App mode: assemble the disk image from the ELF + its assets ──────
PUSH_VHD=""
PUSH_BOOT=""
PUSH_RBF=""

if [[ -n "$ELF" ]]; then
    # Asset files live next to the ELF (the app's build/.../common dir on
    # the pocket layout, or wherever the ELF was built).  Everything that
    # isn't a kernel/ELF/manifest goes into /assets/ — the OS registers
    # them by filename, exactly like Pocket data slots.
    ASSETS=()
    ELF_DIR="$(dirname "$ELF")"
    for f in "$ELF_DIR"/*; do
        [[ -f "$f" ]] || continue
        case "$(basename "$f")" in
            os.bin|bank.ofsf|*.elf|*.json|*.ini) continue ;;
        esac
        ASSETS+=("$f")
    done
    "$SCRIPT_DIR/mkimage.sh" "$APP" "$ELF" "${ASSETS[@]}"
    PUSH_VHD="$OUT_DIR/openfpgaOS.vhd"
    [[ -f "$OUT_DIR/boot.rom" ]] && PUSH_BOOT="$OUT_DIR/boot.rom"
fi

[[ -z "$PUSH_BOOT" && -f "$RUNTIME/os.bin" ]] && PUSH_BOOT="$RUNTIME/os.bin"
[[ -f "$RUNTIME/openfpgaOS.rbf" ]] && PUSH_RBF="$RUNTIME/openfpgaOS.rbf"
[[ -f "$OUT_DIR/openfpgaOS.rbf" ]] && PUSH_RBF="$OUT_DIR/openfpgaOS.rbf"

[[ -z "$PUSH_VHD$PUSH_BOOT$PUSH_RBF" ]] && \
    fail "nothing to push — build an app image or sync core artifacts (openfpgaOS: make sdk DEST=...)"

# ── Push ─────────────────────────────────────────────────────────────
echo "Deploying to root@$MISTER_IP"
ssh "root@$MISTER_IP" "mkdir -p $CORE_DIR $GAMES_DIR"

if [[ -n "$PUSH_RBF" ]]; then
    scp "$PUSH_RBF" "root@$MISTER_IP:$CORE_DIR/openfpgaOS.rbf"
    ok "openfpgaOS.rbf → $CORE_DIR"
fi

if [[ -n "$PUSH_BOOT" ]]; then
    scp "$PUSH_BOOT" "root@$MISTER_IP:$GAMES_DIR/boot.rom"
    ok "boot.rom → $GAMES_DIR"
fi

if [[ -n "$PUSH_VHD" ]]; then
    scp "$PUSH_VHD" "root@$MISTER_IP:$GAMES_DIR/"
    ok "openfpgaOS.vhd → $GAMES_DIR"
fi

echo "Done. Load the core from the MiSTer menu${PUSH_VHD:+ and mount the disk image}."

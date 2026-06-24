#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# openfpgaOS SDK — MiSTer platform: assemble ONE app's deliverable
# (a FAT32 disk image + boot.rom + rbf under build/mister/<app>/).
#
# Uniform image.sh contract with the Pocket sibling; the dist_dir arg is
# ignored (MiSTer images carry no APF manifests — discovery is by
# filename inside the image).  Delegates to mkimage.sh, which derives the
# SDK root and assembles build/mister/<app>/.
#
# Usage: image.sh <app> <elf> <sdk_root> <dist_dir|""> [assets...]
#
set -e
APP="$1"; ELF="$2"
# Drop the 4 fixed positionals (app, elf, sdk_root, dist_dir); the rest
# are asset files.  Guard the shift so a short call can't leave stray
# positionals that would reach mkimage as bogus assets.
if [ $# -ge 4 ]; then shift 4; else shift $#; fi
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/mkimage.sh" "$APP" "$ELF" "$@"

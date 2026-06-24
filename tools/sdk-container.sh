#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# Run an openfpgaOS-SDK app build (or any command) inside the containerized
# RISC-V toolchain.  Mirrors firmware-container.sh but for SDK app builds —
# the toolchain image is shared (riscv64-unknown-elf-gcc + picolibc + musl
# headers + make + python3 + bsdmainutils).
#
# Why this exists: SDK app developers (and downstream cores reusing this
# SDK) shouldn't have to install a RISC-V toolchain by hand — particularly
# painful on macOS where Homebrew's `riscv64-elf-gcc` ships without a libc.
# `bash sdk-container.sh make <targets>` Just Works on any host with Docker.
#
# This script is mirrored to the openfpgaOS-SDK repo (and to any other core
# repo that does `make sdk DEST=<path>`) so SDK consumers get the same
# Docker-only build path without needing the openfpgaCore source.
#
# Usage:
#   bash sdk-container.sh make APP=foo SRCS="main.c"        # one-shot build
#   bash sdk-container.sh -C src/apps/myapp make            # build in subdir
#   bash sdk-container.sh bash                              # interactive shell
#
# Image is auto-built on first use.  Override the toolchain image with
# SDK_IMG=<tag>.  Override the Dockerfile location with SDK_DOCKERFILE=<path>.
set -euo pipefail

# Discover the repo root that contains us.  When this script is shipped INSIDE
# the SDK repo (or a downstream core repo) it'll resolve to that repo's root;
# when invoked from openfpgaCore directly, it resolves to openfpgaCore.  Either
# way, that root is bind-mounted into the container at the same path so
# absolute paths in app Makefiles resolve identically inside.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${SDK_IMG:-openfpgaos-firmware}"
DOCKERFILE="${SDK_DOCKERFILE:-$REPO/tools/docker/Dockerfile.firmware}"

# Optional -C <dir> like make: change to <dir> before running the command.
# Resolve symlinks (pwd -P) so the WORKDIR prefix matches REPO — on macOS,
# /tmp is a symlink to /private/tmp, and bind-mounting /private/tmp/... while
# -w /tmp/... gives the container an inconsistent view and `make -f Makefile`
# claims the makefile is missing.
WORKDIR="$(pwd -P)"
if [ "${1:-}" = "-C" ]; then
    WORKDIR="$(cd "$2" && pwd -P)"
    shift 2
fi
[ $# -ge 1 ] || { echo "usage: sdk-container.sh [-C <dir>] <command> [args...]"; exit 2; }

# Build the image on first use (one-time).  Auto-builds match the pattern
# used by firmware-container.sh / vexii-container.sh — fresh checkout +
# Docker is enough.
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    [ -f "$DOCKERFILE" ] || { echo "ERROR: $DOCKERFILE missing — set SDK_DOCKERFILE=<path>"; exit 1; }
    echo "[sdk] building $IMG image (one-time, ~2-3 min)..."
    docker build -t "$IMG" -f "$DOCKERFILE" "$(dirname "$DOCKERFILE")"
fi

# Pseudo-TTY only when our stdout is a terminal, so an interactive build
# keeps gcc's colored output but a redirected/piped run keeps logs clean.
# The ${TTY[@]+"${TTY[@]}"} idiom is required for bash 3.2 (macOS default)
# under set -u — plain "${TTY[@]}" errors on an empty array there.
TTY=()
[ -t 1 ] && TTY=(-i -t)

# --user keeps outputs owned by the host user.  Repo bind-mounted at the
# SAME path so absolute paths inside the app Makefile resolve identically.
# WORKDIR (cwd inside the container) is whatever the user was in on the
# host (or wherever -C took us) — feels like running make locally.
#
# HOME goes on a dedicated tmpfs at /sdkhome (not /tmp) to avoid shadowing
# repo bind-mounts whose host path happens to live under /tmp.
exec docker run --rm ${TTY[@]+"${TTY[@]}"} \
  --user "$(id -u):$(id -g)" \
  -v "$REPO:$REPO" \
  --tmpfs /sdkhome:exec \
  -e HOME=/sdkhome \
  -w "$WORKDIR" \
  "$IMG" \
  "$@"

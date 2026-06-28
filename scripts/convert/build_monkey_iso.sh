#!/usr/bin/env bash
#
# build_monkey_iso.sh -- produce port-playable artifacts for Monkey Island 1
# and 2, WITH Special-Edition voices, from the Steam Special Edition data.
#
#   Both games run as the SCUMM **SE variant** (detection_tables.h:198/204,
#   GF_DOUBLEFINE_PAK).  That flag is what enables shouldInjectMISEAudio()
#   (sound.cpp:2254) so the engine plays the SE speech over the classic
#   scripts.  GF_DOUBLEFINE_PAK ALSO makes the engine read its resources from
#   the DoubleFine container `monkey1.pak`/`monkey2.pak` (ScummPAKFile;
#   resource.cpp:174) instead of loose monkey?.000/.001 -- so each ISO ships
#   the .pak whole.  The instance ini must set `variant=SE` and the launcher
#   sets `use_remastered_audio=true` for these gameids (src/scummvm/main.cpp).
#
#   No transcoding: the SE speech banks are copied into the ISO verbatim and
#   decoded on-device by built-in decoders (MI1 Speech.xwb = PCM, MI2 = MS
#   -ADPCM; neither needs WMA).
#
#   MI1 (gameid 'monkey', variant=SE):
#       monkey1.iso  (slot 4, kernel of_iso_mount -> /cd) contains:
#         monkey1.pak    -- engine container (classic data inside)
#         Speech.xwb     -- SE speech, PCM 44.1k mono (built-in decode)
#         speech.info    -- hash->clip cue table (initAudioMappingMI)
#         track1.wav..   -- classic CD-audio music, OFFLINE-decoded from the
#                           SE MusicOriginal.xwb (see MUSIC below)
#       The engine finds the loose files by exact basename through SearchMan
#       (the `audio` subdir is added in scumm.cpp:1066-1069, and /cd root is on
#       the search path).
#       MUSIC: classic CD audio as track%d.wav.  Under SE, GF_DOUBLEFINE_PAK
#       sets _hasFileBasedCDAudio and routes MI1 music to MusicOriginal.xwb via
#       SoundSE; that bank is mostly xWMA, which the port can't decode at
#       runtime (wmapro.cpp is a stub -> soundse.cpp returns null).  So we
#       decode it OFFLINE here with ffmpeg (the bank's xWMA/PCM entries -> PCM
#       WAV) into track%d.wav, and the engine's NONSTANDARD_PORT fallback in
#       soundcd.cpp plays those via the classic AudioCD manager when the SE
#       stream is null.  Result: MI1 has BOTH SE voices and music.  (Needs
#       ffmpeg at build time; without it the ISO still builds, music-less.)
#
#   MI2 (gameid 'monkey2', variant=SE):
#       monkey2.iso  (slot 4) contains:
#         monkey2.pak    -- engine container (classic iMUSE MIDI inside)
#         Speech.xwb     -- SE speech, MS-ADPCM mono (built-in decode)
#         speech.info    -- hash->clip cue table
#         SpeechCues.xsb -- names for MI2's nameless XWB entries (REQUIRED)
#       MUSIC: unaffected.  MI2 music is iMUSE AdLib/MT-32 MIDI via the
#       openfpga synth (_vm->_musicEngine), separate from SoundSE.  MI2 has no
#       GF_AUDIOTRACKS so _hasFileBasedCDAudio stays false; voices + MIDI both
#       play.
#
# Pure-python only (no ffmpeg/vgmstream/mkisofs/xorriso/pycdlib): files are
# copied byte-for-byte into a hand-rolled ISO9660 image (scripts/convert/lib_iso9660.py,
# large-file + untruncated-name aware).
#
# Usage:
#   scripts/convert/build_monkey_iso.sh [monkey1|monkey2|all] [--out DIR]
#                               [--mi1-dir DIR] [--mi2-dir DIR] [--keep]
#
# Idempotent: re-running rebuilds artifacts; intermediates live in a work dir
# (default: $SCRATCH or ./build/monkey-work) and are removed unless --keep.
# Final artifacts are written to the --out directory (default: ./build/monkey).
# Nothing is ever written into tracked repo locations.

set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1   # keep scripts/convert/__pycache__ out of the repo

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- defaults -------------------------------------------------------------
# Default to a standard Linux Steam install; override with --mi1-dir/--mi2-dir
# for other layouts (macOS, flatpak, a custom dump directory, etc.).
STEAM_COMMON="${STEAM_COMMON:-$HOME/.local/share/Steam/steamapps/common}"
MI1_DIR_DEFAULT="$STEAM_COMMON/The Secret of Monkey Island Special Edition"
MI2_DIR_DEFAULT="$STEAM_COMMON/Monkey2"

TARGET="all"
OUT_DIR=""
WORK_DIR=""
MI1_DIR="$MI1_DIR_DEFAULT"
MI2_DIR="$MI2_DIR_DEFAULT"
KEEP=0

# ---- arg parsing ----------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        monkey1|monkey2|all) TARGET="$1"; shift ;;
        --out)      OUT_DIR="$2"; shift 2 ;;
        --mi1-dir)  MI1_DIR="$2"; shift 2 ;;
        --mi2-dir)  MI2_DIR="$2"; shift 2 ;;
        --keep)     KEEP=1; shift ;;
        -h|--help)
            sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"   # scripts/convert -> repo root
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/monkey}"
if [ -z "$WORK_DIR" ]; then
    # Always nest a private subdir so the cleanup `rm -rf "$WORK_DIR"` can only
    # ever remove a dir this script created -- never a shared exported $SCRATCH.
    if [ -n "${SCRATCH:-}" ]; then
        WORK_DIR="$SCRATCH/monkey-work"
    else
        WORK_DIR="$REPO_ROOT/build/monkey-work"
    fi
fi
mkdir -p "$OUT_DIR" "$WORK_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
WORK_DIR="$(cd "$WORK_DIR" && pwd)"

PY=python3

# ---- tool checks ----------------------------------------------------------
need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: required tool '$1' not found." >&2
        echo "  install hint: $2" >&2
        exit 3
    }
}
need "$PY"    "pacman -S python    (python3)"

echo "== build_monkey_iso.sh =="
echo "   target   : $TARGET"
echo "   out dir  : $OUT_DIR"
echo "   work dir : $WORK_DIR"
echo

# ---- helpers --------------------------------------------------------------

# SCRIPT_DIR is exported for lib_iso9660.py / any helper invoked below.
export SCRIPT_DIR

build_iso_image() {
    # build_iso_image <out.iso> <volid> <file...>
    local out="$1" volid="$2"; shift 2
    "$PY" "$SCRIPT_DIR/lib_iso9660.py" "$out" "$volid" "$@"
}

# ---- MI1 ------------------------------------------------------------------
#
# SE VOICES.  To make the SCUMM engine inject the Special-Edition speech, the
# game must be detected as the *SE* variant (detection_tables.h:198, the only
# `monkey` row carrying GF_DOUBLEFINE_PAK).  shouldInjectMISEAudio()
# (sound.cpp:2254) requires GID_MONKEY && GF_DOUBLEFINE_PAK &&
# use_remastered_audio.  GF_DOUBLEFINE_PAK ALSO switches the engine to read
# its resources from the DoubleFine container `monkey1.pak` via ScummPAKFile
# (resource.cpp:174, scumm.cpp:1066-1149) -- NOT the classic monkey1.000/.001.
# So the SE-voices ISO bundles `monkey1.pak` (the engine container) plus the
# loose speech bank + mapping file the engine opens by exact basename through
# SearchMan: `Speech.xwb` (PCM, built-in decode) and `speech.info` (the
# hash->clip cue table).  No SpeechCues.xsb for MI1 (its XWB entries are
# already named; indexSpeechXSBFile is MI2-only, soundse.cpp:64-69).
#
# MUSIC: under GF_DOUBLEFINE_PAK the MI1 CD-audio router sets
# _hasFileBasedCDAudio=true (soundcd.cpp:48-50) and serves music from
# MusicOriginal.xwb via SoundSE.  That bank is mostly xWMA, which the port can't
# decode at runtime (wmapro.cpp is a stub), so the SE stream is null.  Instead of
# leaving it silent we decode the bank OFFLINE with ffmpeg into the classic
# track%d.wav set (build_mi1_music.py: lib_xwb extract -> ffmpeg -> PCM WAV,
# named per lib_mi1_trackmap's +1 Red-Book map), bundle those in the ISO, and the
# engine's NONSTANDARD_PORT fallback (soundcd.cpp) plays them via the AudioCD
# manager when the SE stream is null.  So MI1 keeps SE voices AND gets music.
# ffmpeg is required only for this step; if it is missing we warn and ship a
# music-less (but fully voiced) ISO.  Tune size with MI1_MUSIC_RATE / MI1_MUSIC_MONO.
build_mi1() {
    local pak="$MI1_DIR/Monkey1.pak"
    local speech="$MI1_DIR/audio/Speech.xwb"
    local info="$MI1_DIR/audio/speech.info"
    local music="$MI1_DIR/audio/MusicOriginal.xwb"
    [ -f "$pak" ]    || { echo "ERROR: $pak not found (set --mi1-dir)" >&2; exit 4; }
    [ -f "$speech" ] || { echo "ERROR: $speech not found (set --mi1-dir)" >&2; exit 4; }
    [ -f "$info" ]   || { echo "ERROR: $info not found (set --mi1-dir)" >&2; exit 4; }

    # Compress the 838 MB 44.1k PCM Speech.xwb to MS-ADPCM (~3.8:1) so the SD/
    # bridge streams far fewer bytes -- SoundSE reads each clip's codec/rate from
    # the bank metadata and decodes ADPCM exactly as it already does for MI2, so
    # no engine change.  MI1_SPEECH_RATE adds downsampling on top (e.g. 22050 ->
    # ~7:1); MI1_SPEECH_PCM=1 keeps PCM (downsample only).  Falls back to the
    # verbatim bank if the re-encode fails (e.g. no ffmpeg).
    local procspeech="$WORK_DIR/Speech.xwb"
    echo "[MI1] compressing Speech.xwb (${MI1_SPEECH_PCM:+PCM }${MI1_SPEECH_PCM:-MS-ADPCM}${MI1_SPEECH_RATE:+ @ ${MI1_SPEECH_RATE}Hz})..."
    if "$PY" "$SCRIPT_DIR/build_mi1_speech.py" "$speech" "$procspeech" \
            ${MI1_SPEECH_RATE:+--rate "$MI1_SPEECH_RATE"} \
            ${MI1_SPEECH_MONO:+--mono} ${MI1_SPEECH_PCM:+--pcm}; then
        speech="$procspeech"
    else
        echo "[MI1] WARNING: speech compress failed -- shipping verbatim Speech.xwb." >&2
    fi

    # Slim the 1.24 GB Monkey1.pak to just the classic data the engine reads
    # (classic/en/monkey1.00x, ~4.8 MB) -- the rest is unused remaster assets.
    local minpak="$WORK_DIR/monkey1.pak"
    echo "[MI1] slimming Monkey1.pak to classic data..."
    if "$PY" "$SCRIPT_DIR/build_min_pak.py" "$pak" "$minpak" "classic/"; then
        pak="$minpak"
    else
        echo "[MI1] WARNING: pak slim failed -- shipping full pak." >&2
    fi

    # Offline-decode the SE music bank into classic CD-audio track%d.wav files.
    local trackdir="$WORK_DIR/mi1-tracks"
    rm -rf "$trackdir"; mkdir -p "$trackdir"
    local tracks=()
    if [ -f "$music" ]; then
        echo "[MI1] decoding SE music -> track%d.wav via ffmpeg..."
        if "$PY" "$SCRIPT_DIR/build_mi1_music.py" "$music" "$trackdir" \
                ${MI1_MUSIC_RATE:+--rate "$MI1_MUSIC_RATE"} \
                ${MI1_MUSIC_MONO:+--mono}; then
            while IFS= read -r w; do tracks+=("$w"); done \
                < <(ls "$trackdir"/track*.wav 2>/dev/null)
        else
            echo "[MI1] WARNING: music decode failed -- building voices-only ISO." >&2
        fi
    else
        echo "[MI1] note: $music absent -- building voices-only ISO." >&2
    fi

    echo "[MI1] building monkey1.iso (slim pak + ADPCM speech + ${#tracks[@]} ADPCM CD tracks)..."
    # Slot 4 ISO: slim monkey1.pak (classic data, read via ScummPAKFile), the
    # MS-ADPCM speech bank, speech.info, and the MS-ADPCM track%d.wav music --
    # all at the ISO root.  Names are emitted untruncated by lib_iso9660.py so
    # `speech.info` stays `SPEECH.INFO;1` (8.3-truncation would break the
    # exact-basename open; track%d.wav already fits 8.3).
    build_iso_image "$OUT_DIR/monkey1.iso" "MONKEY1" \
        "$pak" "$speech" "$info" ${tracks[@]+"${tracks[@]}"}

    echo "[MI1] done (SE voices + CD music, all MS-ADPCM, slim pak)."
}

# ---- MI2 ------------------------------------------------------------------
#
# SE VOICES.  Same gating as MI1: detect as the SE variant
# (detection_tables.h:204, the GF_DOUBLEFINE_PAK `monkey2` row), which makes
# the engine read `monkey2.pak` via ScummPAKFile and enables
# shouldInjectMISEAudio().  MI2 speech needs THREE loose files (engine opens
# them by exact basename via SearchMan): `Speech.xwb` (MS-ADPCM, built-in
# decode), `speech.info` (hash->clip cue table), and `SpeechCues.xsb`
# (indexSpeechXSBFile, soundse.cpp:220 -- supplies the NAMES for MI2's
# nameless XWB entries; REQUIRED for MI2, unlike MI1).
#
# MUSIC: unaffected.  MI2 music is iMUSE AdLib/MT-32 MIDI rendered by the
# openfpga synth through _vm->_musicEngine -- entirely separate from SoundSE.
# MI2 has no GF_AUDIOTRACKS, so _hasFileBasedCDAudio stays false and neither
# use_remastered_audio nor GF_DOUBLEFINE_PAK reroutes it.  We bundle speech
# only; MIDI music keeps playing.  The MIDI lives inside monkey2.pak's classic
# resources, so no separate verification scan is needed (the pak is shipped
# whole).
build_mi2() {
    local pak="$MI2_DIR/monkey2.pak"
    local speech="$MI2_DIR/audio/Speech.xwb"
    local info="$MI2_DIR/audio/speech.info"
    local cues="$MI2_DIR/audio/SpeechCues.xsb"
    [ -f "$pak" ]    || { echo "ERROR: $pak not found (set --mi2-dir)" >&2; exit 4; }
    [ -f "$speech" ] || { echo "ERROR: $speech not found (set --mi2-dir)" >&2; exit 4; }
    [ -f "$info" ]   || { echo "ERROR: $info not found (set --mi2-dir)" >&2; exit 4; }
    [ -f "$cues" ]   || { echo "ERROR: $cues not found (set --mi2-dir)" >&2; exit 4; }

    # Slim the 506 MB monkey2.pak to just the classic data (iMUSE MIDI + scripts
    # in classic/en/monkey2.00x) the engine actually reads.  MI2's Speech.xwb is
    # ALREADY MS-ADPCM, so it ships verbatim (no re-encode needed).
    local minpak="$WORK_DIR/monkey2.pak"
    echo "[MI2] slimming monkey2.pak to classic data..."
    if "$PY" "$SCRIPT_DIR/build_min_pak.py" "$pak" "$minpak" "classic/"; then
        pak="$minpak"
    else
        echo "[MI2] WARNING: pak slim failed -- shipping full pak." >&2
    fi

    echo "[MI2] building monkey2.iso (slim pak + SE speech bank)..."
    # Slot 4 ISO: slim monkey2.pak (engine container) + verbatim SE speech files.
    # SpeechCues.xsb is emitted untruncated (8.3 would mangle it to
    # SPEECHCU.XSB and break MI2 voice naming).
    build_iso_image "$OUT_DIR/monkey2.iso" "MONKEY2" \
        "$pak" "$speech" "$info" "$cues"

    echo "[MI2] done (SE voices; iMUSE MIDI music via synth; slim pak)."
}

# ---- run ------------------------------------------------------------------
case "$TARGET" in
    monkey1) build_mi1 ;;
    monkey2) build_mi2 ;;
    all)     build_mi1; build_mi2 ;;
esac

if [ "$KEEP" -eq 0 ]; then
    rm -rf "$WORK_DIR"
fi

# ---- summary --------------------------------------------------------------
echo
echo "================ ARTIFACTS ================"
[ -f "$OUT_DIR/monkey1.iso" ] && ls -l "$OUT_DIR/monkey1.iso"
[ -f "$OUT_DIR/monkey2.iso" ] && ls -l "$OUT_DIR/monkey2.iso"
cat <<EOF

================ STAGING (APF data slots) ================
MI1 (instance "Monkey Island 1"):
  slot 4  <- monkey1.iso   (data_file; kernel of_iso_mount -> /cd)
                           contains monkey1.pak, Speech.xwb, speech.info,
                           track1.wav..track24.wav (offline-decoded music)
  instance ini (monkey1_os.ini) must declare:
      variant=SE
      data_file=monkey1.iso
      voices=true            (launcher -> use_remastered_audio=true)
  Music: classic CD audio as MS-ADPCM track%d.wav (decoded from MusicOriginal.xwb
         with ffmpeg; played via the soundcd.cpp SE fallback).
  Size : ~336 MB by default -- slim pak 4.8 MB (just classic data; the rest of
         the 1.24 GB pak is unused remaster assets) + MS-ADPCM speech ~218 MB +
         speech.info 6 MB + MS-ADPCM music ~122 MB.  All audio is MS-ADPCM
         (~3.8:1) so the SD/bridge streams ~4x fewer bytes.  Shrink further:
           speech  MI1_SPEECH_RATE=22050   (~218 -> ~109 MB)   MI1_SPEECH_PCM=1 keeps PCM
           music   MI1_MUSIC_RATE=22050 MI1_MUSIC_MONO=1       (~122 -> ~35 MB)

MI2 (instance "Monkey Island 2"):
  slot 4  <- monkey2.iso   (data_file; kernel of_iso_mount -> /cd)
                           contains monkey2.pak, Speech.xwb, speech.info, SpeechCues.xsb
  instance ini (monkey2_os.ini) must declare:
      variant=SE
      data_file=monkey2.iso
      voices=true
  Music: iMUSE MIDI via the synth (music=openfpga), unaffected by SE voices.

  (These ini edits are ALREADY APPLIED in the repo by this change.)
NOTE: the .sav slots (10..18) and ini slot 9 are created at runtime; not built here.
EOF

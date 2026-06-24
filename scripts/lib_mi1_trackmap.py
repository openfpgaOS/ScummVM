#!/usr/bin/env python3
"""Authoritative MI1 (The Secret of Monkey Island) CD-audio track map.

PROBLEM
-------
The SCUMM engine, when a room script triggers CD music, computes a *CD track
number* T and asks the audio-CD manager to play it.  On the openfpga port the
manager has no .cue, so it falls through to DefaultAudioCDManager, which opens
"track<T>.wav" by name (see backends/audiocd/default/default-audiocd.cpp).  So
the build must place, in trackT.wav, the music that belongs to engine CD track
T.

The re-recorded Special Edition music lives in audio/MusicOriginal.xwb as 24
wave-bank entries NAMED "track2".."track25" (+ a synthetic "silence").  The SE
named these by the *Red Book* track number on the original mixed-mode Audio-CD:
track 1 of that disc is the DATA track, the 24 music tracks are Red Book tracks
2..25.

The engine, however, numbers the same 24 music tracks 1..24 in its little 32-
byte CD-audio descriptor resources (soundIDs 100..129 in monkey1.001, byte
+0x18 = track).  i.e. engine track T == Red Book track (T+1) == SE wave
"track{T+1}".

MAPPING  (engine track T  ->  SE wave name)
-------------------------------------------
    engine T  ->  SE wave "track{T+1}"  ->  emit as trackT.wav

This is exactly what stock ScummVM does on its own SE path: it calls
getAudioStreamFromIndex(track, kSoundSETypeCDAudio), and _musicEntries is filled
in wave-bank storage order, so _musicEntries[T] is the (T)-th stored entry.
Because the bank is stored track2,track3,... after a single hoisted "track22"
at index 0, _musicEntries[T] is the wave named "track{T+1}" for the whole
contiguous run the game uses (engines/scumm/soundse.cpp:147-164,
engines/scumm/soundcd.cpp:164).

EVIDENCE THIS IS RIGHT (not an off-by-one)
------------------------------------------
Engine CD-track descriptors actually requested by room scripts (parsed from
monkey1.001, replicating getCDTrackIdFromSoundId):
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19, 21,22,23,24}
    -> 23 distinct tracks, with a single GAP at engine track 20.
The SE bank has 24 music waves track2..track25.  Under the +1 map every engine
track resolves to an existing wave AND exactly ONE wave is left over: "track21"
(= Red Book 21), which lines up precisely with the engine's missing track 20.
A name-IDENTITY map (engine T -> wave "trackT") instead leaves THREE anomalies
(no wave for engine track 1, and waves track20 & track25 unused), so identity
is wrong.  The single clean gap is the tell that the +1 map is correct.

The hard anchor is preserved: engine track 17 = the intro/title music (the
GID_MONKEY `track == 17` special case in soundcd.cpp:222; soundID 110 room 10
start 0 = intro, soundID 108 room 38 start 01:35.48 = title reprise).  Under the
+1 map engine 17 -> SE wave "track18" (2:42), long enough to contain the reprise
at the 95.48 s offset.  (The single longest WMA, "track17" 3:42, is Red Book 17
= engine track 16 -- a different scene, not the intro; "longest == intro" is a
heuristic that does NOT hold here.)

NOTE the engine-internal numbers are NOT Red Book absolute and NOT contiguous
with the wave names beyond the +1 relation; do not try to "simplify" this to an
identity map.
"""

import re

# The set of CD tracks the engine actually requests, with the scene each track
# accompanies (room numbers are exact, derived from the DSOU/DROO directory of
# monkey1.000/.001; scene labels are annotations).  Engine track 20 is never
# requested (its Red Book wave "track21" is therefore unused).
ENGINE_TRACKS = {
    1:  "Melee Island dock / map screen",
    2:  "Monkey Island beach",
    3:  "Sword Master's house",
    4:  "Cannibal village",
    5:  "Ghost ship",
    6:  "Ending / credits",
    7:  "Giant monkey head area",
    8:  "Circus tent",
    9:  "Voodoo / SCUMM-related",
    10: "Voodoo (alt)",
    11: "Tunnels / mansion",
    12: "Fettucini circus",
    13: "Fettucini circus sting",
    14: "Melee streets / lookout",
    15: "Voodoo Lady's house",
    16: "Giant monkey head (alt / Red Book longest cue)",
    17: "Title / intro (+ 01:35 title reprise)",   # hard anchor
    18: "Forest / clearing",
    19: "Ship interior sting",
    # 20 intentionally absent (no room requests it)
    21: "Ship",
    22: "Ship / hold",
    23: "Monkey Island interior",
    24: "Ship deck (alt)",
}


def engine_track_to_wave_name(track):
    """Engine CD track number -> SE wave-bank entry name that holds its music."""
    return "track%d" % (track + 1)


def wave_name_to_engine_track(name):
    """SE wave-bank entry name -> engine CD track number, or None if the wave
    has no engine track (i.e. it is 'silence', or Red Book 'track21' which the
    game never triggers).  The decoded WAV must be written as trackT.wav."""
    m = re.fullmatch(r"track(\d+)", name)
    if not m:
        return None                      # 'silence' or anything unexpected
    redbook = int(m.group(1))            # SE wave 'trackN' == Red Book track N
    track = redbook - 1                  # engine track == Red Book - 1
    if track not in ENGINE_TRACKS:
        return None                      # e.g. 'track21' -> engine 20 (unused)
    return track


def build_filename_map(wave_names):
    """Given the list of SE wave names present in the bank, return a dict
    {output_basename 'trackT.wav': source_wave_name} for every wave that maps to
    an engine-requested CD track.  Waves with no engine track are dropped."""
    out = {}
    for nm in wave_names:
        t = wave_name_to_engine_track(nm)
        if t is None:
            continue
        out["track%d.wav" % t] = nm
    return out


if __name__ == "__main__":
    # Self-print the authoritative table for auditing.
    print("engine CD track T  ->  SE wave        ->  output file   scene")
    for t in sorted(ENGINE_TRACKS):
        wn = engine_track_to_wave_name(t)
        print("  %2d              ->  %-9s    ->  track%d.wav    %s"
              % (t, wn, t, ENGINE_TRACKS[t]))
    print("\n(SE wave 'track21' (Red Book 21) and 'silence' are unused: the "
          "engine never requests CD track 20.)")

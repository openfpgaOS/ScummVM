#!/usr/bin/env python3
"""Prepare ScummVM openfpgaOS assets from a user source folder.

The shipped instances use short, stable filenames under Assets/.../common.
This helper maps a user's rip folder into that layout.  ISO files are copied
or renamed, while CUE/BIN rips are expanded into per-game folders as short
cue/bin pairs.  Multi-track rips are coalesced into one raw .bin so a game
instance only needs two APF data slots: slot 4 for the cue and slot 7 for
the bin.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import zipfile
from pathlib import Path


LEGACY_CD_ZIPS = {
    "Loom (USA).zip": ("loom", "loom"),
    "MIMAD.zip": ("mimad", "mimad"),
    "kq5.zip": ("kq5", "kq5"),
    "kq7.zip": ("kq7", "kq7"),
    "lsl5.zip": ("lsl5", "lsl5"),
    "lsl6.zip": ("lsl6", "lsl6"),
    "lsll4s.zip": ("lsl7", "lsl7"),
    "pqmw.zip": ("pqmw", "pqmw"),
    "sqc.zip": ("sqc", "sqc"),
}

CUE_IMAGES = {
    "Monkey Island Madness (USA).cue": ("mimad", "mimad"),
    "King's Quest V - Absence Makes the Heart Go Yonder! (USA).cue":
        ("kq5", "kq5"),
    "King's Quest VII - The Princeless Bride (USA).cue": ("kq7", "kq7"),
    "Leisure Suit Larry 5 - Passionate Patti Does a Little Undercover Work (USA).cue":
        ("lsl5", "lsl5"),
    "Leisure Suit Larry 6 - Shape Up or Slip Out! (USA).cue":
        ("lsl6", "lsl6"),
    "Leisure Suit Larry - Love for Sail! (USA).cue": ("lsl7", "lsl7"),
    "Daryl F. Gates' Police Quest Collection - The 4 Most Wanted (USA).cue":
        ("pqmw", "pqmw"),
    "Space Quest Collection (USA, Europe).cue": ("sqc", "sqc"),
}

ISO_IMAGES = {
    "ATLANTIS.iso": "atlantis.iso",
    "dott.iso": "tentacle.iso",
    "loom.iso": "loom.iso",
}


FILE_RE = re.compile(r'^\s*FILE\s+"([^"]+)"\s+\S+', re.IGNORECASE)
TRACK_RE = re.compile(r"^\s*TRACK\s+(\d+)\s+(\S+)", re.IGNORECASE)
INDEX_RE = re.compile(r"^\s*INDEX\s+(\d+)\s+(\d+):(\d+):(\d+)", re.IGNORECASE)


def frame_to_msf(frame: int) -> str:
    minute, rem = divmod(frame, 75 * 60)
    second, sector = divmod(rem, 75)
    return f"{minute:02d}:{second:02d}:{sector:02d}"


def parse_cue(text: str):
    current_file = None
    file_order: list[str] = []
    tracks = []

    for line in text.splitlines():
        m = FILE_RE.match(line)
        if m:
            current_file = m.group(1)
            if current_file not in file_order:
                file_order.append(current_file)
            continue

        m = TRACK_RE.match(line)
        if m:
            if not current_file:
                raise ValueError("TRACK before FILE in cue")
            tracks.append({
                "file": current_file,
                "number": int(m.group(1)),
                "type": m.group(2),
                "indexes": {},
            })
            continue

        m = INDEX_RE.match(line)
        if m and tracks:
            index = int(m.group(1))
            frame = (int(m.group(2)) * 60 + int(m.group(3))) * 75 + int(m.group(4))
            tracks[-1]["indexes"][index] = frame

    if not file_order or not tracks:
        raise ValueError("cue has no FILE/TRACK entries")
    return file_order, tracks


def zip_member_by_name(zf: zipfile.ZipFile, name: str) -> zipfile.ZipInfo:
    names = {info.filename.lower(): info for info in zf.infolist()}
    info = names.get(name.lower())
    if info:
        return info

    wanted_base = os.path.basename(name).lower()
    matches = [info for info in zf.infolist()
               if os.path.basename(info.filename).lower() == wanted_base]
    if len(matches) == 1:
        return matches[0]
    raise KeyError(name)


def source_file_by_name(src_dir: Path, name: str) -> Path:
    path = src_dir / name
    if path.exists():
        return path

    lowered = name.lower()
    for src in src_dir.iterdir():
        if src.is_file() and src.name.lower() == lowered:
            return src

    wanted_base = os.path.basename(name).lower()
    matches = [src for src in src_dir.iterdir()
               if src.is_file() and src.name.lower() == wanted_base]
    if len(matches) == 1:
        return matches[0]
    raise FileNotFoundError(name)


def write_coalesced_cue(cue_text: str, dst_dir: Path, folder: str, stem: str,
                        open_source, size_source) -> None:
    out_dir = dst_dir / folder
    out_dir.mkdir(parents=True, exist_ok=True)
    out_bin = out_dir / f"{stem}.bin"
    out_cue = out_dir / f"{stem}.cue"

    file_order, tracks = parse_cue(cue_text)

    file_start_frames: dict[str, int] = {}
    with out_bin.open("wb") as out:
        for cue_file in file_order:
            file_size = size_source(cue_file)
            if file_size % 2352:
                raise RuntimeError(
                    f"{cue_file}: size is not sector-aligned")
            file_start_frames[cue_file] = out.tell() // 2352
            with open_source(cue_file) as inp:
                shutil.copyfileobj(inp, out, length=1024 * 1024)

    lines = [f'FILE "{stem}.bin" BINARY']
    for track in tracks:
        lines.append(f"  TRACK {track['number']:02d} {track['type']}")
        base = file_start_frames[track["file"]]
        for index in sorted(track["indexes"]):
            lines.append(
                f"    INDEX {index:02d} "
                f"{frame_to_msf(base + track['indexes'][index])}")

    out_cue.write_text("\n".join(lines) + "\n", encoding="ascii")


def convert_cd_zip(src_zip: Path, dst_dir: Path, folder: str, stem: str) -> None:
    with zipfile.ZipFile(src_zip, "r") as zf:
        cue_infos = [info for info in zf.infolist()
                     if info.filename.lower().endswith(".cue")]
        if len(cue_infos) != 1:
            raise RuntimeError(f"{src_zip.name}: expected exactly one .cue")

        cue_text = zf.read(cue_infos[0]).decode("latin-1")

        def open_source(cue_file: str):
            return zf.open(zip_member_by_name(zf, cue_file), "r")

        def size_source(cue_file: str) -> int:
            return zip_member_by_name(zf, cue_file).file_size

        write_coalesced_cue(cue_text, dst_dir, folder, stem,
                            open_source, size_source)
    print(f"{src_zip.name} -> {folder}/{stem}.cue + {folder}/{stem}.bin")


def convert_cue_file(src_cue: Path, dst_dir: Path, folder: str, stem: str) -> None:
    cue_text = src_cue.read_text(encoding="latin-1")
    src_dir = src_cue.parent

    def open_source(cue_file: str):
        return source_file_by_name(src_dir, cue_file).open("rb")

    def size_source(cue_file: str) -> int:
        return source_file_by_name(src_dir, cue_file).stat().st_size

    write_coalesced_cue(cue_text, dst_dir, folder, stem,
                        open_source, size_source)
    print(f"{src_cue.name} -> {folder}/{stem}.cue + {folder}/{stem}.bin")


def copy_renamed_assets(src_dir: Path, dst_dir: Path) -> None:
    for src_name, dst_name in ISO_IMAGES.items():
        src = src_dir / src_name
        if not src.exists():
            print(f"missing {src_name}; skipped")
            continue
        dst = dst_dir / dst_name
        if src.resolve() == dst.resolve():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        print(f"{src.name} -> {dst_name}")


def copy_passthrough_assets(src_dir: Path, dst_dir: Path) -> None:
    converted = {name.lower() for name in LEGACY_CD_ZIPS}
    converted.update(name.lower() for name in CUE_IMAGES)
    converted.update(name.lower() for name in ISO_IMAGES)
    for src in sorted(src_dir.iterdir()):
        if not src.is_file():
            continue
        name = src.name.lower()
        if name in converted:
            continue
        if src.suffix.lower() not in {".iso", ".zip"}:
            continue
        dst = dst_dir / src.name
        if src.resolve() == dst.resolve():
            continue
        shutil.copy2(src, dst)
        print(f"copied {src.name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_common", type=Path)
    parser.add_argument("dest_common", type=Path)
    args = parser.parse_args()

    src_dir = args.source_common
    dst_dir = args.dest_common
    if not src_dir.is_dir():
        raise SystemExit(f"source is not a directory: {src_dir}")
    dst_dir.mkdir(parents=True, exist_ok=True)

    copy_renamed_assets(src_dir, dst_dir)

    for cue_name, (folder, stem) in CUE_IMAGES.items():
        src_cue = src_dir / cue_name
        if src_cue.exists():
            convert_cue_file(src_cue, dst_dir, folder, stem)
        else:
            print(f"missing {cue_name}; skipped")

    for zip_name, (folder, stem) in LEGACY_CD_ZIPS.items():
        src_zip = src_dir / zip_name
        if src_zip.exists():
            convert_cd_zip(src_zip, dst_dir, folder, stem)
        else:
            print(f"missing {zip_name}; skipped")

    copy_passthrough_assets(src_dir, dst_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

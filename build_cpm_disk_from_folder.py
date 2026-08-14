#!/usr/bin/env python3
"""Create a raw CP/M-style disk image from a folder of CP/M files.

This script writes a simple CP/M-style catalog + payload layout into the raw
128-byte-sector image format expected by cpm22_machine.h:
    80 tracks * 16 sectors * 128 bytes = 163840 bytes

It is intended as a practical next-layer bridge between the files in CPM22/
and the SD-card disk image read by the emulator.

The format is intentionally simple and deliberately does NOT try to emulate
full BDOS filesystem semantics; it simply places files into the raw disk image
in a predictable sector layout.
"""

from __future__ import annotations

import argparse
from pathlib import Path

TRACKS = 80
SECTORS_PER_TRACK = 16
SECTOR_SIZE = 128
TOTAL_SECTORS = TRACKS * SECTORS_PER_TRACK
TOTAL_BYTES = TOTAL_SECTORS * SECTOR_SIZE

# Native CP/M directory: 64 entries x 32 bytes = 2048 bytes.
DIRECTORY_ENTRIES = 64
DIRECTORY_BYTES = DIRECTORY_ENTRIES * 32
DIRECTORY_BLOCKS = 1
DATA_START_BLOCK = 2
BLOCK_SIZE = 2048
SECTORS_PER_BLOCK = BLOCK_SIZE // SECTOR_SIZE


def make_disk_image(path: Path, size: int) -> bytearray:
    image = bytearray(size)
    path.write_bytes(image)
    return image


def file_to_cpm_name(path: Path) -> tuple[str, str]:
    name = path.name.upper()
    stem, suffix = Path(name).stem.upper(), Path(name).suffix.upper().lstrip('.')
    if len(stem) > 8:
        stem = stem[:8]
    if len(suffix) > 3:
        suffix = suffix[:3]
    return stem.ljust(8, ' '), suffix.ljust(3, ' ')


def sector_index(track: int, sector: int) -> int:
    return (track * SECTORS_PER_TRACK + sector) * SECTOR_SIZE


def write_dir_entry(disk: bytearray, entry_index: int, name: str, ext: str, blocks: list[int], file_size: int) -> None:
    offset = entry_index * 32
    entry = bytearray(32)
    entry[0] = 0
    entry[1:9] = name.encode('ascii', 'ignore')[:8].ljust(8, b' ')
    entry[9:12] = ext.encode('ascii', 'ignore')[:3].ljust(3, b' ')
    entry[12] = 0
    entry[13] = 0
    entry[14] = 0
    entry[15] = min(128, (file_size + SECTOR_SIZE - 1) // SECTOR_SIZE)
    for index, block in enumerate(blocks[:16]):
        entry[16 + index] = block
    disk[offset:offset + 32] = entry


def write_file_bytes(disk: bytearray, start_sector: int, data: bytes) -> None:
    total = len(data)
    sector_count = (total + SECTOR_SIZE - 1) // SECTOR_SIZE
    for i in range(sector_count):
        sector = start_sector + i
        if sector >= TOTAL_SECTORS:
            raise ValueError(f"Disk full while writing file at sector {sector}")
        start = sector * SECTOR_SIZE
        chunk = data[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE]
        disk[start:start + len(chunk)] = chunk
        if len(chunk) < SECTOR_SIZE:
            disk[start + len(chunk):start + SECTOR_SIZE] = b'\x00' * (SECTOR_SIZE - len(chunk))


def build_disk_image(source_dir: Path, output_path: Path, drive: str, allow_all: bool = False) -> int:
    if not source_dir.exists():
        raise FileNotFoundError(f"Source directory not found: {source_dir}")

    disk = bytearray(TOTAL_BYTES)

    files = []
    for p in sorted(source_dir.iterdir()):
        if not p.is_file():
            continue
        if not allow_all:
            if p.suffix.upper() not in {'.COM', '.SYS', '.ASM', '.LIB', '.TXT', '.BAS', '.HEX', '.BIN'}:
                continue
        files.append(p)

    if not files:
        raise ValueError(f"No CP/M-like files found in {source_dir}")

    next_block = DATA_START_BLOCK
    catalog_index = 0

    for p in files:
        if catalog_index >= 64:
            raise ValueError("Catalog overflow: too many files for this disk image")

        data = p.read_bytes()
        file_size = len(data)
        block_count = max(1, (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE)
        blocks = list(range(next_block, next_block + block_count))
        if blocks[-1] * BLOCK_SIZE >= TOTAL_BYTES:
            raise ValueError("Disk full while allocating file")
        start_sector = blocks[0] * SECTORS_PER_BLOCK
        write_file_bytes(disk, start_sector, data)
        name, ext = file_to_cpm_name(p)
        write_dir_entry(disk, catalog_index, name, ext, blocks, file_size)
        next_block += block_count
        catalog_index += 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(disk)
    return len(disk)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a raw CP/M disk image from a directory of CP/M files.")
    parser.add_argument("--source", type=Path, required=True, help="Directory containing CP/M files to copy into the disk image.")
    parser.add_argument("--output", type=Path, required=True, help="Target .DSK file path.")
    parser.add_argument("--drive", choices=["A", "B"], default="A", help="Drive letter to embed in the catalog header.")
    parser.add_argument("--all-files", action="store_true", help="Include all files, not only common CP/M file extensions.")
    args = parser.parse_args()

    size = build_disk_image(args.source, args.output, args.drive, allow_all=args.all_files)
    print(f"Created {args.output} ({size} bytes) from {args.source}")
    print("Layout: 80 tracks, 16 sectors/track, 128 bytes/sector")
    print(f"Drive mapping: {args.drive.upper()} -> {args.output.name}")


if __name__ == "__main__":
    main()

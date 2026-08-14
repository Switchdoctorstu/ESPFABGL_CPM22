#!/usr/bin/env python3
"""Create raw CP/M disk images for the ESP32 emulator.

The emulated BIOS reads and writes 128-byte sectors using track/sector numbers.
This script creates the image layout expected by cpm22_machine.h:
    80 tracks * 16 sectors * 128 bytes = 163,840 bytes

Files are named A.DSK and B.DSK and are intended for the SD card root.
"""

from __future__ import annotations

import argparse
from pathlib import Path

DEFAULT_TRACKS = 80
DEFAULT_SECTORS_PER_TRACK = 16
DEFAULT_SECTOR_SIZE = 128


def make_disk_image(path: Path, tracks: int, sectors_per_track: int, sector_size: int) -> int:
    size = tracks * sectors_per_track * sector_size
    data = bytearray(size)
    path.write_bytes(data)
    return size


def main() -> None:
    parser = argparse.ArgumentParser(description="Create a raw CP/M disk image for the ESP32 emulator.")
    parser.add_argument("--drive", choices=["A", "B"], default="A", help="Drive letter to map this image to.")
    parser.add_argument("--output", type=Path, help="Output .DSK path. Defaults to ./A.DSK or ./B.DSK.")
    parser.add_argument("--tracks", type=int, default=DEFAULT_TRACKS, help=f"Tracks per disk ({DEFAULT_TRACKS} default)")
    parser.add_argument("--sectors-per-track", type=int, default=DEFAULT_SECTORS_PER_TRACK, help=f"Sectors per track ({DEFAULT_SECTORS_PER_TRACK} default)")
    parser.add_argument("--sector-size", type=int, default=DEFAULT_SECTOR_SIZE, help=f"Bytes per sector ({DEFAULT_SECTOR_SIZE} default)")
    parser.add_argument("--seed-file", type=Path, help="Optional binary file to place at the beginning of the disk image.")
    parser.add_argument("--seed-offset", type=int, default=0, help="Offset in bytes where the seed file is written.")
    args = parser.parse_args()

    output = args.output or Path(f"{args.drive}.DSK")
    output.parent.mkdir(parents=True, exist_ok=True)

    if args.tracks <= 0 or args.sectors_per_track <= 0 or args.sector_size <= 0:
        raise ValueError("track, sector, and sector-size counts must all be > 0")

    size = make_disk_image(output, args.tracks, args.sectors_per_track, args.sector_size)

    if args.seed_file:
        seed = args.seed_file.read_bytes()
        if args.seed_offset < 0 or args.seed_offset + len(seed) > size:
            raise ValueError(f"seed-file does not fit in this disk image: max size is {size} bytes")
        disk = bytearray(output.read_bytes())
        disk[args.seed_offset:args.seed_offset + len(seed)] = seed
        output.write_bytes(disk)

    print(f"Created {output} for drive {args.drive} ({size} bytes, {args.tracks} tracks, {args.sectors_per_track} sectors/track, {args.sector_size} bytes/sector)")
    print("Map this image to the SD card root as /A.DSK or /B.DSK for the emulator.")


if __name__ == "__main__":
    main()

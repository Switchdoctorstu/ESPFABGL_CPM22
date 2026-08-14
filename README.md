# ESP_FABGL CP/M 2.2 Emulator

## Status

Z80 CP/M-style emulator on ESP32 using FabGL for VGA output and PS/2 keyboard
input, with an SD card disk backend for A:/B: drives.

There is exactly one active boot path (see `setup()`/`loop()` in
ESP_FABGL.ino and `Cpm22Machine` in cpm22_machine.h):

- Z80 CPU execution is real (FabGL `emudevs/Z80`).
- BDOS (address 0005h) is **not** run as Z80 code. `Cpm22Machine` traps every
  `CALL 0005h` and implements the BDOS functions natively in C++
  (`dispatchBdosCall` in cpm22_machine.h), reading/writing the SD card disk
  images directly.
- The CCP (command processor) is a small custom Z80 program, `MINICCP.ASM`
  (see CPM22/MINICCP.ASM), assembled by build_miniccp.py into `MINICCP.COM`.
  It supports `DIR`, `TYPE`, `ERA`, `REN`, `SAVE`, `USER`, `RUN`, and direct
  program launch by typing a program name (with optional arguments) without
  needing the `RUN` keyword.
- `/MINICCP.COM` on the SD card is the single source of truth for the CCP:
  `Cpm22Machine::bootFromSd()` loads it at cold boot, and
  `reloadWarmBootImage()` reloads it from SD again on every warm boot
  (BDOS function 0), on program exit (after `RUN`), and whenever the Z80
  jumps to address 0000h. 
- If SD/`MINICCP.COM` is unavailable, PC stays at 0000h and the same warm-boot
  trap keeps retrying automatically (rate-limited logging) until it succeeds.

## Disk Backend

- SD card root holds `A.DSK` and `B.DSK`, raw images of 80 tracks x 16
  sectors x 128 bytes each, and `MINICCP.COM` (the CCP binary).
- Catalog format is standard CP/M: 64 entries x 32 bytes, byte 0 = user
  (0 = in use by this builder), bytes 1-8 = name, bytes 9-11 = extension,
  byte 15 = 128-byte record count, bytes 16+ = allocation block numbers
  (2048-byte blocks, i.e. 16 sectors/block).
- Build disk images from a folder of CP/M files with
  build_cpm_disk_from_folder.py, or create a blank/seeded image with
  build_cpm_disk_image.py.
- CPM22/ contains the classic CP/M 2.2 distribution files used to seed a
  working `A.DSK`.

## Dormant / Not Wired In

These files exist for a future genuine CCP+BDOS+BIOS boot path but are
**not** included by ESP_FABGL.ino, to keep exactly one path active:

- cpm22_bios.h: real CP/M 2.2 BIOS jump table (CBOOT/WBOOT/CONST/CONIN/
  CONOUT/HOME/SELDSK with real DPB/DPH tables/SETTRK/SETSEC/SETDMA/READ/
  WRITE/LISTST/SECTRAN), talking to the port-based disk interface (ports
  20h-28h).
- cpm22_roms.h, cpm22_system_image.h, cpm22_miniccp_image.h: CP/M system
  image constants/hooks and a generated flash copy of MINICCP.COM.
- bdos.asm (+ locations.asm, core_jump.asm): reference/scratch material for
  a full Z80 BDOS implementation.

To revive that path, re-add the includes/calls in ESP_FABGL.ino and remove
the SD-only assumption in `Cpm22Machine::bootFromSd()`.

## Preserved Asset

mcm6576_font.h is intentionally retained for future reuse of original NASCOM
character-generator glyphs; it is not currently wired into the terminal
renderer.

## Current Repository Files

- ESP_FABGL.ino: board bring-up, FabGL terminal/keyboard wiring, single
  SD-based boot path
- cpm22_machine.h: Z80 core, memory model, native BDOS trap, SD disk access,
  SD-based CCP loader/warm-boot reload
- build_miniccp.py: minimal Z80 assembler for CPM22/MINICCP.ASM
- build_cpm22_image_header.ps1: converts a binary image into a C header
  (used for the dormant flash-image path)
- build_cpm_disk_from_folder.py, build_cpm_disk_image.py: disk image builders
- CPM22/: classic CP/M 2.2 distribution sources/binaries used to seed disks
- mcm6576_font.h: preserved original character generator font dump
- mcm6576_font.h: preserved original character generator font dump

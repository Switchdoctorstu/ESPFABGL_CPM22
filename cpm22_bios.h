#pragma once

#include <stdint.h>
#include "cpm22_machine.h"
#include "cpm22_roms.h"

constexpr uint8_t CPM22_BIOS_JUMPTABLE_COUNT = 17;

// Canonical CP/M 2.2 BIOS jump table entry points.
constexpr uint16_t CPM22_BIOS_BOOT_ENTRY = CPM22_BIOS_BASE + 0x00;
constexpr uint16_t CPM22_BIOS_WBOOT_ENTRY = CPM22_BIOS_BASE + 0x03;
constexpr uint16_t CPM22_BIOS_CONST_ENTRY = CPM22_BIOS_BASE + 0x06;
constexpr uint16_t CPM22_BIOS_CONIN_ENTRY = CPM22_BIOS_BASE + 0x09;
constexpr uint16_t CPM22_BIOS_CONOUT_ENTRY = CPM22_BIOS_BASE + 0x0C;

// Disk geometry matches the raw A.DSK/B.DSK layout: 80 tracks x 16 sectors
// x 128 bytes, with the directory occupying block 0 (see
// build_cpm_disk_from_folder.py). One 2048-byte block equals one track.
constexpr uint16_t CPM22_DPB_SPT = 16;    // 128-byte records/track
constexpr uint8_t  CPM22_DPB_BSH = 4;     // block shift (2048-byte blocks)
constexpr uint8_t  CPM22_DPB_BLM = 15;    // block mask
constexpr uint8_t  CPM22_DPB_EXM = 1;     // extent mask (BLS=2048, DSM<256)
constexpr uint16_t CPM22_DPB_DSM = 79;    // max block number (80 blocks)
constexpr uint16_t CPM22_DPB_DRM = 63;    // max directory entry number
constexpr uint8_t  CPM22_DPB_AL0 = 0x80;  // block 0 reserved for directory
constexpr uint8_t  CPM22_DPB_AL1 = 0x00;
constexpr uint16_t CPM22_DPB_CKS = 0;     // fixed disk, no checksum vector
constexpr uint16_t CPM22_DPB_OFF = 0;     // no reserved system tracks

inline bool installCpm22BiosScaffold(Cpm22Machine & machine) {
  uint16_t entryTarget[CPM22_BIOS_JUMPTABLE_COUNT] = { 0 };
  uint16_t cursor = CPM22_BIOS_BASE + (uint16_t) (CPM22_BIOS_JUMPTABLE_COUNT * 3);

  auto emit = [&](uint8_t value) {
    machine.pokeMemory(cursor++, value);
  };

  auto emitWord = [&](uint16_t value) {
    emit((uint8_t) (value & 0xFF));
    emit((uint8_t) ((value >> 8) & 0xFF));
  };

  auto emitJP = [&](uint16_t addr) {
    emit(0xC3);
    emitWord(addr);
  };

  // ---- Disk Parameter Block, shared by both drives (identical geometry).
  uint16_t dpbAddr = cursor;
  emitWord(CPM22_DPB_SPT);
  emit(CPM22_DPB_BSH);
  emit(CPM22_DPB_BLM);
  emit(CPM22_DPB_EXM);
  emitWord(CPM22_DPB_DSM);
  emitWord(CPM22_DPB_DRM);
  emit(CPM22_DPB_AL0);
  emit(CPM22_DPB_AL1);
  emitWord(CPM22_DPB_CKS);
  emitWord(CPM22_DPB_OFF);

  // ---- Allocation vectors, one per drive; BDOS rebuilds contents at login.
  constexpr uint8_t ALV_SIZE = (CPM22_DPB_DSM / 8) + 1;
  uint16_t alvAAddr = cursor;
  for (uint8_t i = 0; i < ALV_SIZE; ++i)
    emit(0x00);
  uint16_t alvBAddr = cursor;
  for (uint8_t i = 0; i < ALV_SIZE; ++i)
    emit(0x00);

  // ---- Directory scratch buffer, shared (only one directory scan at a time).
  uint16_t dirbufAddr = cursor;
  for (uint8_t i = 0; i < 128; ++i)
    emit(0x00);

  // ---- Disk Parameter Headers, one per drive.
  uint16_t dphAAddr = cursor;
  emitWord(0x0000);  // XLT: no sector translation (SECTRAN is pass-through)
  emitWord(0x0000); emitWord(0x0000); emitWord(0x0000);  // BDOS scratch words
  emitWord(dirbufAddr);
  emitWord(dpbAddr);
  emitWord(0x0000);  // CSV: none (CKS=0)
  emitWord(alvAAddr);

  uint16_t dphBAddr = cursor;
  emitWord(0x0000);
  emitWord(0x0000); emitWord(0x0000); emitWord(0x0000);
  emitWord(dirbufAddr);
  emitWord(dpbAddr);
  emitWord(0x0000);
  emitWord(alvBAddr);

  // BOOT -> jump straight into CCP for now (cold start scaffold).
  entryTarget[0] = cursor;
  emitJP(CPM22_CCP_BASE);

  // WBOOT -> same as BOOT in this scaffold.
  entryTarget[1] = cursor;
  emitJP(CPM22_CCP_BASE);

  // CONST: return 0xFF when a key is waiting, 0x00 otherwise.
  entryTarget[2] = cursor;
  emit(0xDB); emit(0x00);  // IN A,(00h)
  emit(0xE6); emit(0x01);  // AND 01h
  emit(0xC8);              // RET Z
  emit(0x3E); emit(0xFF);  // LD A,FFh
  emit(0xC9);              // RET

  // CONIN: block until a key is available, then return it in A.
  entryTarget[3] = cursor;
  emit(0xDB); emit(0x00);  // IN A,(00h)
  emit(0xE6); emit(0x01);  // AND 01h
  emit(0x28); emit(0xFA);  // JR Z, back to IN A,(00h)
  emit(0xDB); emit(0x01);  // IN A,(01h)
  emit(0xC9);              // RET

  // CONOUT: write character from C via console data port.
  entryTarget[4] = cursor;
  emit(0x79);              // LD A,C
  emit(0xD3); emit(0x01);  // OUT (01h),A
  emit(0xC9);              // RET

  // LIST: mirror to console output.
  entryTarget[5] = cursor;
  emitJP(entryTarget[4]);

  // PUNCH: stub (no device attached).
  entryTarget[6] = cursor;
  emit(0xC9);              // RET

  // READER: return Ctrl+Z (logical EOF).
  entryTarget[7] = cursor;
  emit(0x3E); emit(0x1A);  // LD A,1Ah
  emit(0xC9);              // RET

  // SETTRK: track in BC, send low byte to port 22h, high byte to 23h.
  entryTarget[10] = cursor;
  emit(0x79); emit(0xD3); emit(0x22);  // LD A,C ; OUT (22h),A
  emit(0x78); emit(0xD3); emit(0x23);  // LD A,B ; OUT (23h),A
  emit(0xC9);                           // RET

  // HOME: equivalent to SETTRK(0).
  entryTarget[8] = cursor;
  emit(0x01); emitWord(0x0000);  // LD BC,0000h
  emitJP(entryTarget[10]);       // JP SETTRK

  // SELDSK support stubs, emitted first so the dispatcher below can
  // reference their addresses without needing a backpatching pass.
  uint16_t seldskInvalid = cursor;
  emit(0x21); emitWord(0x0000);  // LD HL,0000h
  emit(0xC9);                    // RET

  uint16_t seldskDriveB = cursor;
  emit(0x21); emitWord(dphBAddr);  // LD HL,DPH_B
  emit(0xC9);                      // RET

  // SELDSK: drive number in C. Selects the disk (port 21h) and returns the
  // matching Disk Parameter Header in HL, or HL=0000h for an invalid drive.
  entryTarget[9] = cursor;
  emit(0x79);              // LD A,C
  emit(0xD3); emit(0x21);  // OUT (21h),A
  emit(0xFE); emit(0x02);  // CP 2
  emit(0xD2); emitWord(seldskInvalid);  // JP NC,invalid
  emit(0xB7);              // OR A
  emit(0xC2); emitWord(seldskDriveB);   // JP NZ,driveB
  emit(0x21); emitWord(dphAAddr);       // LD HL,DPH_A
  emit(0xC9);              // RET

  // SETSEC: sector in BC, send low byte to port 24h, high byte to 25h.
  entryTarget[11] = cursor;
  emit(0x79); emit(0xD3); emit(0x24);  // LD A,C ; OUT (24h),A
  emit(0x78); emit(0xD3); emit(0x25);  // LD A,B ; OUT (25h),A
  emit(0xC9);                           // RET

  // SETDMA: DMA in BC, send low/high bytes to ports 26h/27h.
  entryTarget[12] = cursor;
  emit(0x79); emit(0xD3); emit(0x26);  // LD A,C ; OUT (26h),A
  emit(0x78); emit(0xD3); emit(0x27);  // LD A,B ; OUT (27h),A
  emit(0xC9);                           // RET

  // READ: issue read command at port 20h and return status from port 28h.
  entryTarget[13] = cursor;
  emit(0x3E); emit(0x01);  // LD A,01h
  emit(0xD3); emit(0x20);  // OUT (20h),A
  emit(0xDB); emit(0x28);  // IN A,(28h)
  emit(0xC9);              // RET

  // WRITE: issue write command at port 20h and return status from port 28h.
  entryTarget[14] = cursor;
  emit(0x3E); emit(0x02);  // LD A,02h
  emit(0xD3); emit(0x20);  // OUT (20h),A
  emit(0xDB); emit(0x28);  // IN A,(28h)
  emit(0xC9);              // RET

  // LISTST: always ready.
  entryTarget[15] = cursor;
  emit(0x3E); emit(0xFF);  // LD A,FFh
  emit(0xC9);              // RET

  // SECTRAN: pass-through translation, HL <- BC.
  entryTarget[16] = cursor;
  emit(0x60);              // LD H,B
  emit(0x69);              // LD L,C
  emit(0xC9);              // RET

  for (uint8_t i = 0; i < CPM22_BIOS_JUMPTABLE_COUNT; ++i) {
    uint16_t slot = CPM22_BIOS_BASE + (uint16_t) (i * 3);
    machine.pokeMemory(slot + 0, 0xC3);  // JP
    machine.pokeMemory(slot + 1, (uint8_t) (entryTarget[i] & 0xFF));
    machine.pokeMemory(slot + 2, (uint8_t) ((entryTarget[i] >> 8) & 0xFF));
  }

  return true;
}

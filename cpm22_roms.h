#pragma once

#include <stdint.h>
#include "cpm22_system_image.h"

// CP/M 2.2 common 60K layout (can be changed to match your BIOS build):
// TPA: 0100h-DBFFh
// CCP: DC00h
// BDOS: E400h (entry often at E406h)
// BIOS: EC00h
constexpr uint16_t CPM22_CCP_BASE = 0xDC00;
constexpr uint16_t CPM22_BDOS_BASE = 0xE400;
constexpr uint16_t CPM22_BDOS_ENTRY = 0xE406;
constexpr uint16_t CPM22_BIOS_BASE = 0xEC00;

// Minimal fallback bootstrap used when no real CP/M image is linked yet.
// It echoes characters via console ports and sits outside reset vectors.
//   IN A,(00h) -> status
//   wait until bit0 set
//   IN A,(01h)
//   OUT (01h),A
//   JP start
constexpr uint16_t CPM22_BOOTSTRAP_BASE = 0x0100;
constexpr uint8_t CPM22_BOOTSTRAP_IMAGE[] = {
  0xDB, 0x00,       // IN A,(00h)
  0xE6, 0x01,       // AND 01h
  0x28, 0xFA,       // JR Z, -6
  0xDB, 0x01,       // IN A,(01h)
  0xD3, 0x01,       // OUT (01h),A
  0xC3, 0x00, 0x00  // JP 0000h
};
constexpr uint16_t CPM22_BOOTSTRAP_SIZE = sizeof(CPM22_BOOTSTRAP_IMAGE);

static_assert(CPM22_SYSTEM_IMAGE_BASE >= CPM22_CCP_BASE,
              "CPM22_SYSTEM_IMAGE_BASE should usually be at or above CCP base");

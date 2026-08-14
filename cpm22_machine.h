#pragma once

#include <FS.h>
#include <SPI.h>
#include <SD.h>

#include "fabgl.h"
#include "emudevs/Z80.h"

constexpr int CPM_RAM_SIZE = 65536;
constexpr uint32_t CPM_EMULATED_CPU_HZ = 4000000;

constexpr uint8_t CPM22_DISK_CMD_PORT = 0x20;
constexpr uint8_t CPM22_DISK_DRIVE_PORT = 0x21;
constexpr uint8_t CPM22_DISK_TRACK_LOW_PORT = 0x22;
constexpr uint8_t CPM22_DISK_TRACK_HIGH_PORT = 0x23;
constexpr uint8_t CPM22_DISK_SECTOR_LOW_PORT = 0x24;
constexpr uint8_t CPM22_DISK_SECTOR_HIGH_PORT = 0x25;
constexpr uint8_t CPM22_DISK_DMA_LOW_PORT = 0x26;
constexpr uint8_t CPM22_DISK_DMA_HIGH_PORT = 0x27;
constexpr uint8_t CPM22_DISK_STATUS_PORT = 0x28;
constexpr uint8_t CPM22_DISK_CMD_READ = 0x01;
constexpr uint8_t CPM22_DISK_CMD_WRITE = 0x02;
constexpr bool CPM22_ENABLE_CONSOLE_TRACE = false;
constexpr bool CPM22_ENABLE_BDOS_TRACE = false;
constexpr uint8_t CPM22_BDOS_EXECUTE_COM = 60;
constexpr uint16_t CPM22_COM_RETURN_TRAMPOLINE = 0xF000;

constexpr uint16_t CPM22_BDOS_CALL_ADDRESS = 0x0005;

// Single source of truth for the CCP binary: always loaded from SD, both at
// cold boot and on every warm boot / program-exit reload.
constexpr const char * CPM22_CCP_SD_PATH = "/MINICCP.COM";
constexpr uint16_t CPM22_CCP_SD_LOAD_ADDRESS = 0x0100;
constexpr uint32_t CPM22_CCP_SD_MAX_SIZE = 0xDC00 - CPM22_CCP_SD_LOAD_ADDRESS;


class Cpm22Bus {
public:
  using IOReadHandler = int (*)(void * context, uint8_t port);
  using IOWriteHandler = void (*)(void * context, uint8_t port, uint8_t value);

  struct IOPortMeta {
    IOReadHandler read = nullptr;
    IOWriteHandler write = nullptr;
    void * context = nullptr;
  };

  Cpm22Bus() {
    clear();
  }

  void clear() {
    memset(m_memory, 0x00, sizeof(m_memory));
    memset(m_ports, 0x00, sizeof(m_ports));
  }

  bool loadBinary(uint16_t base, const uint8_t * data, size_t size) {
    if (data == nullptr || size == 0 || ((uint32_t) base + size) > 65536)
      return false;

    for (size_t i = 0; i < size; ++i)
      m_memory[base + i] = data[i];

    return true;
  }

  int readByte(uint16_t addr) const {
    return m_memory[addr];
  }

  void writeByte(uint16_t addr, uint8_t value) {
    m_memory[addr] = value;
  }

  int ioRead(uint8_t port) {
    auto const & p = m_ports[port];
    if (p.read)
      return p.read(p.context, port) & 0xFF;
    return 0xFF;
  }

  void ioWrite(uint8_t port, uint8_t value) {
    auto const & p = m_ports[port];
    if (p.write)
      p.write(p.context, port, value);
  }

  void installPort(uint8_t port, IOReadHandler read, IOWriteHandler write, void * context) {
    m_ports[port].read = read;
    m_ports[port].write = write;
    m_ports[port].context = context;
  }

private:
  uint8_t m_memory[CPM_RAM_SIZE];
  IOPortMeta m_ports[256];
};


class Cpm22Machine {
public:
  // A read-only debugger view.  The emulator and the VGA UI run from the
  // Arduino loop, so callers should take this snapshot between calls to
  // runForElapsedTime(), never from an interrupt.
  struct CpuSnapshot {
    uint16_t af;
    uint16_t bc;
    uint16_t de;
    uint16_t hl;
    uint16_t ix;
    uint16_t iy;
    uint16_t sp;
    uint16_t pc;
    int status;
  };

  static constexpr uint8_t CONSOLE_STATUS_RX_READY = 0x01;
  static constexpr uint8_t CONSOLE_STATUS_TX_READY = 0x02;

  Cpm22Machine() {
    m_cpu.setCallbacks(this, readByte, writeByte, readWord, writeWord, readIO, writeIO);
  }

  void setConsoleStream(Stream * stream) {
    m_console = stream;
  }

  void begin(uint16_t startAddress) {
    m_bus.clear();
    installDefaultConsolePorts();
    installDiskPorts();

    m_diskCommand = 0;
    m_diskDrive = 0;
    m_diskTrack = 0;
    m_diskSector = 0;
    m_diskDma = 0x0080;
    m_diskStatus = 0;
    m_bdosLineBuffer = 0;
    m_bdosLineLength = 0;

    m_cpu.reset();
    m_cpu.setPC(startAddress);
    m_lastMicros = micros();
  }

  bool loadBinary(uint16_t address, const uint8_t * data, size_t size) {
    return m_bus.loadBinary(address, data, size);
  }

  void setCpmVectors(uint16_t warmBootAddress, uint16_t bdosEntryAddress) {
    // CP/M convention:
    // 0000h -> jump to warm boot entry point
    // 0005h -> jump to BDOS entry point
    m_bus.writeByte(0x0000, 0xC3);
    m_bus.writeByte(0x0001, warmBootAddress & 0xFF);
    m_bus.writeByte(0x0002, (warmBootAddress >> 8) & 0xFF);

    m_bus.writeByte(0x0005, 0xC3);
    m_bus.writeByte(0x0006, bdosEntryAddress & 0xFF);
    m_bus.writeByte(0x0007, (bdosEntryAddress >> 8) & 0xFF);

    for (uint16_t address = 0x005C; address < 0x0080; ++address)
      m_bus.writeByte(address, 0x00);
    for (uint16_t address = 0x0080; address < 0x0100; ++address)
      m_bus.writeByte(address, 0x00);

    Serial.printf("[CPM] vectors: 0000=0x%02X%02X 0005=0x%02X%02X warm=0x%04X bdos=0x%04X\n",
                  m_bus.readByte(0x0002), m_bus.readByte(0x0001),
                  m_bus.readByte(0x0007), m_bus.readByte(0x0006),
                  warmBootAddress, bdosEntryAddress);
  }

  uint8_t peekMemory(uint16_t address) const {
    return (uint8_t) m_bus.readByte(address);
  }

  void pokeMemory(uint16_t address, uint8_t value) {
    m_bus.writeByte(address, value);
  }

  void setProgramCounter(uint16_t address) {
    m_cpu.setPC(address);
  }

  // Single source of truth for the CCP: always loaded from SD, both at
  // cold boot and on every warm boot / program-exit reload.
  bool bootFromSd() {
    return loadCcpFromSd();
  }

  uint16_t programCounter() const {
    return const_cast<fabgl::Z80 &>(m_cpu).getPC();
  }

  int cpuStatus() const {
    return const_cast<fabgl::Z80 &>(m_cpu).getStatus();
  }

  CpuSnapshot cpuSnapshot() const {
    fabgl::Z80 & cpu = const_cast<fabgl::Z80 &>(m_cpu);
    return {
      cpu.readRegWord(Z80_AF), cpu.readRegWord(Z80_BC),
      cpu.readRegWord(Z80_DE), cpu.readRegWord(Z80_HL),
      cpu.readRegWord(Z80_IX), cpu.readRegWord(Z80_IY),
      cpu.readRegWord(Z80_SP), cpu.getPC(), cpu.getStatus()
    };
  }

  void runForElapsedTime() {
    uint32_t now = micros();
    uint32_t elapsedUs = now - m_lastMicros;
    if (elapsedUs == 0)
      return;

    m_lastMicros = now;

    int32_t cycleBudget = (int32_t) ((uint64_t) elapsedUs * CPM_EMULATED_CPU_HZ / 1000000ULL);
    while (cycleBudget > 0) {
      if (m_programReturnAddress != 0 && m_cpu.getPC() == m_programReturnAddress) {
        reloadWarmBootImage();
        continue;
      }

      if (m_cpu.getPC() == 0x0000) {
        reloadWarmBootImage();
        continue;
      }

      if (m_cpu.getPC() == CPM22_BDOS_CALL_ADDRESS) {
        if (!dispatchBdosCall())
          break;
        continue;
      }

      int usedCycles = m_cpu.step();
      cycleBudget -= usedCycles;

      if (m_cpu.getStatus() == fabgl::Z80_STATUS_HALT)
        break;
    }
  }

private:
  void installDefaultConsolePorts() {
    // Port 00h: console status (bit 0 RX ready, bit 1 TX ready)
    // Port 01h: console data
    m_bus.installPort(0x00, ioReadConsoleStatus, nullptr, this);
    m_bus.installPort(0x01, ioReadConsoleData, ioWriteConsoleData, this);
  }

  void installDiskPorts() {
    m_bus.installPort(CPM22_DISK_CMD_PORT, nullptr, ioWriteDiskCommand, this);
    m_bus.installPort(CPM22_DISK_DRIVE_PORT, ioReadDiskDrive, ioWriteDiskDrive, this);
    m_bus.installPort(CPM22_DISK_TRACK_LOW_PORT, nullptr, ioWriteDiskTrackLow, this);
    m_bus.installPort(CPM22_DISK_TRACK_HIGH_PORT, nullptr, ioWriteDiskTrackHigh, this);
    m_bus.installPort(CPM22_DISK_SECTOR_LOW_PORT, nullptr, ioWriteDiskSectorLow, this);
    m_bus.installPort(CPM22_DISK_SECTOR_HIGH_PORT, nullptr, ioWriteDiskSectorHigh, this);
    m_bus.installPort(CPM22_DISK_DMA_LOW_PORT, nullptr, ioWriteDiskDmaLow, this);
    m_bus.installPort(CPM22_DISK_DMA_HIGH_PORT, nullptr, ioWriteDiskDmaHigh, this);
    m_bus.installPort(CPM22_DISK_STATUS_PORT, ioReadDiskStatus, nullptr, this);
  }

  bool dispatchBdosCall() {
    uint8_t function = m_cpu.readRegByte(Z80_C);
    uint16_t parameter = m_cpu.readRegWord(Z80_DE);

    if (CPM22_ENABLE_BDOS_TRACE && (function == 15 || function == 20))
      Serial.printf("[BDOS] fn=%u DE=0x%04X DMA=0x%04X\n", function, parameter, m_diskDma);

    switch (function) {
      case 0:
        reloadWarmBootImage();
        return true;

      case 1:
        return bdosConsoleInput();

      case 2:
        if (CPM22_ENABLE_BDOS_TRACE && m_bdosConsoleOutputTrace < 3)
          Serial.printf("[BDOS] fn=2 char=0x%02X\n", m_cpu.readRegByte(Z80_E));
        ++m_bdosConsoleOutputTrace;
        writeConsoleData(m_cpu.readRegByte(Z80_E));
        returnBdos(0);
        return true;

      case 6:
        if (m_cpu.readRegByte(Z80_E) == 0xFF) {
          if (!m_console || m_console->available() == 0)
            return false;
          m_cpu.writeRegByte(Z80_A, (uint8_t) m_console->read());
          m_cpu.writeRegByte(Z80_B, 0);
          returnBdosFromCurrentRegisters();
          return true;
        }
        writeConsoleData(m_cpu.readRegByte(Z80_E));
        returnBdos(0);
        return true;

      case 9:
        while (m_bus.readByte(parameter++) != '$')
          writeConsoleData(m_bus.readByte(parameter - 1));
        returnBdos(0);
        return true;

      case 10:
        return bdosReadConsoleBuffer(parameter);

      case 11:
        m_cpu.writeRegByte(Z80_A, (m_console && m_console->available() > 0) ? 0xFF : 0x00);
        m_cpu.writeRegByte(Z80_B, 0);
        returnBdosFromCurrentRegisters();
        return true;

      case 12:
        m_cpu.writeRegByte(Z80_A, 0x22);
        m_cpu.writeRegByte(Z80_B, 0);
        returnBdosFromCurrentRegisters();
        return true;

      case 13:
        m_diskDrive = 0;
        m_diskDma = 0x0080;
        m_userNumber = 0;
        returnBdos(0);
        return true;

      case 14:
        if (m_cpu.readRegByte(Z80_E) > 1) {
          returnBdos(0xFF);
          return true;
        }
        m_diskDrive = m_cpu.readRegByte(Z80_E);
        returnBdos(0);
        return true;

      case 25:
        m_cpu.writeRegByte(Z80_A, m_diskDrive);
        m_cpu.writeRegByte(Z80_B, 0);
        returnBdosFromCurrentRegisters();
        return true;

      case 15:
        copySearchPattern(parameter);
        if (!bdosSearchDirectory(parameter, true)) {
          Serial.printf("[BDOS] open failed FCB=%02X ", parameter);
          for (uint8_t i = 1; i <= 11; ++i)
            Serial.printf("%02X ", m_bus.readByte(parameter + i));
          Serial.println();
          returnBdos(0xFF);
        } else {
          returnBdos(0x00);
        }
        return true;

      case 16:
        returnBdos(0);
        return true;

      case 17:
        m_bdosSearchIndex = 0;
        copySearchPattern(parameter);
        returnBdos(bdosSearchDirectory(parameter, false) ? 0x00 : 0xFF);
        return true;

      case 18:
        returnBdos(bdosSearchDirectory(parameter, false) ? 0x00 : 0xFF);
        return true;

      case 19:
        copySearchPattern(parameter);
        returnBdos(bdosDeleteFile(parameter) ? 0x00 : 0xFF);
        return true;

      case 20:
        returnBdos(bdosReadSequential(parameter) ? 0x00 : 0x01);
        return true;

      case 21:
        returnBdos(bdosWriteSequential(parameter) ? 0x00 : 0xFF);
        return true;

      case 22:
        returnBdos(bdosMakeFile(parameter) ? 0x00 : 0xFF);
        return true;

      case 23:
        returnBdos(bdosRenameFile(parameter) ? 0x00 : 0xFF);
        return true;

      case CPM22_BDOS_EXECUTE_COM:
        bdosExecuteCom(parameter);
        return true;

      case 26:
        m_diskDma = parameter;
        returnBdos(0);
        return true;

      case 33:
        returnBdos(bdosRandomRead(parameter) ? 0x00 : 0x01);
        return true;

      case 34:
        returnBdos(bdosRandomWrite(parameter) ? 0x00 : 0xFF);
        return true;

      case 35:
        returnBdos(bdosComputeFileSize(parameter) ? 0x00 : 0xFF);
        return true;

      case 36:
        bdosSetRandomRecord(parameter);
        return true;

      case 32:
        if (m_cpu.readRegByte(Z80_E) == 0xFF) {
          m_cpu.writeRegByte(Z80_A, m_userNumber);
        } else {
          m_userNumber = m_cpu.readRegByte(Z80_E) & 0x0F;
          m_cpu.writeRegByte(Z80_A, 0);
        }
        m_cpu.writeRegByte(Z80_B, 0);
        returnBdosFromCurrentRegisters();
        return true;

      default:
        Serial.printf("[BDOS] unsupported function=%u DE=0x%04X\n", function, parameter);
        returnBdos(0xFF);
        return true;
    }
  }

  bool bdosConsoleInput() {
    if (!m_console || m_console->available() == 0)
      return false;

    int value = m_console->read();
    if (value < 0)
      return false;

    writeConsoleData((uint8_t) value);
    m_cpu.writeRegByte(Z80_A, (uint8_t) value);
    m_cpu.writeRegByte(Z80_B, 0);
    returnBdosFromCurrentRegisters();
    return true;
  }

  bool bdosReadConsoleBuffer(uint16_t address) {
    uint8_t maximum = m_bus.readByte(address);
    if (m_bdosLineBuffer != address) {
      m_bdosLineBuffer = address;
      m_bdosLineLength = 0;
      m_bus.writeByte(address + 1, 0);
    }

    if (!m_console || m_console->available() == 0)
      return false;

    int value = m_console->read();
    if (value < 0)
      return false;

    uint8_t character = (uint8_t) value;
    if (character == '\r' || character == '\n') {
      writeConsoleData('\r');
      m_bus.writeByte(address + 1, m_bdosLineLength);
      m_bus.writeByte(address + 2 + m_bdosLineLength, 0);
      m_bdosLineBuffer = 0;
      m_bdosLineLength = 0;
      returnBdos(0);
      return true;
    }

    if (character == 8) {
      if (m_bdosLineLength > 0) {
        --m_bdosLineLength;
        writeConsoleData(8);
        writeConsoleData(' ');
        writeConsoleData(8);
      }
      return true;
    }

    if (m_bdosLineLength < maximum) {
      m_bus.writeByte(address + 2 + m_bdosLineLength, character);
      ++m_bdosLineLength;
      writeConsoleData(character);
    }
    return true;
  }

  void copySearchPattern(uint16_t fcbAddress) {
    for (uint8_t i = 0; i < 12; ++i)
      m_bdosSearchPattern[i] = m_bus.readByte(fcbAddress + i);
  }

  bool bdosSearchDirectory(uint16_t fcbAddress, bool resetSearch) {
    if (resetSearch)
      m_bdosSearchIndex = 0;

    uint8_t drive = m_bdosSearchPattern[0];
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    const char * path = diskImagePathForDrive(drive - 1);
    File image = SD.open(path, FILE_READ);
    if (!image)
      return false;

    constexpr uint16_t CATALOG_ENTRY_SIZE = 32;
    constexpr uint8_t CATALOG_ENTRY_COUNT = 64;
    bool found = false;
    uint8_t catalogEntry[CATALOG_ENTRY_SIZE];
    uint8_t cpmEntry[CATALOG_ENTRY_SIZE] = { 0 };

    while (m_bdosSearchIndex < CATALOG_ENTRY_COUNT) {
      uint8_t index = m_bdosSearchIndex++;
      if (!image.seek((uint32_t) index * CATALOG_ENTRY_SIZE))
        break;
      if (image.read(catalogEntry, CATALOG_ENTRY_SIZE) != CATALOG_ENTRY_SIZE)
        break;
      if (catalogEntryFree(catalogEntry))
        continue;
      if (catalogEntry[0] != m_userNumber)
        continue;

      if (!cpmFcbNameMatches(fcbAddress, catalogEntryName(catalogEntry)))
        continue;

      if (catalogEntry[0] == 0) {
        memcpy(cpmEntry, catalogEntry, CATALOG_ENTRY_SIZE);
      } else {
        cpmEntry[0] = 0;
        memcpy(cpmEntry + 1, catalogEntry, 11);
        cpmEntry[12] = 0;
        cpmEntry[13] = 0;
        cpmEntry[14] = 0;
        uint32_t fileSize = catalogEntryFileSize(catalogEntry);
        cpmEntry[15] = (uint8_t) min((uint32_t) 127, (fileSize + 127) / 128);
        memcpy(cpmEntry + 16, catalogEntry + 15, 2);
        for (uint8_t i = 18; i < CATALOG_ENTRY_SIZE; ++i)
          cpmEntry[i] = 0;
      }

      for (uint8_t i = 0; i < CATALOG_ENTRY_SIZE; ++i)
        m_bus.writeByte(m_diskDma + i, cpmEntry[i]);
      found = true;
      break;
    }

    image.close();
    return found;
  }

  static bool cpmNameMatches(const uint8_t * pattern, const uint8_t * name, uint8_t length) {
    for (uint8_t i = 0; i < length; ++i) {
      uint8_t requested = pattern[i] & 0x7F;
      uint8_t actual = name[i] & 0x7F;
      if (requested == '*')
        return true;
      if (requested == '?')
        continue;
      if (requested != actual)
        return false;
    }
    return true;
  }

  static bool catalogEntryFree(const uint8_t * entry) {
    return entry[0] == 0xE5 || (entry[0] == 0 && entry[1] == 0);
  }

  static const uint8_t * catalogEntryName(const uint8_t * entry) {
    return (entry[0] == 0) ? entry + 1 : entry;
  }

  static uint16_t catalogEntryStartSector(const uint8_t * entry) {
    if (entry[0] == 0)
      return (uint16_t) entry[16] * 16U;
    return (uint16_t) entry[15] | ((uint16_t) entry[16] << 8);
  }

  static uint32_t catalogEntryFileSize(const uint8_t * entry) {
    if (entry[0] == 0)
      return (uint32_t) entry[15] * 128UL;
    return (uint32_t) entry[17] | ((uint32_t) entry[18] << 8);
  }

  bool findCatalogEntry(uint8_t drive, uint16_t fcbAddress, uint8_t * entry, uint8_t * index) {
    File image = SD.open(diskImagePathForDrive(drive), FILE_READ);
    if (!image)
      return false;

    for (uint8_t candidate = 0; candidate < 64; ++candidate) {
      if (!image.seek((uint32_t) candidate * 32) || image.read(entry, 32) != 32)
        break;
      if (catalogEntryFree(entry))
        continue;
      if (entry[0] != m_userNumber)
        continue;
      bool nameMatches = cpmFcbNameMatches(fcbAddress, catalogEntryName(entry));
      if (nameMatches) {
        *index = candidate;
        image.close();
        return true;
      }
    }

    image.close();
    return false;
  }

  bool bdosDeleteFile(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    uint8_t entry[32];
    uint8_t entryIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, entry, &entryIndex))
      return false;

    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_WRITE);
    if (!image || !image.seek((uint32_t) entryIndex * 32)) {
      if (image)
        image.close();
      return false;
    }

    uint8_t deleted = 0xE5;
    bool ok = image.write(&deleted, 1) == 1;
    image.close();
    return ok;
  }

  bool bdosMakeFile(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    uint8_t existing[32];
    uint8_t existingIndex = 0;
    if (findCatalogEntry(drive - 1, fcbAddress, existing, &existingIndex))
      return false;

    int freeIndex = findFreeCatalogIndex(drive - 1);
    if (freeIndex < 0)
      return false;

    uint16_t startSector = findNextDataSector(drive - 1);
    if (startSector >= 80 * 16)
      return false;

    uint8_t entry[32] = { 0 };
    entry[0] = m_userNumber;
    for (uint8_t i = 0; i < 11; ++i)
      entry[i + 1] = m_bus.readByte(fcbAddress + 1 + i);
    entry[15] = 0;
    entry[16] = startSector / 16;

    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_WRITE);
    if (!image || !image.seek((uint32_t) freeIndex * 32)) {
      if (image)
        image.close();
      return false;
    }
    bool ok = image.write(entry, sizeof(entry)) == sizeof(entry);
    image.close();
    return ok;
  }

  bool bdosRenameFile(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    uint8_t source[32];
    uint8_t sourceIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, source, &sourceIndex))
      return false;

    uint8_t nameOffset = source[0] == 0 ? 1 : 0;
    uint8_t targetOffset = source[0] == 0 ? 17 : 16;
    for (uint8_t i = 0; i < 11; ++i)
      source[nameOffset + i] = m_bus.readByte(fcbAddress + targetOffset + i);

    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_WRITE);
    if (!image || !image.seek((uint32_t) sourceIndex * 32)) {
      if (image)
        image.close();
      return false;
    }
    bool ok = image.write(source, sizeof(source)) == sizeof(source);
    image.close();
    return ok;
  }

  int findFreeCatalogIndex(uint8_t drive) {
    File image = SD.open(diskImagePathForDrive(drive), FILE_READ);
    if (!image)
      return -1;

    uint8_t entry[32];
    for (uint8_t index = 0; index < 64; ++index) {
      if (!image.seek((uint32_t) index * 32) || image.read(entry, sizeof(entry)) != sizeof(entry))
        break;
      if (entry[0] == 0) {
        image.close();
        return index;
      }
    }
    image.close();
    return -1;
  }

  uint16_t findNextDataSector(uint8_t drive) {
    File image = SD.open(diskImagePathForDrive(drive), FILE_READ);
    if (!image)
      return 16;

    uint16_t nextSector = 16;
    uint8_t entry[32];
    for (uint8_t index = 0; index < 64; ++index) {
      if (!image.seek((uint32_t) index * 32) || image.read(entry, sizeof(entry)) != sizeof(entry))
        break;
      if (catalogEntryFree(entry))
        continue;
      uint16_t start = catalogEntryStartSector(entry);
      uint32_t size = catalogEntryFileSize(entry);
      uint16_t end = start + max((uint32_t) 16, ((size + 2047) / 2048) * 16);
      if (end > nextSector)
        nextSector = end;
    }
    image.close();
    return nextSector;
  }

  void bdosExecuteCom(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      returnBdos(0xFF);

    uint8_t entry[32];
    uint8_t entryIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, entry, &entryIndex)) {
      returnBdos(0xFF);
      return;
    }

    uint32_t startSector = catalogEntryStartSector(entry);
    uint32_t fileSize = catalogEntryFileSize(entry);
    constexpr uint16_t COM_ADDRESS = 0x0100;
    constexpr uint32_t COM_MAX_SIZE = 0xDC00 - COM_ADDRESS;
    if (fileSize == 0 || fileSize > COM_MAX_SIZE) {
      returnBdos(0xFF);
      return;
    }

    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_READ);
    if (!image || !image.seek(startSector * 128UL)) {
      if (image)
        image.close();
      returnBdos(0xFF);
      return;
    }

    uint8_t buffer[128];
    uint16_t address = COM_ADDRESS;
    uint32_t remaining = fileSize;
    bool ok = true;
    while (remaining > 0) {
      size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
      if (image.read(buffer, chunk) != chunk || !m_bus.loadBinary(address, buffer, chunk)) {
        ok = false;
        break;
      }
      address += chunk;
      remaining -= chunk;
    }
    image.close();
    if (!ok) {
      returnBdos(0xFF);
      return;
    }

    uint16_t stack = m_cpu.readRegWord(Z80_SP);
    m_bus.writeByte(stack, CPM22_COM_RETURN_TRAMPOLINE & 0xFF);
    m_bus.writeByte(stack + 1, CPM22_COM_RETURN_TRAMPOLINE >> 8);
    m_programReturnAddress = CPM22_COM_RETURN_TRAMPOLINE;
    m_cpu.setPC(COM_ADDRESS);
  }

  void reloadWarmBootImage() {
    if (loadCcpFromSd()) {
      m_bdosLineBuffer = 0;
      m_bdosLineLength = 0;
      m_diskDrive = 0;
      m_diskDma = 0x0080;
      m_bdosSearchIndex = 0;
      m_userNumber = 0;
      m_programReturnAddress = 0;
      m_cpu.setPC(CPM22_CCP_SD_LOAD_ADDRESS);
    } else {
      // Keep PC at 0000h so the next emulated cycle retries the SD load
      // (e.g. once the card is reinserted), rate-limiting the log noise.
      uint32_t nowMs = millis();
      if (nowMs - m_lastCcpLoadFailureMs > 2000) {
        Serial.println("[CPM] MINICCP.COM unavailable; retrying from SD");
        m_lastCcpLoadFailureMs = nowMs;
      }
      m_programReturnAddress = 0;
      m_cpu.setPC(0x0000);
    }
  }

  bool loadCcpFromSd() {
    File program = SD.open(CPM22_CCP_SD_PATH, FILE_READ);
    if (!program)
      return false;

    size_t size = program.size();
    if (size == 0 || size > CPM22_CCP_SD_MAX_SIZE) {
      program.close();
      return false;
    }

    uint8_t buffer[128];
    uint16_t address = CPM22_CCP_SD_LOAD_ADDRESS;
    size_t remaining = size;
    bool ok = true;
    while (remaining > 0) {
      size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
      if (program.read(buffer, chunk) != chunk || !m_bus.loadBinary(address, buffer, chunk)) {
        ok = false;
        break;
      }
      address += chunk;
      remaining -= chunk;
    }
    program.close();
    return ok;
  }

  bool cpmFcbNameMatches(uint16_t fcbAddress, const uint8_t * entry) const {
    bool compactName = false;
    for (uint8_t i = 1; i < 12; ++i) {
      if (m_bus.readByte(fcbAddress + i) == '.') {
        compactName = true;
        break;
      }
    }

    if (!compactName) {
      for (uint8_t i = 0; i < 11; ++i) {
        uint8_t requested = m_bus.readByte(fcbAddress + 1 + i) & 0x7F;
        uint8_t actual = entry[i] & 0x7F;
        if (requested == 0 || requested == ' ' || requested == '?')
          continue;
        if (requested == '*')
          return true;
        if (requested != actual)
          return false;
      }
      return true;
    }

    uint8_t fcbOffset = 1;
    bool nameEnded = false;
    for (uint8_t entryOffset = 0; entryOffset < 8; ++entryOffset) {
      uint8_t requested = ' ';
      if (!nameEnded) {
        requested = m_bus.readByte(fcbAddress + fcbOffset) & 0x7F;
        if (requested == '.' || requested == 0) {
          nameEnded = true;
          requested = ' ';
        } else {
          ++fcbOffset;
        }
      }
      uint8_t actual = entry[entryOffset] & 0x7F;
      if (requested != ' ' && requested != '?' && requested != actual)
        return false;
    }

    while (m_bus.readByte(fcbAddress + fcbOffset) != '.' &&
           m_bus.readByte(fcbAddress + fcbOffset) != 0 &&
           fcbOffset < 12)
      ++fcbOffset;
    if (m_bus.readByte(fcbAddress + fcbOffset) == '.')
      ++fcbOffset;

    for (uint8_t entryOffset = 8; entryOffset < 11; ++entryOffset) {
      uint8_t requested = m_bus.readByte(fcbAddress + fcbOffset++) & 0x7F;
      uint8_t actual = entry[entryOffset] & 0x7F;
      if (requested == 0 || requested == 13 || requested == ' ' || requested == '?')
        continue;
      if (requested != actual)
        return false;
    }
    return true;
  }

  uint32_t fcbRecordNumber(uint16_t fcbAddress) const {
    uint32_t extent = (uint32_t) m_bus.readByte(fcbAddress + 12) |
                      ((uint32_t) m_bus.readByte(fcbAddress + 14) << 8);
    return extent * 128UL + m_bus.readByte(fcbAddress + 32);
  }

  uint32_t fcbRandomRecord(uint16_t fcbAddress) const {
    return (uint32_t) m_bus.readByte(fcbAddress + 33) |
           ((uint32_t) m_bus.readByte(fcbAddress + 34) << 8) |
           ((uint32_t) m_bus.readByte(fcbAddress + 35) << 16);
  }

  void setFcbRandomRecord(uint16_t fcbAddress, uint32_t record) {
    m_bus.writeByte(fcbAddress + 33, record & 0xFF);
    m_bus.writeByte(fcbAddress + 34, (record >> 8) & 0xFF);
    m_bus.writeByte(fcbAddress + 35, (record >> 16) & 0xFF);
  }

  bool catalogCapacity(uint8_t drive, uint8_t entryIndex, uint16_t startSector, uint32_t requiredBytes) {
    uint16_t nextSector = 80 * 16;
    File image = SD.open(diskImagePathForDrive(drive), FILE_READ);
    if (!image)
      return false;

    uint8_t entry[32];
    for (uint8_t index = 0; index < 64; ++index) {
      if (index == entryIndex)
        continue;
      if (!image.seek((uint32_t) index * 32) || image.read(entry, sizeof(entry)) != sizeof(entry))
        break;
      if (entry[0] == 0)
        continue;
      uint16_t candidate = catalogEntryStartSector(entry);
      if (candidate > startSector && candidate < nextSector)
        nextSector = candidate;
    }
    image.close();
    return requiredBytes <= (uint32_t) (nextSector - startSector) * 128UL;
  }

  bool bdosRandomRead(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    uint8_t entry[32];
    uint8_t entryIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, entry, &entryIndex))
      return false;

    uint32_t record = fcbRandomRecord(fcbAddress);
    uint32_t fileSize = catalogEntryFileSize(entry);
    uint32_t offset = record * 128UL;
    if (offset >= fileSize)
      return false;

    uint32_t startSector = catalogEntryStartSector(entry);
    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_READ);
    if (!image || !image.seek(startSector * 128UL + offset)) {
      if (image)
        image.close();
      return false;
    }
    uint8_t buffer[128] = { 0 };
    size_t count = image.read(buffer, sizeof(buffer));
    image.close();
    if (count == 0)
      return false;
    for (uint16_t i = 0; i < sizeof(buffer); ++i)
      m_bus.writeByte(m_diskDma + i, buffer[i]);
    return true;
  }

  bool bdosRandomWrite(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    uint8_t entry[32];
    uint8_t entryIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, entry, &entryIndex))
      return false;

    uint32_t record = fcbRandomRecord(fcbAddress);
    uint32_t offset = record * 128UL;
    if (!catalogCapacity(drive - 1, entryIndex,
                         catalogEntryStartSector(entry),
                         offset + 128UL))
      return false;

    uint32_t startSector = catalogEntryStartSector(entry);
    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_WRITE);
    if (!image || !image.seek(startSector * 128UL + offset)) {
      if (image)
        image.close();
      return false;
    }
    uint8_t buffer[128];
    for (uint16_t i = 0; i < sizeof(buffer); ++i)
      buffer[i] = m_bus.readByte(m_diskDma + i);
    bool ok = image.write(buffer, sizeof(buffer)) == sizeof(buffer);
    image.close();
    if (!ok)
      return false;

    uint32_t oldSize = catalogEntryFileSize(entry);
    uint32_t newSize = offset + sizeof(buffer);
    if (newSize > oldSize)
      return updateCatalogFileSize(drive - 1, entryIndex, newSize);
    return true;
  }

  bool updateCatalogFileSize(uint8_t drive, uint8_t entryIndex, uint32_t size) {
    if (size > 128UL * 128UL)
      return false;
    File image = SD.open(diskImagePathForDrive(drive), FILE_WRITE);
    if (!image || !image.seek((uint32_t) entryIndex * 32 + 15)) {
      if (image)
        image.close();
      return false;
    }
    uint8_t records = (uint8_t) min((uint32_t) 128, (size + 127) / 128);
    bool ok = image.write(&records, 1) == 1;
    image.close();
    return ok;
  }

  bool bdosComputeFileSize(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;
    uint8_t entry[32];
    uint8_t entryIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, entry, &entryIndex))
      return false;
    uint32_t size = catalogEntryFileSize(entry);
    setFcbRandomRecord(fcbAddress, (size + 127) / 128);
    return true;
  }

  void bdosSetRandomRecord(uint16_t fcbAddress) {
    setFcbRandomRecord(fcbAddress, fcbRecordNumber(fcbAddress));
    returnBdos(0);
  }

  void advanceFcbRecord(uint16_t fcbAddress) {
    uint8_t record = m_bus.readByte(fcbAddress + 32);
    if (record != 127) {
      m_bus.writeByte(fcbAddress + 32, record + 1);
      return;
    }

    m_bus.writeByte(fcbAddress + 32, 0);
    uint8_t extent = m_bus.readByte(fcbAddress + 12);
    if (extent != 255) {
      m_bus.writeByte(fcbAddress + 12, extent + 1);
      return;
    }

    m_bus.writeByte(fcbAddress + 12, 0);
    m_bus.writeByte(fcbAddress + 14, m_bus.readByte(fcbAddress + 14) + 1);
  }

  bool bdosReadSequential(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    uint8_t entry[32];
    uint8_t entryIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, entry, &entryIndex)) {
      Serial.println("[BDOS] sequential read: file lookup failed");
      return false;
    }

    uint32_t record = fcbRecordNumber(fcbAddress);
    uint32_t fileSize = catalogEntryFileSize(entry);
    uint32_t offset = record * 128UL;
    if (offset >= fileSize) {
      Serial.printf("[BDOS] sequential read: EOF record=%lu size=%lu\n",
                    (unsigned long) record,
                    (unsigned long) fileSize);
      return false;
    }

    uint32_t startSector = catalogEntryStartSector(entry);
    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_READ);
    if (!image || !image.seek(startSector * 128UL + offset)) {
      if (image)
        image.close();
      return false;
    }

    uint8_t buffer[128] = { 0 };
    size_t count = image.read(buffer, sizeof(buffer));
    image.close();
    if (count == 0)
      return false;

    for (uint16_t i = 0; i < sizeof(buffer); ++i)
      m_bus.writeByte(m_diskDma + i, buffer[i]);
    advanceFcbRecord(fcbAddress);
    return true;
  }

  bool bdosWriteSequential(uint16_t fcbAddress) {
    uint8_t drive = m_bus.readByte(fcbAddress);
    if (drive == 0)
      drive = m_diskDrive + 1;
    if (drive < 1 || drive > 2)
      return false;

    uint8_t entry[32];
    uint8_t entryIndex = 0;
    if (!findCatalogEntry(drive - 1, fcbAddress, entry, &entryIndex))
      return false;

    uint32_t record = fcbRecordNumber(fcbAddress);
    uint32_t offset = record * 128UL;
    uint32_t startSector = catalogEntryStartSector(entry);
    File image = SD.open(diskImagePathForDrive(drive - 1), FILE_WRITE);
    if (!image || !image.seek(startSector * 128UL + offset)) {
      if (image)
        image.close();
      return false;
    }

    uint8_t buffer[128];
    for (uint16_t i = 0; i < sizeof(buffer); ++i)
      buffer[i] = m_bus.readByte(m_diskDma + i);
    bool ok = image.write(buffer, sizeof(buffer)) == sizeof(buffer);
    image.close();
    if (!ok)
      return false;

    uint32_t newSize = offset + sizeof(buffer);
    uint32_t oldSize = catalogEntryFileSize(entry);
    if (newSize > oldSize) {
      File catalog = SD.open(diskImagePathForDrive(drive - 1), FILE_WRITE);
      if (!catalog || !catalog.seek((uint32_t) entryIndex * 32 + 17)) {
        if (catalog)
          catalog.close();
        return false;
      }
      uint8_t sizeBytes[2] = { (uint8_t) newSize, (uint8_t) (newSize >> 8) };
      bool sizeOk = catalog.write(sizeBytes, sizeof(sizeBytes)) == sizeof(sizeBytes);
      catalog.close();
      if (!sizeOk)
        return false;
    }

    advanceFcbRecord(fcbAddress);
    return true;
  }

  void returnBdos(uint8_t value) {
    m_cpu.writeRegByte(Z80_A, value);
    m_cpu.writeRegByte(Z80_B, 0);
    returnBdosFromCurrentRegisters();
  }

  void returnBdosFromCurrentRegisters() {
    uint16_t stack = m_cpu.readRegWord(Z80_SP);
    uint16_t returnAddress = (uint16_t) m_bus.readByte(stack) |
                             ((uint16_t) m_bus.readByte(stack + 1) << 8);
    m_cpu.writeRegWord(Z80_SP, stack + 2);
    m_cpu.setPC(returnAddress);
  }

  static const char * diskImagePathForDrive(uint8_t drive) {
    return (drive == 1) ? "/B.DSK" : "/A.DSK";
  }

  bool ensureDiskFileExists(uint8_t drive) {
    const char * path = diskImagePathForDrive(drive);
    if (SD.exists(path))
      return true;

    File image = SD.open(path, FILE_WRITE);
    if (!image)
      return false;

    const uint32_t diskSize = 80 * 16 * 128;
    uint8_t zero = 0;
    for (uint32_t i = 0; i < diskSize; ++i)
      image.write(&zero, 1);

    image.close();
    return SD.exists(path);
  }

  bool performDiskTransaction() {
    if (!SD.cardType()) {
      m_diskStatus = 1;
      return false;
    }

    uint8_t drive = m_diskDrive & 0x01;
    if (!ensureDiskFileExists(drive)) {
      m_diskStatus = 1;
      return false;
    }

    File image = SD.open(diskImagePathForDrive(drive), FILE_READ);
    if (!image) {
      m_diskStatus = 1;
      return false;
    }

    uint32_t sectorOffset = ((uint32_t) m_diskTrack * 16UL + (uint32_t) m_diskSector) * 128UL;
    uint32_t requiredSize = sectorOffset + 128UL;

    if (image.size() < requiredSize) {
      image.close();
      File expand = SD.open(diskImagePathForDrive(drive), FILE_WRITE);
      if (!expand) {
        m_diskStatus = 1;
        return false;
      }

      expand.seek(image.size());
      uint8_t zero = 0;
      while (expand.position() < requiredSize) {
        expand.write(&zero, 1);
      }
      expand.close();

      image = SD.open(diskImagePathForDrive(drive), FILE_READ);
      if (!image) {
        m_diskStatus = 1;
        return false;
      }
    }

    image.seek(sectorOffset);
    uint8_t sector[128];
    bool ok = true;

    if (m_diskCommand == CPM22_DISK_CMD_READ) {
      size_t count = image.read(sector, sizeof(sector));
      ok = (count == sizeof(sector));
      if (ok) {
        for (size_t i = 0; i < sizeof(sector); ++i)
          m_bus.writeByte(m_diskDma + i, sector[i]);
      }
    } else if (m_diskCommand == CPM22_DISK_CMD_WRITE) {
      for (size_t i = 0; i < sizeof(sector); ++i)
        sector[i] = m_bus.readByte(m_diskDma + i);
      image.seek(sectorOffset);
      size_t count = image.write(sector, sizeof(sector));
      ok = (count == sizeof(sector));
    } else {
      ok = false;
    }

    image.close();
    m_diskStatus = ok ? 0 : 1;
    Serial.printf("[DISK] result=%s status=%u\n", ok ? "OK" : "FAIL", m_diskStatus);
    return ok;
  }

  uint8_t readConsoleStatus() const {
    uint8_t status = CONSOLE_STATUS_TX_READY;
    if (m_console && m_console->available() > 0)
      status |= CONSOLE_STATUS_RX_READY;
    return status;
  }

  uint8_t readConsoleData() {
    if (!m_console) {
      Serial.println("[INPUT] no terminal stream attached");
      return 0x00;
    }

    int c = m_console->read();
    if (c < 0) {
      return 0x00;
    }

    if (CPM22_ENABLE_CONSOLE_TRACE)
      Serial.printf("[INPUT] stream_read=0x%02X '%c'\n",
                    (uint8_t) c,
                    ((uint8_t) c >= 32 && (uint8_t) c < 127) ? (char) (uint8_t) c : '.');

    // Convert CRLF/LF to CP/M-style CR.
    if ((uint8_t) c == '\n')
      return '\r';

    return (uint8_t) c;
  }

  void writeConsoleData(uint8_t value) {
    if (!m_console)
      return;

    if (value == 0x00) {
      ++m_suppressedNullWrites;
    } else {
      if (CPM22_ENABLE_CONSOLE_TRACE && m_suppressedNullWrites > 0) {
        Serial.printf("[INPUT] suppressed %lu null writes\n",
                      (unsigned long) m_suppressedNullWrites);
      }
      m_suppressedNullWrites = 0;

      if (CPM22_ENABLE_CONSOLE_TRACE)
        Serial.printf("[INPUT] stream_write=0x%02X '%c'\n",
                      value,
                      ((value >= 32) && (value < 127)) ? (char) value : '.');
    }

    if (value == '\r') {
      m_console->write('\r');
      m_console->write('\n');
      return;
    }

    m_console->write(value);
  }

  static int readByte(void * context, int addr) {
    auto m = (Cpm22Machine *) context;
    return m->m_bus.readByte(addr & 0xFFFF);
  }

  static void writeByte(void * context, int addr, int value) {
    auto m = (Cpm22Machine *) context;
    m->m_bus.writeByte(addr & 0xFFFF, value & 0xFF);
  }

  static int readWord(void * context, int addr) {
    int lo = readByte(context, addr);
    int hi = readByte(context, addr + 1);
    return lo | (hi << 8);
  }

  static void writeWord(void * context, int addr, int value) {
    writeByte(context, addr, value & 0xFF);
    writeByte(context, addr + 1, (value >> 8) & 0xFF);
  }

  static int readIO(void * context, int addr) {
    auto m = (Cpm22Machine *) context;
    return m->m_bus.ioRead(addr & 0xFF);
  }

  static void writeIO(void * context, int addr, int value) {
    auto m = (Cpm22Machine *) context;
    m->m_bus.ioWrite(addr & 0xFF, value & 0xFF);
  }

  static int ioReadConsoleStatus(void * context, uint8_t) {
    return ((Cpm22Machine *) context)->readConsoleStatus();
  }

  static int ioReadConsoleData(void * context, uint8_t) {
    return ((Cpm22Machine *) context)->readConsoleData();
  }

  static void ioWriteConsoleData(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->writeConsoleData(value);
  }

  static void ioWriteDiskCommand(void * context, uint8_t port, uint8_t value) {
    auto m = (Cpm22Machine *) context;
    m->m_diskCommand = value;
    Serial.printf("[DISK] cmd=0x%02X drive=%u track=%u sector=%u dma=0x%04X\n",
                  m->m_diskCommand,
                  m->m_diskDrive,
                  m->m_diskTrack,
                  m->m_diskSector,
                  m->m_diskDma);
    if (m->m_diskCommand == CPM22_DISK_CMD_READ || m->m_diskCommand == CPM22_DISK_CMD_WRITE)
      m->performDiskTransaction();
  }

  static void ioWriteDiskDrive(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->m_diskDrive = value;
  }

  static void ioWriteDiskTrackLow(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->m_diskTrack = (uint16_t) ((uint16_t) ((Cpm22Machine *) context)->m_diskTrack & 0xFF00) | value;
  }

  static void ioWriteDiskTrackHigh(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->m_diskTrack = (uint16_t) ((uint16_t) ((Cpm22Machine *) context)->m_diskTrack & 0x00FF) | ((uint16_t) value << 8);
  }

  static void ioWriteDiskSectorLow(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->m_diskSector = (uint16_t) ((uint16_t) ((Cpm22Machine *) context)->m_diskSector & 0xFF00) | value;
  }

  static void ioWriteDiskSectorHigh(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->m_diskSector = (uint16_t) ((uint16_t) ((Cpm22Machine *) context)->m_diskSector & 0x00FF) | ((uint16_t) value << 8);
  }

  static void ioWriteDiskDmaLow(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->m_diskDma = (uint16_t) ((uint16_t) ((Cpm22Machine *) context)->m_diskDma & 0xFF00) | value;
  }

  static void ioWriteDiskDmaHigh(void * context, uint8_t, uint8_t value) {
    ((Cpm22Machine *) context)->m_diskDma = (uint16_t) ((uint16_t) ((Cpm22Machine *) context)->m_diskDma & 0x00FF) | ((uint16_t) value << 8);
  }

  static int ioReadDiskDrive(void * context, uint8_t) {
    return ((Cpm22Machine *) context)->m_diskDrive;
  }

  static int ioReadDiskStatus(void * context, uint8_t) {
    return ((Cpm22Machine *) context)->m_diskStatus;
  }

  fabgl::Z80 m_cpu;
  Cpm22Bus m_bus;
  Stream * m_console = nullptr;
  uint32_t m_lastMicros = 0;
  uint8_t m_diskCommand = 0;
  uint8_t m_diskDrive = 0;
  uint8_t m_userNumber = 0;
  uint16_t m_diskTrack = 0;
  uint16_t m_diskSector = 0;
  uint16_t m_diskDma = 0;
  uint8_t m_diskStatus = 0;
  uint32_t m_suppressedNullWrites = 0;
  uint32_t m_bdosConsoleOutputTrace = 0;
  uint16_t m_bdosLineBuffer = 0;
  uint8_t m_bdosLineLength = 0;
  uint8_t m_bdosSearchIndex = 0;
  uint8_t m_bdosSearchPattern[12] = { 0 };
  uint32_t m_lastCcpLoadFailureMs = 0;
  uint16_t m_programReturnAddress = 0;
};

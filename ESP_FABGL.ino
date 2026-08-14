// CPM 22 ESP Emulator using FabGL VGA and PS/2 support

#include "cpm22_machine.h"
#include "mcm6576_font.h"
#include <SPI.h>
#include <SD.h>
#include <esp_system.h>

Cpm22Machine Emulator;
bool RuntimeVGAEnabled = false;
bool SdCardReady = false;

RTC_DATA_ATTR uint32_t BootCount = 0;
RTC_DATA_ATTR uint32_t CrashGuardState = 0;
constexpr uint32_t VGA_INIT_GUARD_MAGIC = 0x56474131;  // "VGA1"

// Select board pin profile.
// true  = dedicated FabGL ESP32 module pinout
// false = legacy ESP-WROOM wiring used in earlier revisions
#define USE_DEDICATED_FABGL_MODULE_PINOUT 1

#if USE_DEDICATED_FABGL_MODULE_PINOUT
// Dedicated FabGL module VGA mapping:
// R: GPIO22 (MSB), GPIO21 (LSB)
// G: GPIO19 (MSB), GPIO18 (LSB)
// B: GPIO5  (MSB), GPIO4  (LSB)
// HSYNC: GPIO23, VSYNC: GPIO15
#define VGA_RED_1    GPIO_NUM_22
#define VGA_RED_0    GPIO_NUM_21
#define VGA_GREEN_1  GPIO_NUM_19
#define VGA_GREEN_0  GPIO_NUM_18
#define VGA_BLUE_1   GPIO_NUM_5
#define VGA_BLUE_0   GPIO_NUM_4
#define VGA_HSYNC    GPIO_NUM_23
#define VGA_VSYNC    GPIO_NUM_15

#else
// Legacy ESP-WROOM mapping from previous build.
#define VGA_RED_1    GPIO_NUM_22  // MSB (Brightest)
#define VGA_RED_0    GPIO_NUM_23  // LSB (Muted)
#define VGA_GREEN_1  GPIO_NUM_19  // MSB (Brightest)
#define VGA_GREEN_0  GPIO_NUM_21  // LSB (Muted)
#define VGA_BLUE_1   GPIO_NUM_5   // MSB (Brightest)
#define VGA_BLUE_0   GPIO_NUM_18  // LSB (Muted)
#define VGA_HSYNC    GPIO_NUM_17
#define VGA_VSYNC    GPIO_NUM_16
#endif

class LocalTerminal {
public:
  static constexpr uint8_t COLUMNS = 78;
  static constexpr uint8_t ROWS = 24;
  static constexpr int X = 8;
  static constexpr int Y = 8;

  void begin(fabgl::Canvas * canvas) {
    m_canvas = canvas;
    clear();
  }

  void clear() {
    prepareCanvas();
    memset(m_cells, ' ', sizeof(m_cells));
    m_x = m_y = 0;
    renderAll();
  }

  size_t write(uint8_t value) {
    prepareCanvas();
    if (m_ansi) {
      renderCell(m_x, m_y);
      consumeAnsi(value);
      return 1;
    }
    if (value == 0x1B) { m_ansi = true; m_csi = false; m_paramCount = 0; m_params[0] = 0; return 1; }
    renderCell(m_x, m_y);
    switch (value) {
      case '\r': m_x = 0; break;
      case '\n': lineFeed(); break;
      case '\b': if (m_x) --m_x; break;
      case '\t': m_x = (m_x + 8) & ~7; if (m_x >= COLUMNS) lineFeed(); break;
      default: if (value >= 0x20) put(value); break;
    }
    drawCursor();
    return 1;
  }

  size_t write(const char * text) {
    if (!text) return 0;
    size_t count = 0;
    while (*text) count += write((uint8_t) *text++);
    return count;
  }

private:
  fabgl::Canvas * m_canvas = nullptr;
  uint8_t m_cells[COLUMNS * ROWS] = { 0 };
  uint8_t m_x = 0, m_y = 0;
  bool m_ansi = false, m_csi = false;
  uint8_t m_params[4] = { 0 }, m_paramCount = 0;

  void prepareCanvas() {
    if (!m_canvas) return;
    m_canvas->setOrigin(0, 0);
    m_canvas->setClippingRect(fabgl::Rect(X, Y, X + COLUMNS * MCM6576_GLYPH_WIDTH - 1, Y + ROWS * MCM6576_GLYPH_HEIGHT - 1));
  }

  void renderCell(uint8_t x, uint8_t y, bool cursor = false) {
    if (!m_canvas || x >= COLUMNS || y >= ROWS) return;
    int px = X + x * MCM6576_GLYPH_WIDTH;
    int py = Y + y * MCM6576_GLYPH_HEIGHT;
    m_canvas->setBrushColor(cursor ? fabgl::Color::Green : fabgl::Color::Black);
    m_canvas->setPenColor(cursor ? fabgl::Color::Black : fabgl::Color::Green);
    // Do not rely on drawGlyph() to erase unset bits.  Explicitly clearing
    // the cell prevents remnants of the previous row after a software scroll.
    m_canvas->fillRectangle(px, py, px + MCM6576_GLYPH_WIDTH - 1, py + MCM6576_GLYPH_HEIGHT - 1);
    m_canvas->drawGlyph(px, py, MCM6576_GLYPH_WIDTH, MCM6576_GLYPH_HEIGHT, &MCM6576_FONT[0][0], m_cells[y * COLUMNS + x]);
  }

  void renderAll() {
    if (!m_canvas) return;
    for (uint8_t y = 0; y < ROWS; ++y)
      for (uint8_t x = 0; x < COLUMNS; ++x)
        renderCell(x, y);
    drawCursor();
    m_canvas->waitCompletion(false);
  }

  void drawCursor() { renderCell(m_x, m_y, true); }

  void put(uint8_t value) {
    m_cells[m_y * COLUMNS + m_x] = value;
    renderCell(m_x, m_y);
    if (++m_x == COLUMNS) lineFeed();
  }

  void lineFeed() {
    m_x = 0;
    if (++m_y < ROWS) return;
    memmove(m_cells, m_cells + COLUMNS, COLUMNS * (ROWS - 1));
    memset(m_cells + COLUMNS * (ROWS - 1), ' ', COLUMNS);
    m_y = ROWS - 1;
    renderAll();
  }

  uint8_t param(uint8_t index, uint8_t fallback = 1) const {
    return index <= m_paramCount && m_params[index] ? m_params[index] : fallback;
  }

  static uint8_t clamp(uint8_t value, uint8_t maximum) {
    return value > maximum ? maximum : value;
  }

  void consumeAnsi(uint8_t value) {
    if (!m_csi) {
      m_ansi = false;
      if (value == '[') { m_ansi = true; m_csi = true; m_params[0] = 0; }
      return;
    }
    if (value >= '0' && value <= '9') { m_params[m_paramCount] = m_params[m_paramCount] * 10 + value - '0'; return; }
    if (value == ';' && m_paramCount < 3) { m_params[++m_paramCount] = 0; return; }
    switch (value) {
      case 'A': m_y = m_y >= param(0) ? m_y - param(0) : 0; break;
      case 'B': m_y = clamp(m_y + param(0), ROWS - 1); break;
      case 'C': m_x = clamp(m_x + param(0), COLUMNS - 1); break;
      case 'D': m_x = m_x >= param(0) ? m_x - param(0) : 0; break;
      case 'H': case 'f': m_y = clamp(param(0) - 1, ROWS - 1); m_x = clamp(param(1) - 1, COLUMNS - 1); break;
      case 'J': if (m_params[0] == 2 || m_params[0] == 0) clear(); break;
      case 'K': memset(m_cells + m_y * COLUMNS + m_x, ' ', COLUMNS - m_x); for (uint8_t x = m_x; x < COLUMNS; ++x) renderCell(x, m_y); break;
    }
    m_ansi = m_csi = false;
    drawCursor();
  }
};

class FabGLTerminalStream : public Stream {
public:
  FabGLTerminalStream(LocalTerminal * terminal, Stream * debugMirror)
    : m_terminal(terminal), m_debugMirror(debugMirror), m_serialInput(false) {
  }

  void setTerminal(LocalTerminal * terminal) {
    m_terminal = terminal;
  }

  void setSerialInput(bool enabled) {
    m_serialInput = enabled;
  }

  void setInput(Stream * input) {
    m_inputOverride = input;
  }

  int available() override {
    Stream * input = m_inputOverride ? m_inputOverride : (m_serialInput ? m_debugMirror : nullptr);
    return input ? input->available() : 0;
  }

  int read() override {
    Stream * input = m_inputOverride ? m_inputOverride : (m_serialInput ? m_debugMirror : nullptr);
    return input ? input->read() : -1;
  }

  int peek() override {
    Stream * input = m_inputOverride ? m_inputOverride : (m_serialInput ? m_debugMirror : nullptr);
    return input ? input->peek() : -1;
  }

  void flush() override {
    if (m_debugMirror)
      m_debugMirror->flush();
  }

  size_t write(uint8_t value) override {
    size_t written = 0;
    if (m_terminal)
      written += m_terminal->write(value);
    if (m_debugMirror)
      m_debugMirror->write(value);
    return written > 0 ? 1 : 0;
  }

private:
  LocalTerminal * m_terminal;
  Stream * m_debugMirror;
  Stream * m_inputOverride = nullptr;
  bool m_serialInput;
};

class Ps2KeyboardStream : public Stream {
public:
  void poll(fabgl::Keyboard * keyboard) {
    if (!keyboard)
      return;

    while (keyboard->virtualKeyAvailable() > 0) {
      bool keyDown = false;
      fabgl::VirtualKey key = keyboard->getNextVirtualKey(&keyDown, 0);
      if (!keyDown)
        continue;

      int value = keyboard->virtualKeyToASCII(key);
      if (value >= 0)
        enqueue((uint8_t) value);
    }
  }

  int available() override {
    return m_count;
  }

  int read() override {
    if (m_count == 0)
      return -1;

    uint8_t value = m_buffer[m_head];
    m_head = (m_head + 1) % BUFFER_SIZE;
    --m_count;
    return value;
  }

  int peek() override {
    return m_count == 0 ? -1 : m_buffer[m_head];
  }

  void flush() override {
    m_head = 0;
    m_count = 0;
  }

  size_t write(uint8_t) override {
    return 0;
  }

private:
  static constexpr uint8_t BUFFER_SIZE = 64;
  uint8_t m_buffer[BUFFER_SIZE] = { 0 };
  uint8_t m_head = 0;
  uint8_t m_tail = 0;
  uint8_t m_count = 0;

  void enqueue(uint8_t value) {
    if (m_count >= BUFFER_SIZE)
      return;

    m_buffer[m_tail] = value;
    m_tail = (m_tail + 1) % BUFFER_SIZE;
    ++m_count;
  }
};

LocalTerminal Terminal;
FabGLTerminalStream TerminalStream(&Terminal, &Serial);
Ps2KeyboardStream Ps2Input;
fabgl::Keyboard * Ps2Keyboard = nullptr;

// VGA16Controller is suitable for 2-bit per channel resistor DAC output.
fabgl::VGA16Controller VGAController;
fabgl::Canvas ConsoleCanvas(&VGAController);

constexpr bool ENABLE_VGA_OUTPUT = true;
constexpr bool ENABLE_HEARTBEAT_TRACE = false;
// Use FabGL's known-good default routing first on dedicated boards.
// Set false to force explicit custom pin mapping from defines above.
constexpr bool USE_FABGL_DEFAULT_VGA_PINMAP = true;
constexpr bool ENABLE_PS2_KEYBOARD_INPUT = true;
constexpr int VGA_RETRY_BUTTON_PIN = 36;

// The FabGL terminal font is 8 x 16 pixels at this resolution.  Keeping the
// terminal grid explicit makes room for a permanent debugger panel.
constexpr int UI_REGISTER_PANEL_WIDTH = 640;
constexpr int UI_TERMINAL_X = 8;
constexpr int UI_TERMINAL_Y = 8;
constexpr int UI_TERMINAL_COLUMNS = 78;
constexpr int UI_TERMINAL_ROWS = 24;
constexpr uint32_t UI_REGISTER_REFRESH_MS = 200;

static uint32_t LastHeartbeatMS = 0;
static uint32_t LastRegisterRefreshMS = 0;

static const char * resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNMAPPED";
  }
}

static void logBoot(const char * msg) {
  Serial.println(msg);
  Serial.flush();
}

static void drawBootSplash(const char * line1, const char * line2) {
  if (!ENABLE_VGA_OUTPUT)
    return;
  Terminal.clear();
  Terminal.write(line1 ? line1 : "");
  Terminal.write("\r\n");
  Terminal.write(line2 ? line2 : "");
  Terminal.write("\r\n");
}

static void drawDebuggerFrame(bool redrawBackground) {
  fabgl::Canvas * canvas = &ConsoleCanvas;

  canvas->setOrigin(0, 0);
  canvas->setClippingRect(fabgl::Rect(0, 0, 639, 479));
  if (redrawBackground) {
    // Dark umber chassis, brass double-line windows, and small rivets give
    // the otherwise clean terminal a period instrument-panel character.
    canvas->setBrushColor(fabgl::Color::Black);
    canvas->fillRectangle(0, 0, 639, 479);

    canvas->setPenColor(170, 85, 0);  // brown/brass in the VGA 2-bit palette
    canvas->drawRectangle(4, 0, 635, 400);
    canvas->drawRectangle(4, 404, 635, 430);
  }
}

static void drawRegisterPanel() {
  if (!RuntimeVGAEnabled)
    return;

  Cpm22Machine::CpuSnapshot r = Emulator.cpuSnapshot();
  fabgl::Canvas * canvas = &ConsoleCanvas;
  canvas->setOrigin(0, 0);
  canvas->setClippingRect(fabgl::Rect(0, 404, 639, 430));
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->fillRectangle(5, 405, 634, 429);
  canvas->setPenColor(r.status == fabgl::Z80_STATUS_HALT ? fabgl::Color::Red : fabgl::Color::Green);
  canvas->drawTextFmt(14, 410, "AF %04X", r.af);
  canvas->drawTextFmt(92, 410, "BC %04X", r.bc);
  canvas->drawTextFmt(170, 410, "DE %04X", r.de);
  canvas->drawTextFmt(248, 410, "HL %04X", r.hl);
  canvas->drawTextFmt(326, 410, "IX %04X", r.ix);
  canvas->drawTextFmt(404, 410, "IY %04X", r.iy);
  canvas->drawTextFmt(482, 410, "SP %04X", r.sp);
  canvas->drawTextFmt(560, 410, "PC %04X", r.pc);
}

void setup() {
  RuntimeVGAEnabled = ENABLE_VGA_OUTPUT;
  if (!RuntimeVGAEnabled) {
    TerminalStream.setTerminal(nullptr);
    TerminalStream.setSerialInput(true);
  }

  Serial.begin(115200);
  delay(250);

  // Compile identity banner so we can confirm the board is running the expected source.
  Serial.println("[BUILD] ESP_FABGL firmware banner");
  Serial.printf("[BUILD] file=%s\n", __FILE__);
  Serial.printf("[BUILD] date=%s time=%s\n", __DATE__, __TIME__);
  Serial.printf("[BUILD] hash=%08X\n", (unsigned int) (0xA5A5C3C3u ^ __LINE__));

  SPI.begin(14, 2, 12, 13);
  SdCardReady = false;
  if (!SD.begin(13, SPI, 4000000))
    logBoot("[SD] MicroSD init failed");
  else {
    SdCardReady = true;
    logBoot("[SD] MicroSD ready: A.DSK/B.DSK images supported");
    Serial.printf("[SD] cardType=%d existsA=%d existsB=%d\n",
                  SD.cardType(),
                  SD.exists("/A.DSK"),
                  SD.exists("/B.DSK"));

    File root = SD.open("/");
    if (root) {
      Serial.println("[SD] root listing:");
      while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        Serial.println(entry.name());
        entry.close();
      }
      root.close();
    }
  }

  ++BootCount;
  esp_reset_reason_t resetReason = esp_reset_reason();

  logBoot("[BOOT] ESPFABGL setup entered");
  Serial.printf("[BOOT] count=%lu reset_reason=%s (%d)\n",
                (unsigned long) BootCount,
                resetReasonToString(resetReason),
                (int) resetReason);

  pinMode(VGA_RETRY_BUTTON_PIN, INPUT);
  bool forceVgaRetry = (digitalRead(VGA_RETRY_BUTTON_PIN) == LOW);
  if (forceVgaRetry)
    logBoot("[BOOT] VGA retry button asserted (GPIO36)");

  // If previous boot died during VGA init, skip VGA this time so serial remains usable.
  if (CrashGuardState == VGA_INIT_GUARD_MAGIC) {
    RuntimeVGAEnabled = false;
    logBoot("[SAFE MODE] Previous reset occurred during VGA init; VGA disabled this boot");
  }

  // Allow manual one-boot override even when crash guard requests safe mode.
  if (forceVgaRetry && ENABLE_VGA_OUTPUT) {
    RuntimeVGAEnabled = true;
    logBoot("[SAFE MODE] Override active: retrying VGA init this boot");
  }

  // Watchdog reset is reported for diagnostics, but alone does not disable VGA.
  if (resetReason == ESP_RST_INT_WDT ||
      resetReason == ESP_RST_TASK_WDT ||
      resetReason == ESP_RST_WDT) {
    logBoot("[BOOT] Watchdog reset noted");
  }

  if (!RuntimeVGAEnabled)
    logBoot("[BOOT] VGA output disabled for diagnostics");

  if (ENABLE_PS2_KEYBOARD_INPUT) {
    fabgl::PS2Controller::begin(fabgl::PS2Preset::KeyboardPort0, fabgl::KbdMode::NoVirtualKeys);
    Ps2Keyboard = fabgl::PS2Controller::keyboard();
    auto mouse = fabgl::PS2Controller::mouse();
    Serial.printf("[PS2] keyboard=%s mouse=%s\n",
                  Ps2Keyboard ? "detected" : "missing",
                  mouse ? "detected" : "missing");
    if (Ps2Keyboard) {
      Ps2Keyboard->setLayout(&fabgl::USLayout);
      Ps2Keyboard->enableVirtualKeys(true, true);
      TerminalStream.setInput(&Ps2Input);
    }
  }

  logBoot("[BOOT] Terminal routing configured");

  if (RuntimeVGAEnabled) {
    logBoot("[BOOT] Starting VGA controller");

    // Mark entry to VGA init; if we reset before clearing this, next boot enters safe mode.
    CrashGuardState = VGA_INIT_GUARD_MAGIC;

    if (USE_FABGL_DEFAULT_VGA_PINMAP) {
      logBoot("[BOOT] VGA begin() using FabGL default pin map");
      VGAController.begin();
    } else {
      logBoot("[BOOT] VGA begin() using explicit custom pin map");
      // Pass the 6-bit pin setup block directly to FabGL to wake up the DAC lines.
      // Order must be Bit 1 (MSB) then Bit 0 (LSB) for each colour channel.
      VGAController.begin(
        VGA_RED_1,   VGA_RED_0,
        VGA_GREEN_1, VGA_GREEN_0,
        VGA_BLUE_1,  VGA_BLUE_0,
        VGA_HSYNC,   VGA_VSYNC
      );
    }

    // Set monitor timing to standard 640x480 resolution, then attach terminal emulator.
    logBoot("[VGA] before setResolution");
    VGAController.setResolution(VGA_640x480_60Hz);
    logBoot("[VGA] after setResolution");
    logBoot("[TERM] starting local MCM6576 terminal");
    drawDebuggerFrame(true);
    Terminal.begin(&ConsoleCanvas);
    logBoot("[TERM] local terminal ready");
    drawRegisterPanel();
    logBoot("[TERM] PS/2 input queue enabled");
    CrashGuardState = 0;
    logBoot("[BOOT] VGA ready");
  } else {
    CrashGuardState = 0;
  }

  // Bind and start the Z80 core using CP/M I/O callbacks.
  Serial.println("[BOOT] before Emulator.setConsoleStream");
  Emulator.setConsoleStream((Stream *) &TerminalStream);
  Serial.println("[BOOT] before Emulator.begin");
  Emulator.begin(0x0000);
  Serial.println("[BOOT] after Emulator.begin");

  logBoot("[BOOT] Emulator core initialized");

  Serial.printf("[BOOT] SD remains available for CP/M disk I/O: ready=%d\n",
                SdCardReady ? 1 : 0);

  // Single boot path: MINICCP.COM is always loaded from SD, both here and
  // on every subsequent warm boot / program-exit reload (see
  // Cpm22Machine::reloadWarmBootImage). If this fails, PC stays at 0000h
  // and the emulator's own warm-boot trap keeps retrying automatically.
  if (Emulator.bootFromSd())
    Serial.println("[CPM] Z80 CCP loaded from SD");
  else
    Serial.println("[CPM] MINICCP.COM not available yet; will keep retrying from SD");

  logBoot("[BOOT] setup complete");
}

void loop() {
  Ps2Input.poll(Ps2Keyboard);
  Emulator.runForElapsedTime();

  uint32_t nowMS = millis();
  if (RuntimeVGAEnabled && nowMS - LastRegisterRefreshMS >= UI_REGISTER_REFRESH_MS) {
    LastRegisterRefreshMS = nowMS;
    drawRegisterPanel();
  }
  if (ENABLE_HEARTBEAT_TRACE && nowMS - LastHeartbeatMS >= 2000) {
    LastHeartbeatMS = nowMS;
    Serial.printf("[HEARTBEAT] loop alive; tty=%s\n",
                  Ps2Keyboard ? "ready" : "missing");
    Serial.printf("[Z80] pc=0x%04X status=%d\n",
                  Emulator.programCounter(),
                  Emulator.cpuStatus());
  }
}

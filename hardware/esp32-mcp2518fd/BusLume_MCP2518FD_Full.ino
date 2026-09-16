#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <ACAN2517FD.h>
#include <BluetoothSerial.h>
#include <stdio.h>
#include <string.h>
#include <Preferences.h>

// BusLume ESP32 USB-CAN adapter — MCP2518FD full firmware
// ESP32-WROOM-32 <-> MCP2518FD (CAN 2.0B / CAN FD controller)
// USB serial: 115200 baud, Lawicel/SLCAN-compatible commands
// This is the working firmware. v1-v13 remain available as backups.
// Bluetooth Classic SPP name: BusLume-ESP32

const byte SCK_PIN  = 18;
const byte MISO_PIN = 19;
const byte MOSI_PIN = 23;
const byte CS_PIN   = 5;
const byte INT_PIN  = 27;

// На перевіреному MCP2518FD встановлено кварц 20 MHz.
// Якщо на іншому модулі буде маркування 40 MHz або 4 MHz, змінити
// відповідно на OSC_40MHz або OSC_4MHz.
static constexpr auto MCP2518_OSC = ACAN2517FDSettings::OSC_20MHz;

const byte OLED_SDA_PIN = 21;
const byte OLED_SCL_PIN = 22;

const byte DEFAULT_BITRATE = 6; // S6 = 500 kbit/s (SLCAN compatibility)
const unsigned long SERIAL_SPEED = 115200;
const char *BLUETOOTH_NAME = "BusLume-ESP32";
const unsigned long USB_FORCE_UNLOCK_MS = 3000;
const unsigned long BT_COMMAND_IDLE_MS = 80;

// Analog joystick acceleration.  A light deflection repeats slowly so one
// value can be selected precisely; a full deflection repeats quickly for
// scanning through IDs, bytes, periods, or menu entries.
const int JOY_CENTER = 2048;
const int JOY_DEADZONE = 550;
const int JOY_MAX_DEFLECTION = 2047;
const unsigned long JOY_REPEAT_SLOW_MS = 300;
const unsigned long JOY_REPEAT_FAST_MS = 30;
const unsigned long JOY_X_REPEAT_MS = 180;

// Local controls: all buttons are active LOW and connect to GND.
const byte BTN_MODE_PIN = 25;  // BACK
const byte BTN_SHOT_PIN = 26;  // TX: SEND ALL
const byte JOY_SW_PIN = 32;    // short: SIGNAL SHOT, long: SAVE
const byte JOY_X_PIN = 34;     // VRx, input-only ADC
const byte JOY_Y_PIN = 35;     // VRy, input-only ADC

ACAN2517FD canController(CS_PIN, SPI, INT_PIN);
bool canControllerStarted = false;
ACAN2517FDSettings::OperationMode canHardwareMode = ACAN2517FDSettings::Configuration;

// Small internal record retained by the UI/SLCAN layer. It is converted to
// and from ACAN2517FD's CANFDMessage at the driver boundary.
struct can_frame {
  uint32_t can_id = 0;
  uint8_t can_dlc = 0;
  uint8_t data[8] = {0};
};

static constexpr uint32_t CAN_EFF_FLAG = 0x80000000UL;
static constexpr uint32_t CAN_RTR_FLAG = 0x40000000UL;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
BluetoothSerial SerialBT;

// Arbitration rates for the MCP2518FD at the selected 20 MHz oscillator.
// SLCAN codes are retained where the standard table defines them.
struct CanSpeedOption {
  const char *label;
  uint32_t value;
  byte slcanCode;
};

const CanSpeedOption CAN_SPEEDS[] = {
  {"5K",     5000UL,     0xFF},
  {"10K",    10000UL,    0},
  {"20K",    20000UL,    1},
  {"31.25K", 31250UL,    0xFF},
  {"33.33K", 33330UL,    0xFF},
  {"40K",    40000UL,    0xFF},
  {"50K",    50000UL,    2},
  {"80K",    80000UL,    0xFF},
  {"100K",   100000UL,   3},
  {"125K",   125000UL,   4},
  {"200K",   200000UL,   0xFF},
  {"250K",   250000UL,   5},
  {"500K",   500000UL,   6},
  {"1M",     1000000UL,  8}
};

const byte CAN_SPEED_COUNT = sizeof(CAN_SPEEDS) / sizeof(CAN_SPEEDS[0]);
const byte DEFAULT_SPEED_INDEX = 12; // 500 kbit/s

enum HostProtocol : byte {
  HOST_NONE,
  HOST_USB,
  HOST_BLUETOOTH
};

HostProtocol activeCommandHost = HOST_USB;
Print *commandReply = &Serial;
bool usbHostActive = false;
bool bluetoothHostActive = false;
bool hostScreenVisible = false;
bool lastBluetoothClientState = false;

// Keep host receive buffers globally so a Bluetooth disconnect can discard a
// half-received command instead of mixing it with the next connection.
char usbCommandBuffer[80];
char bluetoothCommandBuffer[80];
byte usbCommandLength = 0;
byte bluetoothCommandLength = 0;
unsigned long usbLastByteAt = 0;
unsigned long bluetoothLastByteAt = 0;

enum AdapterMode : byte {
  MODE_CLOSED,
  MODE_NORMAL,
  MODE_LISTEN_ONLY,
  MODE_LOOPBACK
};

AdapterMode adapterMode = MODE_CLOSED;
bool oledOK = false;
bool haveFrame = false;
bool lastFrameWasRx = false;
bool timestampsEnabled = false;

byte bitrateCode = DEFAULT_BITRATE;
byte speedIndex = DEFAULT_SPEED_INDEX;
unsigned long rxCount = 0;
unsigned long txCount = 0;
unsigned long errorCount = 0;
uint16_t lastErrorFlags = 0;

unsigned long lastFrameId = 0;
byte lastFrameDlc = 0;
byte lastFrameData[8] = {0};
unsigned long lastTimestamp = 0;

// Standard 11-bit acceptance filter. Mask 0 means receive all frames.
unsigned long filterCode = 0;
unsigned long filterMask = 0;

char statusLine[21] = "READY - SEND O";
unsigned long lastDisplay = 0;
unsigned long lastRecovery = 0;
enum PopupIcon : byte {
  POPUP_NONE,
  POPUP_SAVE,
  POPUP_DELETE
};

PopupIcon popupIcon = POPUP_NONE;
unsigned long popupUntil = 0;
enum UiScreen : byte {
  UI_MENU,
  UI_MONITOR,
  UI_EDITOR,
  UI_SAVED
};

UiScreen uiBeforeHost = UI_MENU;

struct SavedFrame {
  uint32_t can_id;
  uint8_t dlc;
  uint8_t data[8];
};

const byte MAX_SAVED_FRAMES = 16;
const unsigned long DEFAULT_PERIOD_MS = 1000;
const unsigned long MIN_PERIOD_MS = 10;
const unsigned long MAX_PERIOD_MS = 60000;
const unsigned long PERIOD_STEP_MS = 10;
const byte STORAGE_SCHEMA_VERSION = 1;

Preferences preferences;
SavedFrame savedFrames[MAX_SAVED_FRAMES];
unsigned long savedPeriods[MAX_SAVED_FRAMES];
byte savedCount = 0;
byte savedIndex = 0;

UiScreen uiScreen = UI_MENU;
byte menuIndex = 0;

uint16_t canId = 0x141;
byte canDlc = 8;
byte canData[8] = {0, 1, 2, 3, 4, 5, 6, 7};
byte selectedField = 0; // 0=ID, 1=DLC, 2..9=D0..D7, 10=PERIOD
unsigned long currentPeriodMs = DEFAULT_PERIOD_MS;
byte localShotCounter = 0;
bool sendAllActive = false;
unsigned long lastCurrentTx = 0;
unsigned long lastSavedTx[MAX_SAVED_FRAMES] = {0};

bool lastModeButton = HIGH;
bool lastShotButton = HIGH;
bool lastJoyButton = HIGH;
unsigned long lastModeButtonChange = 0;
unsigned long lastShotButtonChange = 0;
unsigned long lastJoyButtonChange = 0;
unsigned long joyPressedAt = 0;
bool joyIsPressed = false;
bool joyLongHandled = false;
unsigned long shotPressedAt = 0;
unsigned long lastXAction = 0;
unsigned long lastYAction = 0;

bool restoreMode();
bool applyCanSpeed(byte index);

int joystickDirection(int value) {
  if (value < JOY_CENTER - JOY_DEADZONE) return -1;
  if (value > JOY_CENTER + JOY_DEADZONE) return 1;
  return 0;
}

unsigned long joystickRepeatDelay(int value) {
  int deflection = abs(value - JOY_CENTER);
  if (deflection <= JOY_DEADZONE) return 0;
  if (deflection > JOY_MAX_DEFLECTION) deflection = JOY_MAX_DEFLECTION;

  return (unsigned long)map(deflection,
                             JOY_DEADZONE, JOY_MAX_DEFLECTION,
                             JOY_REPEAT_SLOW_MS, JOY_REPEAT_FAST_MS);
}

const char HEX_DIGITS[] = "0123456789ABCDEF";

byte findOLED() {
  const byte addresses[2] = {0x3C, 0x3D};
  for (byte i = 0; i < 2; i++) {
    Wire.beginTransmission(addresses[i]);
    if (Wire.endTransmission() == 0) return addresses[i];
  }
  return 0;
}

void setStatus(const char *text) {
  snprintf(statusLine, sizeof(statusLine), "%s", text);
}

bool hostLockActive() {
  return usbHostActive || bluetoothHostActive;
}

void updateHostScreenState() {
  bool btConnected = SerialBT.hasClient();
  if (btConnected && !lastBluetoothClientState) {
    // A fresh SPP session starts with a clean command boundary.
    bluetoothCommandLength = 0;
    bluetoothLastByteAt = 0;
    setStatus("BT CONNECTED");
  } else if (!btConnected && lastBluetoothClientState) {
    bluetoothCommandLength = 0;
    bluetoothLastByteAt = 0;
    setStatus("BT DISCONNECTED");
  }
  lastBluetoothClientState = btConnected;
  bluetoothHostActive = btConnected;

  if (hostLockActive() && !hostScreenVisible) {
    uiBeforeHost = uiScreen;
    hostScreenVisible = true;
  } else if (!hostLockActive() && hostScreenVisible) {
    hostScreenVisible = false;
    uiScreen = uiBeforeHost;
  }
}

void noteHostActivity(HostProtocol protocol) {
  if (protocol == HOST_BLUETOOTH) {
    bluetoothHostActive = true;
  } else if (protocol == HOST_USB) {
    usbHostActive = true;
  }
  updateHostScreenState();
}

void clearActiveHost() {
  if (activeCommandHost == HOST_BLUETOOTH) {
    bluetoothHostActive = false;
  } else if (activeCommandHost == HOST_USB) {
    usbHostActive = false;
  }
  updateHostScreenState();
}

void handleHostUnlock() {
  // CH340C/UART cannot report a physical USB cable removal to the ESP32.
  // Holding the BACK button provides a deterministic local recovery after
  // unplugging USB. Bluetooth remains locked while its client is connected.
  static unsigned long pressedAt = 0;

  if (bluetoothHostActive) {
    pressedAt = 0;
    return;
  }

  if (digitalRead(BTN_MODE_PIN) == LOW) {
    if (pressedAt == 0) pressedAt = millis();
    if (millis() - pressedAt >= USB_FORCE_UNLOCK_MS) {
      usbHostActive = false;
      pressedAt = 0;
      updateHostScreenState();
      setStatus("USB UNLOCKED");
    }
  } else {
    pressedAt = 0;
  }
}

void selectCommandReply(HostProtocol protocol) {
  activeCommandHost = protocol;
  commandReply = protocol == HOST_BLUETOOTH
               ? (Print *)&SerialBT
               : (Print *)&Serial;
}

void prepareStorage() {
  // v12 starts with an empty saved-frame list. Clear the old v10/v11 records
  // once, then keep all future saves persistent across reboots.
  byte storedSchema = preferences.getUChar("schema", 0);
  if (storedSchema != STORAGE_SCHEMA_VERSION) {
    preferences.clear();
    preferences.putUChar("schema", STORAGE_SCHEMA_VERSION);
  }
}

void saveFramesToPreferences() {
  preferences.putUChar("count", savedCount);
  for (byte i = 0; i < savedCount; i++) {
    char key[5];
    char periodKey[5];
    snprintf(key, sizeof(key), "f%u", (unsigned)i);
    snprintf(periodKey, sizeof(periodKey), "p%u", (unsigned)i);
    preferences.putBytes(key, &savedFrames[i], sizeof(SavedFrame));
    preferences.putULong(periodKey, savedPeriods[i]);
  }
}

void loadFramesFromPreferences() {
  savedCount = preferences.getUChar("count", 0);
  if (savedCount > MAX_SAVED_FRAMES) savedCount = 0;

  for (byte i = 0; i < savedCount; i++) {
    char key[5];
    char periodKey[5];
    snprintf(key, sizeof(key), "f%u", (unsigned)i);
    snprintf(periodKey, sizeof(periodKey), "p%u", (unsigned)i);
    if (preferences.getBytes(key, &savedFrames[i], sizeof(SavedFrame))
        != sizeof(SavedFrame)) {
      savedCount = i;
      break;
    }
    savedPeriods[i] = preferences.getULong(periodKey, DEFAULT_PERIOD_MS);
    if (savedPeriods[i] < MIN_PERIOD_MS || savedPeriods[i] > MAX_PERIOD_MS) {
      savedPeriods[i] = DEFAULT_PERIOD_MS;
    }
  }
}

void saveCurrentFrame() {
  byte slot = savedCount;

  if (savedCount < MAX_SAVED_FRAMES) {
    savedCount++;
  } else {
    for (byte i = 1; i < MAX_SAVED_FRAMES; i++) {
      savedFrames[i - 1] = savedFrames[i];
      savedPeriods[i - 1] = savedPeriods[i];
    }
    slot = MAX_SAVED_FRAMES - 1;
  }

  savedFrames[slot].can_id = canId & 0x7FF;
  savedFrames[slot].dlc = canDlc;
  for (byte i = 0; i < 8; i++) savedFrames[slot].data[i] = canData[i];
  savedPeriods[slot] = currentPeriodMs;

  saveFramesToPreferences();
  savedIndex = slot;
  popupIcon = POPUP_SAVE;
  popupUntil = millis() + 1400;
  setStatus("FRAME SAVED");
}

void deleteSavedFrame() {
  if (savedCount == 0) {
    setStatus("NO SAVED FRAMES");
    return;
  }

  for (byte i = savedIndex; i + 1 < savedCount; i++) {
    savedFrames[i] = savedFrames[i + 1];
    savedPeriods[i] = savedPeriods[i + 1];
  }

  savedCount--;
  if (savedCount == 0) {
    savedIndex = 0;
  } else if (savedIndex >= savedCount) {
    savedIndex = savedCount - 1;
  }

  // Removing a saved frame must also stop any periodic list transmission.
  sendAllActive = false;
  saveFramesToPreferences();
  popupIcon = POPUP_DELETE;
  popupUntil = millis() + 1400;
  setStatus("FRAME DELETED");
}

void copySavedToEditor() {
  if (savedCount == 0) {
    setStatus("NO SAVED FRAMES");
    return;
  }
  canId = savedFrames[savedIndex].can_id & 0x7FF;
  canDlc = savedFrames[savedIndex].dlc;
  for (byte i = 0; i < 8; i++) canData[i] = savedFrames[savedIndex].data[i];
  currentPeriodMs = savedPeriods[savedIndex];
  selectedField = 0;
  uiScreen = UI_EDITOR;
  setStatus("FRAME LOADED");
}

void setStatusForScreen() {
  if (uiScreen == UI_MENU) setStatus("MAIN MENU");
  else if (uiScreen == UI_MONITOR) setStatus("CAN MONITOR");
  else if (uiScreen == UI_EDITOR) setStatus("TX EDITOR");
  else if (uiScreen == UI_SAVED) setStatus("SAVED FRAMES");
}

void printHex(unsigned long value, byte digits) {
  for (int i = digits - 1; i >= 0; i--) {
    commandReply->write(HEX_DIGITS[(value >> (i * 4)) & 0x0F]);
  }
}

void printHexTo(Print &output, unsigned long value, byte digits) {
  for (int i = digits - 1; i >= 0; i--) {
    output.write(HEX_DIGITS[(value >> (i * 4)) & 0x0F]);
  }
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool parseHex(const char *text, byte digits, unsigned long &value) {
  value = 0;
  for (byte i = 0; i < digits; i++) {
    int nibble = hexValue(text[i]);
    if (nibble < 0) return false;
    value = (value << 4) | (unsigned long)nibble;
  }
  return true;
}

const char *bitrateText() {
  return speedIndex < CAN_SPEED_COUNT ? CAN_SPEEDS[speedIndex].label : "---";
}

const char *modeText() {
  switch (adapterMode) {
    case MODE_NORMAL: return "NORMAL";
    case MODE_LISTEN_ONLY: return "LISTEN";
    case MODE_LOOPBACK: return "LOOP";
    default: return "CLOSED";
  }
}

bool applyBitrate(byte code) {
  for (byte i = 0; i < CAN_SPEED_COUNT; i++) {
    if (CAN_SPEEDS[i].slcanCode == code) {
      return applyCanSpeed(i);
    }
  }
  return false;
}

static ACAN2517FDSettings::OperationMode acanModeFor(AdapterMode mode) {
  if (mode == MODE_NORMAL) return ACAN2517FDSettings::Normal20B;
  if (mode == MODE_LISTEN_ONLY) return ACAN2517FDSettings::ListenOnly;
  if (mode == MODE_LOOPBACK) return ACAN2517FDSettings::InternalLoopBack;
  return ACAN2517FDSettings::Configuration;
}

static bool beginCanController(AdapterMode mode) {
  if (speedIndex >= CAN_SPEED_COUNT) return false;

  // ACAN2517FD applies oscillator/bit timing during begin(). Re-starting the
  // driver is the safe equivalent of changing MCP2515's configuration mode.
  if (canControllerStarted) {
    canController.end();
    canControllerStarted = false;
  }

  ACAN2517FDSettings settings(MCP2518_OSC,
                               CAN_SPEEDS[speedIndex].value,
                               DataBitRateFactor::x1);
  settings.mRequestedMode = acanModeFor(mode);
  settings.mDriverTransmitFIFOSize = 16;
  settings.mDriverReceiveFIFOSize = 32;
  settings.mControllerTransmitFIFORetransmissionAttempts =
      ACAN2517FDSettings::ThreeAttempts;

  const uint32_t errorCode = canController.begin(settings, [] {
    canController.isr();
  });
  if (errorCode != 0) {
    lastErrorFlags = static_cast<uint16_t>(errorCode & 0xFFFFU);
    return false;
  }

  canControllerStarted = true;
  canHardwareMode = settings.mRequestedMode;
  return true;
}

bool applyCanSpeed(byte index) {
  if (index >= CAN_SPEED_COUNT) return false;

  speedIndex = index;
  bitrateCode = CAN_SPEEDS[index].slcanCode;
  preferences.putUChar("speed", speedIndex);
  return adapterMode == MODE_CLOSED || beginCanController(adapterMode);
}

bool restoreMode() {
  if (adapterMode == MODE_CLOSED) {
    canHardwareMode = ACAN2517FDSettings::Configuration;
    return true;
  }
  return beginCanController(adapterMode);
}

bool applyAcceptanceFilter() {
  // The ACAN2517FD driver accepts filters through ACAN2517FDFilters. The
  // legacy SLCAN M/m commands are kept as a software filter so all six
  // historical MCP2515 filter slots retain the same user-visible behavior.
  return true;
}

void rememberFrame(const struct can_frame &frame, bool received) {
  lastFrameId = frame.can_id & (frame.can_id & CAN_EFF_FLAG ? 0x1FFFFFFF : 0x7FF);
  lastFrameDlc = frame.can_dlc & 0x0F;
  lastFrameWasRx = received;
  lastTimestamp = millis() & 0xFFFF;
  haveFrame = true;
  for (byte i = 0; i < 8; i++) lastFrameData[i] = frame.data[i];
}

void emitFrameTo(Print &output, const struct can_frame &frame) {
  bool extended = (frame.can_id & CAN_EFF_FLAG) != 0;
  bool remote = (frame.can_id & CAN_RTR_FLAG) != 0;
  unsigned long id = frame.can_id & (extended ? 0x1FFFFFFF : 0x7FF);

  output.write(remote ? (extended ? 'R' : 'r') : (extended ? 'T' : 't'));
  printHexTo(output, id, extended ? 8 : 3);
  output.write(HEX_DIGITS[frame.can_dlc & 0x0F]);

  if (!remote) {
    for (byte i = 0; i < frame.can_dlc && i < 8; i++) {
      printHexTo(output, frame.data[i], 2);
    }
  }

  if (timestampsEnabled) printHexTo(output, lastTimestamp, 4);
  output.write('\r');
}

void emitFrame(const struct can_frame &frame) {
  // Keep USB output compatible with the existing BusLume connection and mirror
  // every CAN frame to the current Bluetooth SPP client as well.  Use the
  // live connection state here; the OLED lock flag is only a UI state and can
  // otherwise become stale during a reconnect.
  emitFrameTo(Serial, frame);
  if (SerialBT.hasClient()) {
    emitFrameTo(SerialBT, frame);
    SerialBT.flush();
  }
}

static CANFDMessage toAcanMessage(const struct can_frame &frame) {
  CANFDMessage message;
  const bool extended = (frame.can_id & CAN_EFF_FLAG) != 0;
  const bool remote = (frame.can_id & CAN_RTR_FLAG) != 0;

  message.id = frame.can_id & (extended ? 0x1FFFFFFFUL : 0x7FFUL);
  message.ext = extended;
  message.type = remote ? CANFDMessage::CAN_REMOTE : CANFDMessage::CAN_DATA;
  message.len = frame.can_dlc > 8 ? 8 : frame.can_dlc;
  for (byte i = 0; i < message.len; ++i) message.data[i] = frame.data[i];
  return message;
}

static void fromAcanMessage(const CANFDMessage &message,
                            struct can_frame &frame) {
  frame = {};
  frame.can_id = message.id & (message.ext ? 0x1FFFFFFFUL : 0x7FFUL);
  if (message.ext) frame.can_id |= CAN_EFF_FLAG;
  if (message.type == CANFDMessage::CAN_REMOTE) frame.can_id |= CAN_RTR_FLAG;
  frame.can_dlc = message.len > 8 ? 8 : message.len;
  for (byte i = 0; i < frame.can_dlc; ++i) frame.data[i] = message.data[i];
}

static bool sendToAcan(const struct can_frame &frame) {
  if (!canControllerStarted || adapterMode != MODE_NORMAL) return false;
  CANFDMessage message = toAcanMessage(frame);
  return canController.tryToSend(message);
}

void rejectCommand(const char *status) {
  commandReply->write('\a');
  setStatus(status);
}

void processCommand(const char *command);

// Some Bluetooth serial terminals do not append CR/LF.  Return true only
// when the buffer contains one complete, unambiguous SLCAN command; the
// caller still waits briefly for the end of the Bluetooth packet so a
// fragmented command is not processed too early.
bool hostCommandComplete(const char *command, byte length) {
  if (length == 0) return false;

  char type = command[0];
  if (type == 'S' || type == 'Z') return length == 2;
  if (type == 'O' || type == 'L' || type == 'Q' || type == 'C' ||
      type == 'V' || type == 'N' || type == 'F') return length == 1;

  if (type == 'M' || type == 'm') return length == 4 || length == 9;

  if (type == 't' || type == 'T' || type == 'r' || type == 'R') {
    bool extended = type == 'T' || type == 'R';
    bool remote = type == 'r' || type == 'R';
    byte idDigits = extended ? 8 : 3;
    byte headerLength = 1 + idDigits + 1;
    if (length < headerLength) return false;

    int dlc = hexValue(command[1 + idDigits]);
    if (dlc < 0 || dlc > 8) return false;
    byte expected = headerLength + (remote ? 0 : (byte)(dlc * 2));
    return length == expected;
  }

  return false;
}

void commitHostCommand(HostProtocol protocol, char *buffer, byte &length) {
  if (length == 0) return;
  buffer[length] = '\0';
  selectCommandReply(protocol);
  processCommand(buffer);
  length = 0;
  commandReply = &Serial;
}

bool parseAndSendFrame(const char *command) {
  if (adapterMode != MODE_NORMAL) {
    rejectCommand("TX NOT READY");
    return false;
  }

  bool extended = command[0] == 'T' || command[0] == 'R';
  bool remote = command[0] == 'r' || command[0] == 'R';
  byte idDigits = extended ? 8 : 3;
  size_t headerLength = 1 + idDigits + 1;

  if (strlen(command) < headerLength) {
    rejectCommand("BAD TX");
    return false;
  }

  unsigned long id = 0;
  unsigned long dlc = 0;
  if (!parseHex(command + 1, idDigits, id) ||
      !parseHex(command + 1 + idDigits, 1, dlc) || dlc > 8) {
    rejectCommand("BAD TX");
    return false;
  }

  size_t expectedLength = headerLength + (remote ? 0 : dlc * 2);
  if (strlen(command) != expectedLength) {
    rejectCommand("BAD TX");
    return false;
  }

  struct can_frame frame = {};
  frame.can_id = id & (extended ? 0x1FFFFFFF : 0x7FF);
  if (extended) frame.can_id |= CAN_EFF_FLAG;
  if (remote) frame.can_id |= CAN_RTR_FLAG;
  frame.can_dlc = dlc;

  for (byte i = 0; i < frame.can_dlc && !remote; i++) {
    unsigned long dataByte = 0;
    if (!parseHex(command + headerLength + i * 2, 2, dataByte)) {
      rejectCommand("BAD TX");
      return false;
    }
    frame.data[i] = dataByte;
  }

  if (sendToAcan(frame)) {
    txCount++;
    rememberFrame(frame, false);
    commandReply->write('\r');
    setStatus("TX OK");
    return true;
  }

  lastErrorFlags = static_cast<uint16_t>(canController.errorCounters() & 0xFFFFU);
  errorCount++;
  rejectCommand("TX ERROR");
  return false;
}

void processCommand(const char *command) {
  if (command[0] == '\0') return;

  // SLCAN bitrate selection. S6 = 500 kbit/s.
  // It is accepted both before and after opening so the local UI can start
  // the CAN controller while BusLume can still perform its normal handshake.
  if (command[0] == 'S' && command[1] != '\0' && command[2] == '\0') {
    if (applyBitrate(command[1] - '0')) {
      commandReply->write('\r');
      setStatus("BITRATE OK");
    } else {
      rejectCommand("BAD BITRATE");
    }
    return;
  }

  // Open in normal, listen-only, or loopback mode.
  if (command[0] == 'O' || command[0] == 'L' || command[0] == 'Q') {
    // A host mode change always ends the local periodic sender. This avoids
    // continuing to transmit after BusLume requests listen-only or loopback.
    sendAllActive = false;
    AdapterMode requestedMode = command[0] == 'O' ? MODE_NORMAL :
                                command[0] == 'L' ? MODE_LISTEN_ONLY :
                                MODE_LOOPBACK;
    adapterMode = requestedMode;
    if (restoreMode()) {
      commandReply->write('\r');
      setStatus(command[0] == 'O' ? "OPEN" : command[0] == 'L' ? "LISTEN" : "LOOPBACK");
    } else {
      adapterMode = MODE_CLOSED;
      rejectCommand("OPEN ERROR");
    }
    return;
  }

  if (command[0] == 'C' && command[1] == '\0') {
    sendAllActive = false;
    if (canControllerStarted) {
      canController.end();
      canControllerStarted = false;
    }
    canHardwareMode = ACAN2517FDSettings::Configuration;
    adapterMode = MODE_CLOSED;
    commandReply->write('\r');
    // BusLume's close command ends a USB host session for the local UI.
    // For Bluetooth, hasClient() keeps the lock while the SPP link remains.
    clearActiveHost();
    setStatus("CLOSED");
    return;
  }

  if (command[0] == 't' || command[0] == 'T' ||
      command[0] == 'r' || command[0] == 'R') {
    parseAndSendFrame(command);
    return;
  }

  // Standard SLCAN acceptance code/mask. Use 3 hex digits for 11-bit CAN.
  if (command[0] == 'M' || command[0] == 'm') {
    size_t filterLength = strlen(command);
    byte filterDigits = filterLength == 4 ? 3 : filterLength == 9 ? 8 : 0;
    unsigned long value = 0;
    if (filterDigits == 0 || !parseHex(command + 1, filterDigits, value)) {
      rejectCommand("BAD FILTER");
      return;
    }
    // This firmware applies standard 11-bit filtering; for an 8-digit
    // command only the lower 11 bits are used.
    if (command[0] == 'M') filterCode = value & 0x7FF;
    else filterMask = value & 0x7FF;

    if (applyAcceptanceFilter()) {
      commandReply->write('\r');
      setStatus(filterMask ? "FILTER ON" : "FILTER OFF");
    } else {
      rejectCommand("FILTER ERROR");
    }
    return;
  }

  // Timestamp extension: Z0 disables, Z1 enables 4 hex timestamp after data.
  if (command[0] == 'Z' && (command[1] == '0' || command[1] == '1') && command[2] == '\0') {
    timestampsEnabled = command[1] == '1';
    commandReply->write('\r');
    setStatus(timestampsEnabled ? "TIME ON" : "TIME OFF");
    return;
  }

  if (command[0] == 'V' && command[1] == '\0') {
    commandReply->print("V0100\r");
    return;
  }

  if (command[0] == 'N' && command[1] == '\0') {
    commandReply->print("N0001\r");
    return;
  }

  // F returns a compact ACAN2517FD diagnostic/error value as four
  // hexadecimal digits for SLCAN compatibility.
  if (command[0] == 'F' && command[1] == '\0') {
    lastErrorFlags = canControllerStarted
                   ? static_cast<uint16_t>(canController.errorCounters() & 0xFFFFU)
                   : 0;
    commandReply->print("F");
    printHex(lastErrorFlags, 4);
    commandReply->write('\r');
    return;
  }

  rejectCommand("UNKNOWN CMD");
}

void readHostStream(Stream &input, HostProtocol protocol,
                    char *buffer, byte &length, unsigned long &lastByteAt) {
  while (input.available() > 0) {
    char c = (char)input.read();
    noteHostActivity(protocol);
    lastByteAt = millis();

    if (c == '\r' || c == '\n') {
      commitHostCommand(protocol, buffer, length);
    } else if (length < 79) {
      buffer[length++] = c;
    } else {
      length = 0;
      selectCommandReply(protocol);
      rejectCommand("CMD TOO LONG");
      commandReply = &Serial;
    }
  }

  if (protocol == HOST_BLUETOOTH && length > 0 &&
      hostCommandComplete(buffer, length) &&
      millis() - lastByteAt >= BT_COMMAND_IDLE_MS) {
    commitHostCommand(protocol, buffer, length);
  }
}

void readHostCommands() {
  readHostStream(Serial, HOST_USB, usbCommandBuffer, usbCommandLength,
                 usbLastByteAt);
  readHostStream(SerialBT, HOST_BLUETOOTH, bluetoothCommandBuffer,
                 bluetoothCommandLength, bluetoothLastByteAt);
  updateHostScreenState();
}

bool recoverCanController();

void checkMcpErrors() {
  if (adapterMode == MODE_CLOSED || !canControllerStarted) return;
  const uint16_t counters = static_cast<uint16_t>(canController.errorCounters() & 0xFFFFU);
  if (counters != 0) {
    if (counters != lastErrorFlags) ++errorCount;
    lastErrorFlags = counters;
    setStatus("CAN ERROR");
  }
}

bool recoverCanController() {
  if (adapterMode == MODE_CLOSED || millis() - lastRecovery < 1000) return false;
  lastRecovery = millis();
  setStatus("BUS RECOVERY");
  const bool recovered = beginCanController(adapterMode);
  if (recovered) lastErrorFlags = 0;
  return recovered;
}

bool pressedEdge(byte pin, bool &lastState, unsigned long &lastChange) {
  bool state = digitalRead(pin);
  unsigned long now = millis();

  if (state != lastState && now - lastChange >= 35) {
    lastChange = now;
    lastState = state;
    return state == LOW;
  }
  return false;
}

void changeSelectedField(int delta) {
  if (selectedField == 0) {
    int value = (int)canId + delta;
    if (value < 0) value = 0x7FF;
    if (value > 0x7FF) value = 0;
    canId = value;
  } else if (selectedField == 1) {
    int value = (int)canDlc + delta;
    if (value < 0) value = 8;
    if (value > 8) value = 0;
    canDlc = value;
  } else if (selectedField == 10) {
    long value = (long)currentPeriodMs + (long)delta * (long)PERIOD_STEP_MS;
    if (value < (long)MIN_PERIOD_MS) value = MAX_PERIOD_MS;
    if (value > (long)MAX_PERIOD_MS) value = MIN_PERIOD_MS;
    currentPeriodMs = (unsigned long)value;
  } else {
    byte index = selectedField - 2;
    int value = (int)canData[index] + delta;
    if (value < 0) value = 255;
    if (value > 255) value = 0;
    canData[index] = value;
  }
}

bool transmitLocalFrame(const struct can_frame &frame) {
  // If BusLume has closed the channel, reopen it for local OLED operation.
  if (adapterMode == MODE_CLOSED) {
    adapterMode = MODE_NORMAL;
    if (!beginCanController(adapterMode)) {
      adapterMode = MODE_CLOSED;
      setStatus("CAN OPEN ERROR");
      return false;
    }
  }

  if (adapterMode != MODE_NORMAL) {
    setStatus("LISTEN ONLY");
    return false;
  }

  if (sendToAcan(frame)) {
    txCount++;
    localShotCounter++;
    rememberFrame(frame, false);

    // Echo a successful OLED transmission to USB so BusLume can display it.
    // This does not echo failed frames and does not change host-command TX.
    emitFrame(frame);

    snprintf(statusLine, sizeof(statusLine), "OLED TX #%u", (unsigned)localShotCounter);
    return true;
  }

  errorCount++;
  lastErrorFlags = canControllerStarted
                 ? static_cast<uint16_t>(canController.errorCounters() & 0xFFFFU)
                 : 0;
  setStatus("OLED TX ERROR");
  return false;
}

void sendEditorFrame() {
  struct can_frame frame = {};
  frame.can_id = canId & 0x7FF;
  frame.can_dlc = canDlc;
  for (byte i = 0; i < 8; i++) frame.data[i] = canData[i];
  transmitLocalFrame(frame);
}

bool transmitSavedFrameAt(byte index) {
  if (index >= savedCount) return false;

  struct can_frame frame = {};
  frame.can_id = savedFrames[index].can_id & 0x7FF;
  frame.can_dlc = savedFrames[index].dlc;
  for (byte i = 0; i < 8; i++) frame.data[i] = savedFrames[index].data[i];
  return transmitLocalFrame(frame);
}

void sendSavedFrame() {
  if (savedCount == 0) {
    setStatus("NO SAVED FRAMES");
    return;
  }
  transmitSavedFrameAt(savedIndex);
}

void toggleSendAll() {
  bool currentIdMode = uiScreen == UI_EDITOR;

  if (!currentIdMode && savedCount == 0) {
    setStatus("NO SAVED FRAMES");
    return;
  }

  sendAllActive = !sendAllActive;
  unsigned long now = millis();

  if (sendAllActive) {
    if (currentIdMode) {
      // While the current frame is being sent periodically, lock the editor
      // cursor on PERIOD.  This prevents an accidental joystick movement from
      // changing the ID, DLC, or data bytes that are on the bus.
      selectedField = 10;
      lastXAction = now;
      lastYAction = now;
      // GPIO26 in the transmitter schedules the ID/Data currently visible in
      // the editor, using its currently selected period.
      lastCurrentTx = now - currentPeriodMs;
      setStatus("CURRENT ID ON");
    } else {
      for (byte i = 0; i < savedCount; i++) {
        lastSavedTx[i] = now - savedPeriods[i];
      }
      setStatus("SEND ALL ON");
    }
  } else {
    setStatus(currentIdMode ? "CURRENT ID OFF" : "SEND ALL OFF");
  }
}

void serviceSendAll() {
  if (!sendAllActive) return;
  unsigned long now = millis();

  if (uiScreen == UI_EDITOR) {
    if (now - lastCurrentTx >= currentPeriodMs) {
      // Send exactly the frame currently edited on the OLED, never a saved
      // frame. The scheduler advances even on a CAN error.
      sendEditorFrame();
      lastCurrentTx = now;
    }
    return;
  }

  if (savedCount == 0) {
    sendAllActive = false;
    return;
  }

  for (byte i = 0; i < savedCount; i++) {
    if (now - lastSavedTx[i] >= savedPeriods[i]) {
      // Advance the scheduler even when a frame fails. This keeps a missing
      // CAN acknowledgement from flooding the bus with retries.
      transmitSavedFrameAt(i);
      lastSavedTx[i] = now;
      if (adapterMode == MODE_LISTEN_ONLY) {
        sendAllActive = false;
        setStatus("SEND ALL STOP");
      }
    }
  }
}

void enterMenuItem() {
  if (menuIndex == 0) {
    uiScreen = UI_MONITOR;
    setStatus("CAN MONITOR");
  } else if (menuIndex == 1) {
    uiScreen = UI_EDITOR;
    setStatus("TX EDITOR");
  } else if (menuIndex == 2) {
    uiScreen = UI_SAVED;
    savedIndex = savedCount == 0 ? 0 : min(savedIndex, (byte)(savedCount - 1));
    setStatus(savedCount == 0 ? "NO SAVED FRAMES" : "SAVED FRAMES");
  }
}

void performPrimaryAction() {
  if (uiScreen == UI_MENU) enterMenuItem();
  else if (uiScreen == UI_MONITOR) setStatus("MONITOR ONLY");
  else if (uiScreen == UI_EDITOR) sendEditorFrame();
  else if (uiScreen == UI_SAVED) sendSavedFrame();
}

void changeMonitorSpeed(int delta) {
  int next = (int)speedIndex + delta;
  if (next < 0) next = CAN_SPEED_COUNT - 1;
  if (next >= CAN_SPEED_COUNT) next = 0;

  if (applyCanSpeed((byte)next)) {
    char message[21];
    snprintf(message, sizeof(message), "SPEED %s", bitrateText());
    setStatus(message);
  } else {
    setStatus("SPEED ERROR");
  }
}

void handleJoystick() {
  if (hostLockActive()) return;

  int x = analogRead(JOY_X_PIN);
  int y = analogRead(JOY_Y_PIN);
  unsigned long now = millis();
  int yDirection = joystickDirection(y);
  int xDirection = joystickDirection(x);
  unsigned long yRepeatDelay = joystickRepeatDelay(y);
  // X only selects a field. Keep its original fixed repeat speed so a
  // sideways movement cannot rapidly jump across ID/DLC/data fields.
  unsigned long xRepeatDelay = JOY_X_REPEAT_MS;

  if (uiScreen == UI_MENU) {
    if (yDirection != 0 && now - lastYAction >= yRepeatDelay) {
      // Main-menu navigation follows the physical joystick direction:
      // up goes to the previous item, down goes to the next item.
      if (yDirection > 0) {
        if (menuIndex > 0) menuIndex--;
        lastYAction = now;
      } else if (yDirection < 0) {
        if (menuIndex < 2) menuIndex++;
        lastYAction = now;
      }
    } else if (yDirection == 0) {
      lastYAction = now;
    }
    return;
  }

  if (uiScreen == UI_MONITOR) {
    if (yDirection != 0 && now - lastYAction >= yRepeatDelay) {
      // The Y axis is intentionally inverted: up = previous speed,
      // down = next speed.  The new rate is applied immediately.
      if (yDirection < 0) {
        changeMonitorSpeed(-1);
        lastYAction = now;
      } else if (yDirection > 0) {
        changeMonitorSpeed(1);
        lastYAction = now;
      }
    } else if (yDirection == 0) {
      lastYAction = now;
    }
    return;
  }

  if (uiScreen == UI_SAVED) {
    if (yDirection != 0 && now - lastYAction >= yRepeatDelay && savedCount > 0) {
      if (yDirection < 0) {
        if (savedIndex > 0) savedIndex--;
        lastYAction = now;
      } else if (yDirection > 0) {
        if (savedIndex + 1 < savedCount) savedIndex++;
        lastYAction = now;
      }
    } else if (yDirection == 0) {
      lastYAction = now;
    }
    return;
  }

  if (uiScreen != UI_EDITOR) return;

  if (sendAllActive) {
    // SEND ALL in the transmitter permits changing only the period.  X is
    // deliberately ignored until transmission is stopped.
    selectedField = 10;
    if (yDirection != 0 && now - lastYAction >= yRepeatDelay) {
      if (yDirection < 0) {
        changeSelectedField(-1);
        lastYAction = now;
      } else if (yDirection > 0) {
        changeSelectedField(1);
        lastYAction = now;
      }
    } else if (yDirection == 0) {
      lastYAction = now;
    }
    return;
  }

  // Left/right selects ID, DLC, or a data byte.
  if (xDirection != 0 && now - lastXAction >= xRepeatDelay) {
    if (xDirection < 0) {
      if (selectedField > 0) selectedField--;
      lastXAction = now;
    } else if (xDirection > 0) {
      if (selectedField < 10) selectedField++;
      lastXAction = now;
    }
  } else if (xDirection == 0) {
    lastXAction = now;
  }

  // Y axis is intentionally inverted: up decrements, down increments.
  if (yDirection != 0 && now - lastYAction >= yRepeatDelay) {
    if (yDirection < 0) {
      changeSelectedField(-1);
      lastYAction = now;
    } else if (yDirection > 0) {
      changeSelectedField(1);
      lastYAction = now;
    }
  } else if (yDirection == 0) {
    lastYAction = now;
  }
}

void handleShotButton() {
  bool state = digitalRead(BTN_SHOT_PIN);
  unsigned long now = millis();

  if (state != lastShotButton && now - lastShotButtonChange >= 35) {
    lastShotButtonChange = now;
    lastShotButton = state;

    if (state == LOW) {
      shotPressedAt = now;
    } else {
      unsigned long duration = now - shotPressedAt;
      if (uiScreen == UI_EDITOR) {
        // In the transmitter GPIO26 controls the periodic sender.
        // The current frame itself is sent by the joystick short press.
        if (duration >= 35) toggleSendAll();
      } else if (duration >= 35) {
        performPrimaryAction();
      }
    }
  }
}

void handleJoystickButton() {
  bool state = digitalRead(JOY_SW_PIN);
  unsigned long now = millis();

  if (state != lastJoyButton && now - lastJoyButtonChange >= 35) {
    lastJoyButtonChange = now;
    lastJoyButton = state;

    if (state == LOW) {
      joyIsPressed = true;
      joyPressedAt = now;
      joyLongHandled = false;
    } else if (joyIsPressed) {
      unsigned long duration = now - joyPressedAt;

      if (uiScreen == UI_EDITOR && !joyLongHandled && duration >= 1000) {
        saveCurrentFrame();
        joyLongHandled = true;
      } else if (uiScreen == UI_SAVED && !joyLongHandled && duration >= 1000) {
        deleteSavedFrame();
        joyLongHandled = true;
      } else if (uiScreen == UI_SAVED && !joyLongHandled && duration >= 35) {
        // A short press loads the selected saved frame into the transmitter.
        // GPIO26 remains the one-shot test button on this screen.
        copySavedToEditor();
        joyLongHandled = true;
      } else if (uiScreen == UI_EDITOR && !joyLongHandled && duration >= 35) {
        // Short press in the transmitter is SIGNAL SHOT. Other screens keep
        // their existing primary action.
        performPrimaryAction();
        joyLongHandled = true;
      } else if (uiScreen != UI_EDITOR && uiScreen != UI_SAVED && duration >= 35) {
        // Other screens keep their existing primary action.
        performPrimaryAction();
        joyLongHandled = true;
      }

      // A long action is triggered while the button is still held. The short
      // action is triggered after the release, unless the long action already
      // handled this press.
      if (state == HIGH) {
        joyIsPressed = false;
        joyPressedAt = 0;
        joyLongHandled = false;
      }
    }
  }

  // Debounced button transitions above run only on edges. Check the hold
  // threshold continuously so the user sees the confirmation immediately,
  // without guessing how long to keep the joystick pressed.
  if (joyIsPressed && !joyLongHandled && uiScreen == UI_EDITOR &&
      now - joyPressedAt >= 1000) {
    saveCurrentFrame();
    joyLongHandled = true;
  } else if (joyIsPressed && !joyLongHandled && uiScreen == UI_SAVED &&
             now - joyPressedAt >= 1000) {
    deleteSavedFrame();
    joyLongHandled = true;
  }
}

void handleLocalButtons() {
  if (hostLockActive()) {
    handleHostUnlock();
    return;
  }

  // GPIO25 is BACK in every local screen, including the transmitter.
  if (pressedEdge(BTN_MODE_PIN, lastModeButton, lastModeButtonChange)) {
    if (uiScreen != UI_MENU) sendAllActive = false;
    if (uiScreen != UI_MENU) {
      uiScreen = UI_MENU;
      setStatusForScreen();
    }
  }

  handleShotButton();

  handleJoystickButton();
}

void readCanFrames() {
  if (adapterMode == MODE_CLOSED || !canControllerStarted) return;

  for (byte i = 0; i < 32; i++) {
    CANFDMessage message;
    if (!canController.receive(message)) break;

    struct can_frame frame = {};
    fromAcanMessage(message, frame);
    const uint32_t plainId = frame.can_id &
      ((frame.can_id & CAN_EFF_FLAG) ? 0x1FFFFFFFUL : 0x7FFUL);
    if (filterMask != 0 && (plainId & filterMask) != (filterCode & filterMask)) {
      continue;
    }

    rxCount++;
    rememberFrame(frame, true);
    emitFrame(frame);
  }
  checkMcpErrors();
}

void drawSelectedText(const char *text, int x, int y, int width, bool selected) {
  if (selected) {
    display.drawBox(x - 2, y - 8, width, 10);
    display.setDrawColor(0);
  }
  display.drawStr(x, y, text);
  if (selected) display.setDrawColor(1);
}

void drawCenteredText(const char *text, int y) {
  int width = display.getStrWidth(text);
  display.drawStr((128 - width) / 2, y, text);
}

void drawSplash() {
  if (!oledOK) return;
  display.clearBuffer();
  display.drawFrame(0, 0, 128, 64);
  display.setFont(u8g2_font_ncenB18_tr);
  drawCenteredText("BusLume", 31);
  display.setFont(u8g2_font_5x7_tf);
  drawCenteredText("ESP32 CAN ADAPTER", 46);
  drawCenteredText("USB  |  BLUETOOTH", 57);
  display.sendBuffer();
}

void drawUsbIcon(int cx, int cy) {
  display.drawLine(cx, cy - 16, cx, cy + 13);
  display.drawLine(cx, cy - 16, cx - 5, cy - 10);
  display.drawLine(cx, cy - 16, cx + 5, cy - 10);
  display.drawLine(cx, cy - 3, cx + 13, cy - 3);
  display.drawLine(cx + 13, cy - 3, cx + 8, cy - 8);
  display.drawLine(cx + 13, cy - 3, cx + 8, cy + 2);
  display.drawBox(cx - 3, cy + 2, 6, 6);
  display.drawCircle(cx, cy + 14, 3);
}

void drawBluetoothIcon(int cx, int cy) {
  display.drawLine(cx, cy - 17, cx, cy + 17);
  display.drawLine(cx, cy - 17, cx + 10, cy - 8);
  display.drawLine(cx + 10, cy - 8, cx - 9, cy + 9);
  display.drawLine(cx - 9, cy - 9, cx + 10, cy + 8);
  display.drawLine(cx + 10, cy + 8, cx, cy + 17);
}

void drawHostConnectionScreen() {
  if (!oledOK) return;
  char line[24];

  display.clearBuffer();
  display.drawFrame(0, 0, 128, 64);

  if (usbHostActive && bluetoothHostActive) {
    drawUsbIcon(42, 24);
    drawBluetoothIcon(86, 24);
    display.setFont(u8g2_font_5x7_tf);
    drawCenteredText("USB + BLUETOOTH", 49);
  } else if (bluetoothHostActive) {
    drawBluetoothIcon(64, 23);
    display.setFont(u8g2_font_5x7_tf);
    drawCenteredText("BLUETOOTH CONNECTED", 49);
  } else {
    drawUsbIcon(64, 23);
    display.setFont(u8g2_font_5x7_tf);
    drawCenteredText("USB CONNECTED", 49);
  }

  display.setFont(u8g2_font_5x7_tf);
  snprintf(line, sizeof(line), "CAN RX:%lu TX:%lu", rxCount, txCount);
  drawCenteredText(line, 57);
  drawCenteredText("BTN25 HOLD 3s UNLOCK", 63);
}

void drawMenuScreen() {
  if (!oledOK) return;
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 8, "BUSLUME MAIN MENU");
  drawSelectedText("MONITOR", 12, 21, 112, menuIndex == 0);
  drawSelectedText("TRANSMITTER", 12, 33, 112, menuIndex == 1);
  drawSelectedText("SAVED FRAMES", 12, 45, 112, menuIndex == 2);
  display.drawStr(0, 63, "JOY U/D  SW SELECT");
}

void drawMonitorScreen() {
  if (!oledOK) return;
  char line[32];

  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 8, "CAN MONITOR");

  snprintf(line, sizeof(line), "MODE: %s", modeText());
  display.drawStr(0, 17, line);
  snprintf(line, sizeof(line), "SPEED: %s", bitrateText());
  display.drawStr(0, 27, line);

  snprintf(line, sizeof(line), "RX:%lu", rxCount);
  display.drawStr(0, 38, line);
  snprintf(line, sizeof(line), "TX:%lu", txCount);
  display.drawStr(43, 38, line);
  snprintf(line, sizeof(line), "ERR:%lu", errorCount);
  display.drawStr(86, 38, line);

  display.drawStr(0, 50, statusLine);
  display.drawStr(0, 63, "JOY Y SPEED   BTN25 BACK");
}

void drawEditorScreen() {
  if (!oledOK) return;
  char line[24];

  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 8, sendAllActive ? "TRANSMITTER [ID LOOP]" : "TRANSMITTER");

  display.drawStr(0, 18, "ID:");
  snprintf(line, sizeof(line), "%03X", (unsigned)canId);
  drawSelectedText(line, 15, 18, 24, selectedField == 0);

  display.drawStr(48, 18, "DLC:");
  snprintf(line, sizeof(line), "%u", (unsigned)canDlc);
  drawSelectedText(line, 68, 18, 10, selectedField == 1);

  for (byte i = 0; i < 4; i++) {
    int x = i * 32;
    snprintf(line, sizeof(line), "D%u:", (unsigned)i);
    display.drawStr(x, 30, line);
    snprintf(line, sizeof(line), "%02X", canData[i]);
    drawSelectedText(line, x + 14, 30, 14, selectedField == i + 2);
  }

  for (byte i = 4; i < 8; i++) {
    int x = (i - 4) * 32;
    snprintf(line, sizeof(line), "D%u:", (unsigned)i);
    display.drawStr(x, 42, line);
    snprintf(line, sizeof(line), "%02X", canData[i]);
    drawSelectedText(line, x + 14, 42, 14, selectedField == i + 2);
  }

  display.drawStr(0, 53, "P:");
  snprintf(line, sizeof(line), "%lums", currentPeriodMs);
  drawSelectedText(line, 14, 53, 48, selectedField == 10);

  display.drawStr(70, 53, statusLine);
  display.drawStr(0, 63, "J:S/L:SAVE 25:BACK 26:ID TX");
}

void drawSavedScreen() {
  if (!oledOK) return;
  char line[32];

  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 8, "SAVED FRAMES");

  if (savedCount == 0) {
    display.drawStr(0, 27, "NO SAVED FRAMES");
    display.drawStr(0, 45, "EDIT A FRAME THEN");
    display.drawStr(0, 54, "HOLD JOY TO SAVE");
  } else {
    snprintf(line, sizeof(line), "%u/%u S%03lX D%u P%lums",
             (unsigned)(savedIndex + 1), (unsigned)savedCount,
             (unsigned long)(savedFrames[savedIndex].can_id & 0x7FF),
             (unsigned)savedFrames[savedIndex].dlc,
             savedPeriods[savedIndex]);
    display.drawStr(0, 20, line);
    snprintf(line, sizeof(line), "%02X %02X %02X %02X",
             savedFrames[savedIndex].data[0], savedFrames[savedIndex].data[1],
             savedFrames[savedIndex].data[2], savedFrames[savedIndex].data[3]);
    display.drawStr(0, 32, line);
    snprintf(line, sizeof(line), "%02X %02X %02X %02X",
             savedFrames[savedIndex].data[4], savedFrames[savedIndex].data[5],
             savedFrames[savedIndex].data[6], savedFrames[savedIndex].data[7]);
    display.drawStr(0, 44, line);
    display.drawStr(0, 54, statusLine);
  }
  display.drawStr(0, 63, "U/D ID  J:S LOAD  J:L DEL  26 SHOT");
}

void drawFloppyIcon(int cx, int cy) {
  display.drawBox(cx - 15, cy - 17, 30, 34);
  display.setDrawColor(1);
  display.drawBox(cx - 10, cy - 17, 16, 9);
  display.drawBox(cx - 11, cy + 2, 22, 12);
  display.setDrawColor(0);
  display.drawFrame(cx - 11, cy + 2, 22, 12);
  display.setDrawColor(1);
}

void drawTrashIcon(int cx, int cy) {
  display.drawBox(cx - 12, cy - 10, 24, 4);
  display.drawBox(cx - 7, cy - 15, 14, 5);
  display.drawBox(cx - 10, cy - 6, 20, 22);
  display.setDrawColor(1);
  display.drawBox(cx - 5, cy - 3, 2, 15);
  display.drawBox(cx - 1, cy - 3, 2, 15);
  display.drawBox(cx + 3, cy - 3, 2, 15);
  display.setDrawColor(0);
}

void drawPopup() {
  if (popupIcon == POPUP_NONE) return;
  if (millis() >= popupUntil) {
    popupIcon = POPUP_NONE;
    return;
  }

  // Draw only the icon. The base screen and popup are sent together once,
  // preventing the OLED flash caused by two consecutive buffer transfers.
  display.setDrawColor(1);
  display.drawBox(37, 12, 54, 40);
  display.setDrawColor(0);
  display.drawFrame(38, 13, 52, 38);

  if (popupIcon == POPUP_SAVE) drawFloppyIcon(64, 32);
  else if (popupIcon == POPUP_DELETE) drawTrashIcon(64, 32);

  display.setDrawColor(1);
}

void drawScreen() {
  if (hostLockActive()) {
    drawHostConnectionScreen();
    display.sendBuffer();
    return;
  }
  if (uiScreen == UI_MENU) drawMenuScreen();
  else if (uiScreen == UI_MONITOR) drawMonitorScreen();
  else if (uiScreen == UI_EDITOR) drawEditorScreen();
  else if (uiScreen == UI_SAVED) drawSavedScreen();
  drawPopup();
  display.sendBuffer();
}

void setup() {
  Serial.begin(SERIAL_SPEED);
  SerialBT.begin(BLUETOOTH_NAME);
  delay(300);
  pinMode(INT_PIN, INPUT);
  pinMode(BTN_MODE_PIN, INPUT_PULLUP);
  pinMode(BTN_SHOT_PIN, INPUT_PULLUP);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  analogReadResolution(12);

  preferences.begin("buslume", false);
  prepareStorage();
  speedIndex = preferences.getUChar("speed", DEFAULT_SPEED_INDEX);
  if (speedIndex >= CAN_SPEED_COUNT) speedIndex = DEFAULT_SPEED_INDEX;
  bitrateCode = CAN_SPEEDS[speedIndex].slcanCode;

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  byte oledAddress = findOLED();
  if (oledAddress != 0) {
    display.setI2CAddress(oledAddress << 1);
    display.begin();
    oledOK = true;
  }

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  loadFramesFromPreferences();

  // Start locally in normal CAN 2.0B mode. BusLume can still send S6/O over
  // USB or Bluetooth and the driver will be restarted with the new settings.
  adapterMode = MODE_NORMAL;
  if (!beginCanController(adapterMode)) {
    adapterMode = MODE_CLOSED;
    setStatus("CAN INIT ERROR");
  } else {
    setStatus("LOCAL READY");
  }
  drawSplash();
  delay(2000);
  drawScreen();
}

void loop() {
  readHostCommands();
  updateHostScreenState();
  handleLocalButtons();
  handleJoystick();
  serviceSendAll();
  readCanFrames();
  if (millis() - lastDisplay >= 200) {
    lastDisplay = millis();
    drawScreen();
  }
}

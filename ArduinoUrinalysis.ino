// ============================================
// ARDUINO URINALYSIS DEVICE
// Device ID: URINE-TEST-001
// Version: 1.0
// ============================================

#include "Menu.h"
#include "Keypad.h"
#include "Bluetooth.h"
#include "pHSensor.h"
#include "colourSensor.h"
#include "tdsSensor.h"
#include "cameraSensor.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <ArduinoBLE.h>
#include <ArduinoJson.h>

#define DEVICE_NAME    "URINE-TEST-001"
#define DEVICE_VERSION "1.0"
#define DEVICE_TYPE    "Urinalysis Analyzer"

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ============================================
// GLOBAL SCREEN STATE
// Flags set by menu callbacks; loop() detects them and hands off
// to the appropriate full-screen function. This avoids any flicker
// that would occur if we called the screen functions directly from
// a menu action (menuSelect() would still draw one menu frame first).
// ============================================

static bool inBTSettings   = false;
static bool inPHCal        = false;
static bool inRGBCal       = false;
static bool inTDSCal       = false;
static bool inCameraCal    = false;
static bool inBothColorCal = false;
static bool inIlluminatorAdjust  = false;
static bool inIlluminator2Adjust = false;   // secondary illuminator (D10)
static bool inDevDiagnostics = false;   // hidden developer live-readings screen

// ============================================
// HIDDEN DEVELOPER UNLOCK
// ============================================
// Five consecutive presses of key 16 (bottom-right of the 4x4 keypad)
// while on the Main Menu opens the Developer Diagnostics screen.
// Any other key resets the counter. The combo is not surfaced in any
// hint text — devs unlock it by knowing the magic key.
//
// To remove this entirely (e.g. for a production build), delete this
// counter, its bump/reset in loop(), and the runDevDiagnosticsScreen()
// function. The screen will become unreachable without altering any
// user-visible menu.
static const uint8_t DEV_UNLOCK_KEY    = 16;
static const uint8_t DEV_UNLOCK_COUNT  = 3;
static uint8_t       devUnlockProgress = 0;

// ============================================
// FORWARD DECLARATIONS
// ============================================

void startTest();
void openSettings();
void backToMain();

void openSensors();
// Color Sensors (umbrella: RGB + Camera)
void openColorSensors();
void openBothColorCalibration();
void backToColorSensors();
// pH
void openPH();
void openPHCalibration();
void resetPHCalibration();
// RGB
void openRGB();
void openRGBCalibration();
void resetRGBCalibration();
void openIlluminatorAdjust();
void openIlluminator2Adjust();
// TDS
void openTDS();
void openTDSCalibration();
void resetTDSCalibration();
// Camera (ESP32-CAM)
void openCamera();
void openCameraCalibration();
void resetCameraCalibration();

void openBluetoothSettings();

// Screen runners
void runCalibrationScreen();
void runRGBCalibrationScreen();
void runTDSCalibrationScreen();
void runCameraCalibrationScreen();
void runBothColorCalibrationScreen();
void runBluetoothSettingsScreen();
void runIlluminatorAdjustScreen();
void runIlluminator2AdjustScreen();
void runDevDiagnosticsScreen();   // hidden developer live-readings screen

// Universal back navigation (key 8 from any menu)
void goBackUniversal();

// ============================================
// MENUS
// ============================================

Menu mainMenu = {
  "Main Menu",
  {
    {"Start Test", startTest},
    {"Settings",   openSettings},
  },
  2
};

Menu settingsMenu = {
  "Settings",
  {
    {"Bluetooth",  openBluetoothSettings},
    {"Sensors",    openSensors},
    {"Back",       backToMain},
  },
  3
};

Menu sensorsMenu = {
  "Sensors",
  {
    {"pH Sensor",          openPH},
    {"Color Sensors",      openColorSensors},
    {"TDS Sensor",         openTDS},
    {"Back to Main Menu",  backToMain},
  },
  4
};

// ---- Color Sensors umbrella ----
// Groups the two colour-related sensors (RGB / TCS34725 and Camera /
// ESP32-CAM) under one menu, and adds a "Calibrate Both" option that
// walks the dark/white sequence for both sensors in lockstep.
//
// "Back" returns to the Sensors menu (parent), not all the way to main —
// this matches the menu hierarchy and keeps navigation predictable.
Menu colorSensorsMenu = {
  "Color Sensors",
  {
    {"RGB Sensor",         openRGB},
    {"Camera Sensor",      openCamera},
    {"Calibrate Both",     openBothColorCalibration},
    {"Back",               openSensors},
  },
  4
};

Menu pHMenu = {
  "pH Sensor",
  {
    {"pH Calibrate",       openPHCalibration},
    {"pH Cal Reset",       resetPHCalibration},
    {"Back to Main Menu",  backToMain},
  },
  3
};

Menu RGBMenu = {
  "RGB Sensor",
  {
    {"RGB Calibrate",      openRGBCalibration},
    {"Light 1 Adjust",     openIlluminatorAdjust},
    {"Light 2 Adjust",     openIlluminator2Adjust},
    {"RGB Cal Reset",      resetRGBCalibration},
    {"Back",               backToColorSensors},
  },
  5
};

Menu TDSMenu = {
  "TDS Sensor",
  {
    {"TDS Calibrate",      openTDSCalibration},
    {"TDS Cal Reset",      resetTDSCalibration},
    {"Back to Main Menu",  backToMain},
  },
  3
};

Menu cameraMenu = {
  "Camera Sensor",
  {
    {"Camera Calibrate",   openCameraCalibration},
    {"Camera Cal Reset",   resetCameraCalibration},
    {"Back",               backToColorSensors},
  },
  3
};

// ============================================
// CHARACTER SET FOR TEXT EDITOR
// ============================================
//
// Keypad controls inside the text editor:
//   UP   (key  2) — previous character
//   DOWN (key 10) — next character
//   SEL  (key 15) — append current character
//   key 16        — backspace
//   key  4        — DONE / save field
//   key  8        — CANCEL / discard
//
static const char CHAR_SET[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "0123456789 -_.";
static const int CHAR_SET_LEN = sizeof(CHAR_SET) - 1;

// ============================================
// TEXT EDITOR
// ============================================

/**
 * On-device single-line text editor.
 * Edits `buffer` (of length `maxLen`) in-place.
 * Returns true if the user confirmed, false if cancelled.
 */
bool runTextEditor(const char* title, char* buffer, uint8_t maxLen) {
  char working[BLE_NAME_MAX_LEN];
  strncpy(working, buffer, maxLen);
  working[maxLen - 1] = '\0';

  int charIdx = 0;
  int strLen  = strlen(working);

  while (true) {
    char display[BLE_NAME_MAX_LEN + 2];
    strncpy(display, working, strLen);
    display[strLen]     = CHAR_SET[charIdx];
    display[strLen + 1] = '\0';

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, title);
    u8g2.drawHLine(0, 12, 128);
    u8g2.drawStr(0, 28, display);
    u8g2.drawHLine(0, 30, 128);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 42, "UP/DN:char  SEL:add");
    u8g2.drawStr(0, 52, "key4:save  key16:del");
    u8g2.drawStr(0, 62, "key8:cancel");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if      (key == 2)  { charIdx = (charIdx - 1 + CHAR_SET_LEN) % CHAR_SET_LEN; }
    else if (key == 10) { charIdx = (charIdx + 1) % CHAR_SET_LEN; }
    else if (key == 15) {
      if (strLen < maxLen - 1) {
        working[strLen++] = CHAR_SET[charIdx];
        working[strLen]   = '\0';
      }
    }
    else if (key == 16) {
      if (strLen > 0) working[--strLen] = '\0';
    }
    else if (key == 4) {
      strncpy(buffer, working, maxLen);
      buffer[maxLen - 1] = '\0';
      return true;
    }
    else if (key == 8) {
      return false;
    }

    delay(120);
  }
}

// ============================================
// TX POWER SELECTOR
// ============================================

static const int8_t TX_LEVELS[] = { -40, -20, -16, -12, -8, -4, 0, 4 };
static const int    TX_COUNT    = sizeof(TX_LEVELS) / sizeof(TX_LEVELS[0]);

void runTxPowerSelector() {
  int idx = 0;
  for (int i = 0; i < TX_COUNT; i++) {
    if (TX_LEVELS[i] == bleSettings.txPower) { idx = i; break; }
  }

  while (true) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "TX Power (dBm)");
    u8g2.drawHLine(0, 12, 128);

    for (int i = 0; i < TX_COUNT; i++) {
      int y = 26 + i * 11;
      if (y > 63) break;

      char buf[16];
      snprintf(buf, sizeof(buf), "%4d dBm", TX_LEVELS[i]);

      if (i == idx) {
        u8g2.drawBox(0, y - 9, 128, 11);
        u8g2.setDrawColor(0);
        u8g2.drawStr(4, y, buf);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(4, y, buf);
      }
    }
    u8g2.sendBuffer();

    int key = scanKey();
    if (key == 2)  { idx = (idx - 1 + TX_COUNT) % TX_COUNT; }
    if (key == 10) { idx = (idx + 1) % TX_COUNT; }
    if (key == 15) {
      bleSettings.txPower = TX_LEVELS[idx];
      bleSaveSettings();
      bleApplySettings();

      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      char buf[24];
      snprintf(buf, sizeof(buf), "TX: %d dBm saved", bleSettings.txPower);
      u8g2.drawStr(0, 32, buf);
      u8g2.sendBuffer();
      delay(1200);
      return;
    }
    if (key == 8) return;

    delay(120);
  }
}

// ============================================
// BLUETOOTH SETTINGS SCREEN (SIMPLE)
// ============================================
//
// Three options:
//   0 — Advertising: ON / OFF  (toggle)
//   1 — Name: <current name>   (text editor)
//   2 — Advanced Settings      (opens advanced screen)
//
// key8 navigates back at any time (shown in hint).
//
void runBluetoothAdvancedScreen();   // forward declaration

void runBluetoothSettingsScreen() {
  static const int N = 3;
  int cursor = 0;

  while (true) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "BT Settings");
    u8g2.drawHLine(0, 12, 128);

    for (int i = 0; i < N; i++) {
      int y = 26 + i * 13;   // 13px spacing: items at y=26, 39, 52

      char line[32] = "";
      switch (i) {
        case 0: snprintf(line, sizeof(line), "BT:  %s",
                         bleSettings.advertisingEnabled ? "ON" : "OFF");  break;
        case 1: snprintf(line, sizeof(line), "Name: %.12s", bleSettings.localName); break;
        case 2: snprintf(line, sizeof(line), "Advanced Settings");               break;
      }

      if (i == cursor) {
        u8g2.drawBox(0, y - 10, 128, 12);
        u8g2.setDrawColor(0);
        u8g2.drawStr(4, y, line);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(4, y, line);
      }
    }
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 63, "UP/DN:move SEL:pick key8:back");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 2)  { cursor = (cursor - 1 + N) % N; delay(120); continue; }
    if (key == 10) { cursor = (cursor + 1) % N;      delay(120); continue; }
    if (key == 8)  { setMenu(&settingsMenu); return; }

    if (key == 15) {
      bool changed = false;

      switch (cursor) {
        case 0:
          bleSettings.advertisingEnabled = !bleSettings.advertisingEnabled;
          changed = true;
          break;
        case 1:
          changed = runTextEditor("Edit BT Name", bleSettings.localName, BLE_NAME_MAX_LEN);
          break;
        case 2:
          runBluetoothAdvancedScreen();
          // After returning from advanced, stay in this simple screen
          break;
      }

      if (changed) {
        bleSaveSettings();
        bleApplySettings();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(16, 32, "BT Settings Saved");
        u8g2.sendBuffer();
        delay(1200);
      }
    }

    delay(120);
  }
}

// ============================================
// BLUETOOTH ADVANCED SETTINGS SCREEN
// ============================================

void runBluetoothAdvancedScreen() {
  static const int N = 7;
  int cursor = 0;

  while (true) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 7, "BT Advanced  (key8=back)");
    u8g2.drawHLine(0, 9, 128);

    for (int i = 0; i < N; i++) {
      int y = 20 + i * 10;
      if (y > 63) break;

      char line[32] = "";
      switch (i) {
        case 0: snprintf(line, sizeof(line), "Name:  %.13s", bleSettings.localName);    break;
        case 1: snprintf(line, sizeof(line), "Mfr:   %.13s", bleSettings.manufacturer); break;
        case 2: snprintf(line, sizeof(line), "Model: %.13s", bleSettings.modelNumber);  break;
        case 3: snprintf(line, sizeof(line), "Adv:   %s",
                         bleSettings.advertisingEnabled ? "ON" : "OFF");                break;
        case 4: snprintf(line, sizeof(line), "TX:    %d dBm", bleSettings.txPower);     break;
        case 5: snprintf(line, sizeof(line), "[Reset to defaults]");                    break;
        case 6: snprintf(line, sizeof(line), "[Back]");                                 break;
      }

      if (i == cursor) {
        u8g2.drawBox(0, y - 7, 128, 9);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, line);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(2, y, line);
      }
    }
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 2)  { cursor = (cursor - 1 + N) % N; delay(120); continue; }
    if (key == 10) { cursor = (cursor + 1) % N;      delay(120); continue; }
    if (key == 8)  { return; }

    if (key == 15) {
      bool changed = false;

      switch (cursor) {
        case 0: changed = runTextEditor("Edit BT Name",   bleSettings.localName,    BLE_NAME_MAX_LEN); break;
        case 1: changed = runTextEditor("Edit Mfr Name",  bleSettings.manufacturer, BLE_NAME_MAX_LEN); break;
        case 2: changed = runTextEditor("Edit Model No.", bleSettings.modelNumber,  BLE_NAME_MAX_LEN); break;
        case 3:
          bleSettings.advertisingEnabled = !bleSettings.advertisingEnabled;
          changed = true;
          break;
        case 4:
          runTxPowerSelector();
          break;
        case 5:
          bleResetToDefaults();
          bleApplySettings();
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(4, 32, "BT reset to defaults");
          u8g2.sendBuffer();
          delay(1400);
          break;
        case 6:
          return;
      }

      if (changed) {
        bleSaveSettings();
        bleApplySettings();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(16, 32, "BT Settings Saved");
        u8g2.sendBuffer();
        delay(1200);
      }
    }

    delay(120);
  }
}

// ============================================
// pH CALIBRATION SCREEN
// ============================================
//
// Layout:
//   y=10  Title
//   y=12  Divider
//   y=24  Step instruction
//   y=36  Live voltage reading
//   y=50  Action hint
//   y=62  Cancel hint
//
// Controls:
//   SELECT (15)  — capture / confirm save
//   UP/DN (2/10) — cancel at any point
//
void runCalibrationScreen() {
  while (true) {
    float liveV = pHReadVoltage();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- pH Calibration --");
    u8g2.drawHLine(0, 12, 128);
    u8g2.drawStr(0, 24, calStepLabel());

    if (calStep != CAL_DONE) {
      char vBuf[20];
      snprintf(vBuf, sizeof(vBuf), "Live: %.4f V", liveV);
      u8g2.drawStr(0, 36, vBuf);
      u8g2.drawStr(0, 50, "SELECT when stable");
    } else {
      u8g2.drawStr(0, 36, "All points captured.");
      u8g2.drawStr(0, 50, "SELECT = Save");
    }

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 62, "UP/DN = Cancel");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 15) {
      if (calStep == CAL_DONE) {
        calSave();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(20, 32, "Cal Saved!");
        u8g2.sendBuffer();
        delay(1500);
        break;
      } else {
        calCapture();
      }
    } else if (key == 2 || key == 10) {
      calCancel();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(28, 32, "Cal Cancelled");
      u8g2.sendBuffer();
      delay(1200);
      break;
    }

    delay(80);
  }

  inPHCal = false;
  setMenu(&pHMenu);
}

// ============================================
// RGB CALIBRATION SCREEN
// ============================================
//
// Two-step sequence: DARK → WHITE → DONE
//
// Layout:
//   y=10  Title
//   y=12  Divider
//   y=22  Step instruction
//   y=34  Live R G B (raw counts)
//   y=44  Live C (clear channel)
//   y=56  Action hint
//
// Controls:
//   SELECT (15)      — capture / confirm save
//   UP/DN/key8       — cancel at any point
//
void runRGBCalibrationScreen() {
  // Illumination is step-gated: during the DARK reference step every light
  // (external D9/D10 illuminator and the AS7341 on-board LED) is OFF so the
  // sensor measures true darkness. During the WHITE reference step (and while
  // waiting to save afterwards) only the AS7341's on-board LED is lit; the
  // external LEDs stay off so the colour sensor sees a single clean illuminant.
  // Lights are restored to idle on every exit path at the bottom of this fn.

  while (true) {
    // Drive illumination from the current calibration step:
    //   COLOR_CAL_DARK  → all off  (true dark reference)
    //   COLOR_CAL_WHITE → on-board LED on, external off (AS7341's own light)
    //   COLOR_CAL_DONE  → on-board LED on (keep lit while user confirms save)
    if (colorCalStep == COLOR_CAL_DARK) {
      illuminatorOff();
      colorOnboardLedOff();
    } else {
      illuminatorOn();       // external LEDs stay off; AS7341 uses its own LED
      colorOnboardLedOn();
    }

    RawRGBC live = colorReadRaw();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- RGB Calibration -");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 22, colorCalStepLabel());

    char rBuf[22], cBuf[22];
    snprintf(rBuf, sizeof(rBuf), "R:%-5u G:%-5u B:%-5u", live.r, live.g, live.b);
    snprintf(cBuf, sizeof(cBuf), "C:%-5u", live.c);
    u8g2.drawStr(0, 34, rBuf);
    u8g2.drawStr(0, 44, cBuf);

    if (colorCalStep == COLOR_CAL_DONE) {
      u8g2.drawStr(0, 56, "SEL=Save  UP/DN=Cancel");
    } else {
      u8g2.drawStr(0, 56, "SEL=Capture UP/DN=Cncl");
    }

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 15) {
      if (colorCalStep == COLOR_CAL_DONE) {
        colorCalSave();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(20, 26, "RGB Cal Saved!");
        char wBuf[22], dBuf[22];
        snprintf(wBuf, sizeof(wBuf), "W R:%u G:%u",
                 colorCalData.white.r, colorCalData.white.g);
        snprintf(dBuf, sizeof(dBuf), "D R:%u G:%u",
                 colorCalData.dark.r,  colorCalData.dark.g);
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(0, 42, wBuf);
        u8g2.drawStr(0, 52, dBuf);
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.sendBuffer();
        delay(2000);
        break;
      } else {
        colorCalCapture();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        if (colorCalStep == COLOR_CAL_WHITE) {
          u8g2.drawStr(10, 32, "Dark captured!");
        } else if (colorCalStep == COLOR_CAL_DONE) {
          u8g2.drawStr(10, 32, "White captured!");
        }
        u8g2.sendBuffer();
        delay(900);
      }
    } else if (key == 2 || key == 10 || key == 8) {
      colorCalCancel();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(20, 32, "RGB Cal Cancelled");
      u8g2.sendBuffer();
      delay(1200);
      break;
    }

    delay(80);
  }

  // ---- Restore idle illumination ----
  // Every exit path (save / cancel) reaches here: the AS7341's on-board LED
  // goes back off and the external LEDs back on (the idle/menu light), so the
  // device is never left dark.
  colorOnboardLedOff();
  illuminatorOn();

  inRGBCal = false;
  setMenu(&RGBMenu);
}

// ============================================
// ILLUMINATOR ADJUST SCREEN
// ============================================
//
// Lets the user tune the brightness of the white-LED illuminator. The
// LED stays on continuously inside this screen so the change is
// visible immediately on the sample.
//
// Layout:
//   y=10  Title
//   y=12  Divider
//   y=24  "Brightness: xxx"
//   y=44  Bar showing duty (0..255 -> 0..126 px)
//   y=58  "UP/DN=adjust"
//   y=64  "SEL=save  k8=cancel"
//
// Controls:
//   UP   (key  2) — increase brightness
//   DOWN (key 10) — decrease brightness
//   SELECT (15)  — save to EEPROM and exit
//   key  8       — cancel (revert to stored value) and exit
//
// Step size is 5 per press; hold-to-repeat is not implemented, the
// 120 ms scan loop naturally rate-limits to ~8 changes/sec which is
// fine for tuning by eye.
void runIlluminatorAdjustScreen() {
  // Snapshot the previously-saved value so cancel can revert.
  uint8_t origBrightness = illuminatorGetBrightness();

  // Apply current stored value immediately so the user starts from
  // the live state, not from "off".
  illuminatorSetBrightness(origBrightness);

  const uint8_t STEP = 5;

  while (true) {
    uint8_t brightness = illuminatorGetBrightness();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- Light Adjust ----");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);

    char line[24];
    snprintf(line, sizeof(line), "Brightness: %u", brightness);
    u8g2.drawStr(0, 24, line);

    // Bar: 0..255 mapped to 0..126 px, with a 1-px frame border.
    int barW = (int)((long)brightness * 126L / 255L);
    u8g2.drawFrame(0, 36, 128, 8);
    if (barW > 0) {
      u8g2.drawBox(1, 37, barW, 6);
    }

    u8g2.drawStr(0, 58, "UP/DN=adjust");
    u8g2.drawStr(0, 64, "SEL=save  k8=cancel");

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 2) {
      // UP — increase
      int v = (int)brightness + STEP;
      if (v > 255) v = 255;
      illuminatorSetBrightness((uint8_t)v);
    } else if (key == 10) {
      // DOWN — decrease
      int v = (int)brightness - STEP;
      if (v < 0) v = 0;
      illuminatorSetBrightness((uint8_t)v);
    } else if (key == 15) {
      // Save and exit
      colorCalSaveIlluminator();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(8, 26, "Light settings");
      u8g2.drawStr(28, 40, "saved.");
      u8g2.sendBuffer();
      delay(1200);
      break;
    } else if (key == 8) {
      // Cancel — revert and exit
      illuminatorSetBrightness(origBrightness);
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(8, 32, "Light: cancelled");
      u8g2.sendBuffer();
      delay(1000);
      break;
    }

    delay(120);
  }

  // White LED illuminator left on — always-on mode.

  inIlluminatorAdjust = false;
  setMenu(&RGBMenu);
}

// ============================================
// ILLUMINATOR 2 ADJUST SCREEN  (D10 — secondary LED)
// ============================================
//
// Mirrors runIlluminatorAdjustScreen() exactly but operates on the
// secondary illuminator (D10) through illuminator2SetBrightness() and
// colorCalSaveIlluminator2(). Both LEDs remain on during this screen
// so the user can judge the combined effect while tuning D10 in isolation.
//
void runIlluminator2AdjustScreen() {
  uint8_t origBrightness = illuminator2GetBrightness();
  illuminator2SetBrightness(origBrightness);

  const uint8_t STEP = 5;

  while (true) {
    uint8_t brightness = illuminator2GetBrightness();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- Light 2 Adjust --");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);

    char line[24];
    snprintf(line, sizeof(line), "Brightness: %u", brightness);
    u8g2.drawStr(0, 24, line);

    // Bar: 0..255 mapped to 0..126 px, with a 1-px frame border.
    int barW = (int)((long)brightness * 126L / 255L);
    u8g2.drawFrame(0, 36, 128, 8);
    if (barW > 0) {
      u8g2.drawBox(1, 37, barW, 6);
    }

    u8g2.drawStr(0, 58, "UP/DN=adjust");
    u8g2.drawStr(0, 64, "SEL=save  k8=cancel");

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 2) {
      int v = (int)brightness + STEP;
      if (v > 255) v = 255;
      illuminator2SetBrightness((uint8_t)v);
    } else if (key == 10) {
      int v = (int)brightness - STEP;
      if (v < 0) v = 0;
      illuminator2SetBrightness((uint8_t)v);
    } else if (key == 15) {
      colorCalSaveIlluminator2();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(8, 26, "Light 2 settings");
      u8g2.drawStr(28, 40, "saved.");
      u8g2.sendBuffer();
      delay(1200);
      break;
    } else if (key == 8) {
      illuminator2SetBrightness(origBrightness);
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(8, 32, "Light 2: cancelled");
      u8g2.sendBuffer();
      delay(1000);
      break;
    }

    delay(120);
  }

  // Secondary LED left on alongside primary — always-on mode.

  inIlluminator2Adjust = false;
  setMenu(&RGBMenu);
}

// ============================================
// TDS CALIBRATION SCREEN
// ============================================
//
// Two-step sequence: LOW (84 uS/cm) → HIGH (1413 uS/cm) → DONE
//
// Calibration is done in EC space (uS/cm) against standard KCl reference
// solutions. TDS (ppm) is derived from EC at read time using the global
// 0.5 conversion factor — this matches the way certified reference
// solutions are actually labelled.
//
// Layout:
//   y=10  Title
//   y=12  Divider
//   y=24  Step instruction (which solution to use right now)
//   y=36  Live voltage reading (watch it settle before capturing)
//   y=48  Live EC estimate using current calibration
//   y=62  Action hint
//
// Controls:
//   SELECT (15)  — capture / confirm save
//   UP/DN (2/10) — cancel at any point
//
void runTDSCalibrationScreen() {
  // ---- Power-on for the duration of the screen ----
  // Live calibration loops at ~3 Hz; if every iteration triggered the
  // auto on/settle/read/off cycle inside tdsReadVoltage(), we'd burn
  // 400 ms per frame on settle alone and thrash the MOSFET. Pin the
  // probe ON here, and tdsReadVoltage() will detect "already powered"
  // and just read without toggling. Make sure EVERY exit path calls
  // tdsPowerOff() — otherwise we'd leak power state out to the menu.
  tdsPowerOnAndSettle();

  while (true) {
    float liveV  = tdsReadVoltage();
    // Show live EC (uS/cm) — that's what we're calibrating against, so
    // the user can confirm the live reading is approaching the expected
    // reference value (84 or 1413 uS/cm) before capturing.
    float liveEC = voltageToEC(liveV, pHReadTemperature());

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- TDS Calibration -");
    u8g2.drawHLine(0, 12, 128);
    u8g2.drawStr(0, 24, tdsCalStepLabel());

    if (tdsCalStep != TDS_CAL_DONE) {
      char vBuf[22], eBuf[22];
      snprintf(vBuf, sizeof(vBuf), "Live: %.4f V", liveV);
      snprintf(eBuf, sizeof(eBuf), "Est:  %.0f uS/cm", liveEC);
      u8g2.drawStr(0, 36, vBuf);
      u8g2.drawStr(0, 48, eBuf);
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(0, 62, "SELECT when stable  UP=Cncl");
    } else {
      u8g2.drawStr(0, 36, "Both points captured.");
      u8g2.drawStr(0, 48, "SELECT = Save");
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(0, 62, "UP/DN = Cancel");
    }

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 15) {
      if (tdsCalStep == TDS_CAL_DONE) {
        tdsCalSave();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(20, 26, "TDS Cal Saved!");
        char lBuf[22], hBuf[22];
        snprintf(lBuf, sizeof(lBuf), "Lo %.0fuS@%.3fV",
                 tdsCalData.low.ec,  tdsCalData.low.voltage);
        snprintf(hBuf, sizeof(hBuf), "Hi %.0fuS@%.3fV",
                 tdsCalData.high.ec, tdsCalData.high.voltage);
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(0, 42, lBuf);
        u8g2.drawStr(0, 52, hBuf);
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.sendBuffer();
        delay(2000);
        break;
      } else {
        tdsCalCapture();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        if (tdsCalStep == TDS_CAL_HIGH) {
          u8g2.drawStr(0, 32, "84 uS/cm captured!");
        } else if (tdsCalStep == TDS_CAL_DONE) {
          u8g2.drawStr(0, 32, "1413 uS/cm captured!");
        }
        u8g2.sendBuffer();
        delay(900);
      }
    } else if (key == 2 || key == 10) {
      tdsCalCancel();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(20, 32, "TDS Cal Cancelled");
      u8g2.sendBuffer();
      delay(1200);
      break;
    }

    delay(80);
  }

  // ---- De-energise probe on exit ----
  // Reached via every exit path above (save / cancel). Keeps the probe
  // dark whenever we're not actively in this screen.
  tdsPowerOff();

  inTDSCal = false;
  setMenu(&TDSMenu);
}

// ============================================
// CAMERA CALIBRATION SCREEN  (ESP32-CAM)
// ============================================
//
// Two-step sequence: DARK → WHITE → DONE
//
// The ESP32-CAM does the actual frame capture, ROI averaging, and
// persistence. This screen just drives the state machine and shows the
// last raw averaged values so the operator can sanity-check the capture.
//
// Layout:
//   y=10  Title
//   y=12  Divider
//   y=22  Step instruction
//   y=34  Last captured RGB (or "live" hint while idle)
//   y=46  Connection status
//   y=58  Action hint
//
// Controls:
//   SELECT (15)      — capture / confirm save
//   UP/DN/key8       — cancel at any point
//
void runCameraCalibrationScreen() {
  // Illumination is step-gated. During the DARK reference step every light is
  // OFF so the camera captures a true black level. During the WHITE reference
  // step (and while waiting to save) the external white LEDs are ON — the SAME
  // light the camera sees in a real test (see the flash timing in startTest) —
  // while the AS7341's on-board LED is kept OFF so it can't add a hot-spot the
  // camera would bake into its white reference. Lights restored to idle on exit.

  while (true) {
    // Drive illumination from the current calibration step:
    //   CAM_CAL_DARK  → all off          (true dark reference)
    //   CAM_CAL_WHITE → external LEDs on (camera's working light)
    //   CAM_CAL_DONE  → external LEDs on (keep lit while user confirms save)
    if (camCalStep == CAM_CAL_DARK) {
      illuminatorOff();
      colorOnboardLedOff();
    } else {
      colorOnboardLedOff();
      illuminatorOn();
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- Cam Calibration -");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 22, camCalStepLabel());

    char rgbBuf[24];
    if (camLastCalR == 0 && camLastCalG == 0 && camLastCalB == 0) {
      snprintf(rgbBuf, sizeof(rgbBuf), "Last: (none)");
    } else {
      snprintf(rgbBuf, sizeof(rgbBuf), "Last R:%u G:%u B:%u",
               camLastCalR, camLastCalG, camLastCalB);
    }
    u8g2.drawStr(0, 34, rgbBuf);

    u8g2.drawStr(0, 46, camOnline ? "ESP32-CAM: online" : "ESP32-CAM: OFFLINE");

    if (camCalStep == CAM_CAL_DONE) {
      u8g2.drawStr(0, 58, "SEL=Save  UP/DN=Cancel");
    } else {
      u8g2.drawStr(0, 58, "SEL=Capture UP/DN=Cncl");
    }

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    if (key == 15) {
      if (camCalStep == CAM_CAL_DONE) {
        camCalSave();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(20, 32, "Cam Cal Saved!");
        u8g2.sendBuffer();
        delay(1500);
        break;
      } else {
        // CAL_DARK / CAL_WHITE — this can take ~1 s on the ESP32 side
        // (warmup frames + capture). Show a brief progress message.
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(16, 32, "Capturing...");
        u8g2.sendBuffer();

        // For the WHITE reference the external LEDs were only switched on at
        // the top of this loop iteration, so give them + the camera AEC/AWB a
        // moment to settle before the capture. (DARK is shot in the dark, so
        // no settle is needed there.)
        if (camCalStep == CAM_CAL_WHITE) {
          delay(CAM_LIGHT_SETTLE_MS);
        }
        camCalCapture();

        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        if (camCalStep == CAM_CAL_WHITE) {
          u8g2.drawStr(10, 32, "Dark captured!");
        } else if (camCalStep == CAM_CAL_DONE) {
          u8g2.drawStr(10, 32, "White captured!");
        } else {
          u8g2.drawStr(10, 32, "Capture failed.");
        }
        u8g2.sendBuffer();
        delay(900);
      }
    } else if (key == 2 || key == 10 || key == 8) {
      camCalCancel();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(20, 32, "Cam Cal Cancelled");
      u8g2.sendBuffer();
      delay(1200);
      break;
    }

    delay(80);
  }

  colorOnboardLedOff();   // ensure AS7341 on-board LED off
  illuminatorOn();        // external LEDs on (idle light) on exit
  inCameraCal = false;
  setMenu(&cameraMenu);
}

// ============================================
// COMBINED RGB + CAMERA CALIBRATION SCREEN
// ============================================
//
// Walks the user through DARK → WHITE → DONE for both colour sensors
// in lockstep. One SELECT press captures the same reference point on
// both sensors back-to-back, which guarantees the two sensors are
// calibrated against the *same physical reference surface* — useful
// because the RGB sensor and ESP32-CAM look at the same scene during
// a real test.
//
// Graceful handling rules:
//
//   1. If the camera is OFFLINE at entry, the user is given a choice:
//      proceed with RGB-only, or cancel. We never silently calibrate
//      only one sensor when the user explicitly asked for both.
//
//   2. If a camera capture fails *during* the sequence (UART error,
//      ESP32 reset, etc.), the RGB side of that step is preserved and
//      the user is offered the choice to retry just the camera step
//      OR drop the camera and continue RGB-only OR cancel everything.
//
//   3. Save is "best effort, both": colorCalSave() runs first (it's
//      synchronous and local). If that succeeds and the camera was in
//      the flow, camCalSave() runs next. A camera-save failure does
//      NOT roll back the RGB save — the RGB cal is already on EEPROM
//      and useful on its own. The user is informed clearly.
//
//   4. Cancel at any point calls both colorCalCancel() and
//      camCalCancel(), which each restore from their own persistent
//      storage — there is no partial-write state to worry about.
//
// Layout (128x64):
//   y=10  Title:   "-- RGB+Cam Cal -----"
//   y=12  divider
//   y=22  Step instruction (DARK / WHITE / SAVE)
//   y=32  RGB live R G B
//   y=42  RGB live C  +  Cam status (online/offline/skipped)
//   y=52  Last cam capture (or "-")
//   y=62  Action hint
//
// Controls:
//   SELECT (15)       — capture / confirm save
//   UP/DN (2/10) / 8  — cancel at any point
//
// ---- helper: ask user how to handle a camera fault mid-sequence ----
// Returns:
//   0 = retry the same camera step
//   1 = drop the camera, finish RGB-only
//   2 = cancel everything
static int promptCamFaultChoice(const char* faultMsg) {
  int cursor = 0;
  static const int N = 3;
  static const char* labels[N] = {
    "Retry camera",
    "Skip cam (RGB only)",
    "Cancel everything"
  };

  while (true) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- Camera Fault ----");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 22, faultMsg);

    for (int i = 0; i < N; i++) {
      int y = 34 + i * 10;
      if (i == cursor) {
        u8g2.drawBox(0, y - 7, 128, 9);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, labels[i]);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(2, y, labels[i]);
      }
    }
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();
    if (key == 2)  { cursor = (cursor - 1 + N) % N; delay(120); continue; }
    if (key == 10) { cursor = (cursor + 1) % N;      delay(120); continue; }
    if (key == 15) return cursor;
    if (key == 8)  return 2;   // physical-cancel == "Cancel everything"

    delay(80);
  }
}

// ---- helper: ask user how to handle camera being offline at entry ----
// Returns:
//   true  = proceed RGB-only
//   false = cancel
static bool promptCamOfflineChoice() {
  int cursor = 0;
  static const int N = 2;
  static const char* labels[N] = {
    "Proceed RGB only",
    "Cancel"
  };

  while (true) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- Camera Offline --");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 24, "ESP32-CAM did not");
    u8g2.drawStr(0, 33, "respond. Calibrate");
    u8g2.drawStr(0, 42, "RGB sensor only?");

    for (int i = 0; i < N; i++) {
      int y = 54 + i * 9;
      if (i == cursor) {
        u8g2.drawBox(0, y - 7, 128, 9);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, labels[i]);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(2, y, labels[i]);
      }
    }
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();
    if (key == 2)  { cursor = (cursor - 1 + N) % N; delay(120); continue; }
    if (key == 10) { cursor = (cursor + 1) % N;      delay(120); continue; }
    if (key == 15) return (cursor == 0);
    if (key == 8)  return false;

    delay(80);
  }
}

void runBothColorCalibrationScreen() {
  // ---- Pre-flight: handle camera-offline at entry ----
  // We sample camOnline once at entry; if the camera comes back later
  // we don't re-include it (would require re-syncing state machines).
  bool useCam = camOnline;
  if (!useCam) {
    bool proceed = promptCamOfflineChoice();
    if (!proceed) {
      // User cancelled before starting — make sure both state machines
      // are idle and bail back to the menu.
      colorCalCancel();
      camCalCancel();
      inBothColorCal = false;
      setMenu(&colorSensorsMenu);
      return;
    }
  }

  // ---- Start the state machines ----
  // Both are in COLOR_CAL_DARK / CAM_CAL_DARK after their *Begin() calls.
  colorCalBegin();
  if (useCam) camCalBegin();
  // Illumination is step-gated (see the live-preview and capture blocks below):
  // the DARK reference is captured with EVERY light off so both sensors see a
  // true black level; the WHITE reference lights each sensor with its OWN light
  // (AS7341 under its on-board LED with the external LEDs off; the camera under
  // the external white LEDs). Restored to idle on exit.

  // ---- Main loop ----
  while (true) {
    // Live preview reads the AS7341. Light it from the current step:
    //   COLOR_CAL_DARK → all off (true dark; nothing should be lit)
    //   else           → on-board LED on, external off (AS7341's own light)
    // This also restores the AS7341 light state after a camera capture has
    // switched to the external LEDs.
    if (colorCalStep == COLOR_CAL_DARK) {
      illuminatorOn();
      colorOnboardLedOff();
    } else {
      illuminatorOn();
      colorOnboardLedOff();
    }
    RawRGBC live = colorReadRaw();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- RGB+Cam Cal -----");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);

    // Step label: drive off the RGB state machine; the camera one
    // mirrors it in lockstep so they're always at the same stage
    // (unless the camera was skipped — handled by useCam).
    u8g2.drawStr(0, 22, colorCalStepLabel());

    // Live RGB readout (raw counts)
    char rBuf[24], cBuf[24];
    snprintf(rBuf, sizeof(rBuf), "R:%-5u G:%-5u B:%-5u", live.r, live.g, live.b);
    u8g2.drawStr(0, 32, rBuf);

    // Clear channel + camera state on the same line to save space
    if (useCam) {
      snprintf(cBuf, sizeof(cBuf), "C:%-5u  cam:on", live.c);
    } else {
      snprintf(cBuf, sizeof(cBuf), "C:%-5u  cam:skip", live.c);
    }
    u8g2.drawStr(0, 42, cBuf);

    // Last camera capture echo (only meaningful when useCam)
    char camLine[28];
    if (useCam) {
      if (camLastCalR == 0 && camLastCalG == 0 && camLastCalB == 0) {
        snprintf(camLine, sizeof(camLine), "cam: (none yet)");
      } else {
        snprintf(camLine, sizeof(camLine), "cam R:%u G:%u B:%u",
                 camLastCalR, camLastCalG, camLastCalB);
      }
    } else {
      snprintf(camLine, sizeof(camLine), "cam: skipped");
    }
    u8g2.drawStr(0, 52, camLine);

    // Action hint
    if (colorCalStep == COLOR_CAL_DONE) {
      u8g2.drawStr(0, 62, "SEL=Save  UP/DN=Cancel");
    } else {
      u8g2.drawStr(0, 62, "SEL=Capture UP/DN=Cncl");
    }

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    int key = scanKey();

    // ---- Cancel ----
    if (key == 2 || key == 10 || key == 8) {
      colorCalCancel();
      if (useCam) camCalCancel();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 28, "Both calibrations");
      u8g2.drawStr(20, 42, "cancelled.");
      u8g2.sendBuffer();
      delay(1300);
      break;
    }

    // ---- SELECT ----
    if (key == 15) {
      // ---- Final-stage save path ----
      if (colorCalStep == COLOR_CAL_DONE) {
        // Save RGB first (fast, local I2C/EEPROM).
        colorCalSave();
        bool rgbSaved = (colorCalStep == COLOR_CAL_IDLE);

        bool camSaved = true;
        if (useCam) {
          camCalSave();
          camSaved = (camCalStep == CAM_CAL_IDLE);
        }

        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        if (rgbSaved && camSaved) {
          u8g2.drawStr(0, 22, "Both saved!");
          u8g2.setFont(u8g2_font_5x7_tf);
          if (useCam) {
            char wBuf[24];
            snprintf(wBuf, sizeof(wBuf), "RGB W R:%u G:%u",
                     colorCalData.white.r, colorCalData.white.g);
            u8g2.drawStr(0, 36, wBuf);
            snprintf(wBuf, sizeof(wBuf), "Cam W R:%u G:%u",
                     camLastCalR, camLastCalG);
            u8g2.drawStr(0, 48, wBuf);
          } else {
            char wBuf[24];
            snprintf(wBuf, sizeof(wBuf), "RGB W R:%u G:%u",
                     colorCalData.white.r, colorCalData.white.g);
            u8g2.drawStr(0, 36, wBuf);
            u8g2.drawStr(0, 48, "Cam: skipped");
          }
        } else if (rgbSaved && !camSaved) {
          // RGB on disk, camera save failed — be honest about it.
          u8g2.drawStr(0, 22, "RGB saved.");
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(0, 36, "Cam save FAILED.");
          u8g2.drawStr(0, 48, "RGB cal is in effect.");
        } else {
          // RGB itself didn't save — extremely rare; surface it.
          u8g2.drawStr(0, 22, "Save failed.");
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(0, 36, "Try again, or reset");
          u8g2.drawStr(0, 48, "to defaults.");
        }
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.sendBuffer();
        delay(2200);
        break;
      }

      // ---- Mid-sequence capture path (DARK or WHITE) ----
      // Show a brief progress indicator because the camera capture
      // takes ~1 s on the ESP32 side.
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(16, 32, "Capturing...");
      u8g2.sendBuffer();

      // Snapshot pre-capture state so we can detect partial failure.
      ColorCalStep rgbBefore = colorCalStep;
      CamCalStep   camBefore = useCam ? camCalStep : CAM_CAL_IDLE;

      // Are we capturing the DARK reference right now? If so EVERY light stays
      // off for both sensors so each captures a true black level.
      bool capturingDark = (rgbBefore == COLOR_CAL_DARK);

      // AS7341 first. DARK → all off; WHITE → external off, on-board LED on
      // (the AS7341's own illuminant). Assert the state here, then let it settle
      // before the capture integrates.
      if (capturingDark) {
        illuminatorOff();
        colorOnboardLedOff();
      } else {
        illuminatorOff();
        colorOnboardLedOn();
      }
      delay(COLOR_FLASH_SETTLE_MS);
      colorCalCapture();
      bool rgbAdvanced = (colorCalStep != rgbBefore);

      // Camera second (slow, can fail). DARK → all off (true black level);
      // WHITE → external white LEDs on with the AS7341 on-board LED OFF, the
      // SAME light the camera sees in a real test (startTest). Capturing the
      // camera's white reference under any other light would bake that light
      // into every measured frame.
      bool camAdvanced = true;
      if (useCam) {
        if (capturingDark) {
          illuminatorOff();
          colorOnboardLedOff();
        } else {
          colorOnboardLedOff();
          illuminatorOn();
        }
        delay(CAM_LIGHT_SETTLE_MS);   // let the LEDs + camera AEC/AWB settle
        camCalCapture();
        camAdvanced = (camCalStep != camBefore);
      }

      // ---- Result handling ----
      if (rgbAdvanced && camAdvanced) {
        // Happy path: both moved forward.
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        if (colorCalStep == COLOR_CAL_WHITE) {
          // Dark reference captured on both sensors. No illumination change
          // needed here — the next loop iteration's top restores the AS7341
          // preview light (external off, on-board on) while the user swaps in
          // the white reference.
          u8g2.drawStr(0, 22, "Dark captured");
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(0, 36, "on both sensors.");
          u8g2.drawStr(0, 48, "Show WHITE next.");
        } else if (colorCalStep == COLOR_CAL_DONE) {
          u8g2.drawStr(0, 22, "White captured");
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(0, 36, "on both sensors.");
          u8g2.drawStr(0, 48, "SEL to save.");
        }
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.sendBuffer();
        delay(1100);
      } else if (rgbAdvanced && !camAdvanced) {
        // Camera fault mid-flow — RGB already captured this point.
        // Ask the user how to proceed. We can't "undo" the RGB capture
        // cleanly (would require restarting the whole sequence), so the
        // choices are: retry just the camera, skip the camera entirely,
        // or cancel the whole thing.
        int choice = promptCamFaultChoice("Cam capture failed.");

        if (choice == 0) {
          // Retry camera. Loop back and try the same camCalCapture()
          // again next SELECT — but the state machine has already been
          // partially walked. The cleanest recovery is to *re-run*
          // camCalCapture() now, since on the ESP32 side it's just
          // another CAL_DARK/CAL_WHITE command with no harmful side
          // effects beyond re-overwriting the same slot.
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(16, 32, "Retrying cam...");
          u8g2.sendBuffer();

          // Roll the cam state back one notch so this capture targets
          // the same step RGB just advanced past.
          if (colorCalStep == COLOR_CAL_WHITE) {
            camCalStep = CAM_CAL_DARK;
          } else if (colorCalStep == COLOR_CAL_DONE) {
            camCalStep = CAM_CAL_WHITE;
          }
          // Camera light depends on which reference we're retrying:
          //   colorCalStep == COLOR_CAL_WHITE → retrying the DARK ref → all off
          //   colorCalStep == COLOR_CAL_DONE  → retrying the WHITE ref → ext on
          if (colorCalStep == COLOR_CAL_WHITE) {
            illuminatorOff();
            colorOnboardLedOff();
          } else {
            colorOnboardLedOff();
            illuminatorOn();
          }
          delay(CAM_LIGHT_SETTLE_MS);   // let the LEDs + camera AEC/AWB settle
          camCalCapture();

          if ((colorCalStep == COLOR_CAL_WHITE  && camCalStep == CAM_CAL_WHITE) ||
              (colorCalStep == COLOR_CAL_DONE   && camCalStep == CAM_CAL_DONE)) {
            // Recovered — both back in sync.
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_6x10_tf);
            u8g2.drawStr(8, 32, "Cam recovered!");
            u8g2.sendBuffer();
            delay(1100);
          } else {
            // Still broken. Treat camera as offline for the rest of the
            // sequence so the user isn't trapped.
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_6x10_tf);
            u8g2.drawStr(0, 22, "Cam still failing.");
            u8g2.setFont(u8g2_font_5x7_tf);
            u8g2.drawStr(0, 36, "Continuing RGB only.");
            u8g2.drawStr(0, 48, "Old cam cal kept.");
            u8g2.setFont(u8g2_font_6x10_tf);
            u8g2.sendBuffer();
            delay(1600);
            camCalCancel();
            useCam = false;
          }
        } else if (choice == 1) {
          // Drop the camera. Restore old cam cal from NVS and continue
          // RGB-only for the rest of this run.
          camCalCancel();
          useCam = false;
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(0, 28, "Skipping camera.");
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(0, 42, "Continuing RGB only.");
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.sendBuffer();
          delay(1300);
        } else {
          // Cancel everything.
          colorCalCancel();
          camCalCancel();
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(0, 28, "Both calibrations");
          u8g2.drawStr(20, 42, "cancelled.");
          u8g2.sendBuffer();
          delay(1300);
          break;
        }
      } else if (!rgbAdvanced) {
        // RGB itself failed to advance (e.g. sensor disconnected mid-cal).
        // Very rare; report and let user decide.
        if (useCam && !camAdvanced) {
          // Both failed — likely a wider hardware issue.
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(0, 22, "Both captures");
          u8g2.drawStr(28, 36, "failed.");
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(0, 50, "Cancelling...");
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.sendBuffer();
          delay(1500);
          colorCalCancel();
          camCalCancel();
          break;
        } else {
          // RGB failed but camera (if used) advanced — extremely unusual.
          // Roll back the camera so the two stay in sync and let the
          // user retry.
          if (useCam) {
            if (camCalStep == CAM_CAL_WHITE)      camCalStep = CAM_CAL_DARK;
            else if (camCalStep == CAM_CAL_DONE)  camCalStep = CAM_CAL_WHITE;
          }
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(0, 22, "RGB capture");
          u8g2.drawStr(28, 36, "failed.");
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(0, 50, "Try SELECT again.");
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.sendBuffer();
          delay(1500);
        }
      }
    }

    delay(80);
  }

  // ---- Restore idle illumination ----
  // All exit paths (save, cancel, fault) reach here. Hand illumination back to
  // idle: AS7341 on-board LED off, external white LEDs on, so the device isn't
  // left dark.
  colorOnboardLedOff();
  illuminatorOn();

  inBothColorCal = false;
  setMenu(&colorSensorsMenu);
}

// ============================================
// UNIVERSAL BACK NAVIGATION
// ============================================
//
// Maps key 8 (treated as the "Back" / left button across the firmware)
// to a one-level-up navigation from any menu screen. Full-screen
// runners (calibration screens, BT settings, dev diagnostics, etc.)
// still handle their own key 8 internally — this function is only
// called from the main menu loop when no full-screen takeover is
// active.
//
// Hierarchy:
//   mainMenu       (root — key 8 does nothing)
//   ├── settingsMenu       → mainMenu
//   │   └── sensorsMenu    → settingsMenu
//   │       ├── pHMenu             → sensorsMenu
//   │       ├── colorSensorsMenu   → sensorsMenu
//   │       │   ├── RGBMenu        → colorSensorsMenu
//   │       │   └── cameraMenu     → colorSensorsMenu
//   │       └── TDSMenu            → sensorsMenu
//   └── (BT settings handled as full-screen takeover)
//
// Note: this is parallel to the existing in-menu "Back" items — those
// stay in place because they're discoverable on the screen, while
// this is the physical-button shortcut.
void goBackUniversal() {
  Menu* m = getMenu();
  if (m == &settingsMenu)      { setMenu(&mainMenu);         return; }
  if (m == &sensorsMenu)       { setMenu(&settingsMenu);     return; }
  if (m == &pHMenu)            { setMenu(&sensorsMenu);      return; }
  if (m == &colorSensorsMenu)  { setMenu(&sensorsMenu);      return; }
  if (m == &RGBMenu)           { setMenu(&colorSensorsMenu); return; }
  if (m == &cameraMenu)        { setMenu(&colorSensorsMenu); return; }
  if (m == &TDSMenu)           { setMenu(&sensorsMenu);      return; }
  // mainMenu or unknown: do nothing.
}

// ============================================
// DEVELOPER DIAGNOSTICS SCREEN  (hidden)
// ============================================
//
// Live, free-running readout of every sensor on the board. Designed
// for bench debugging — no calibration, no state machines, no saving;
// just raw and computed values updated as fast as the sensors will
// respond.
//
// How to reach this screen:
//   On the Main Menu, press key 16 (bottom-right corner) five times
//   in a row. Any other keypress resets the counter. This is a hidden
//   developer unlock — it appears nowhere in the UI.
//
// Pages (UP / DOWN to switch):
//   1. pH + temp + TDS/EC
//   2. RGB raw R/G/B/C
//   3. RGB normalised + lux + CCT + illuminator
//   4. Camera RGB + BLE status
//
// Controls:
//   UP   (2)  — previous page
//   DOWN (10) — next page
//   SEL  (15) — force immediate refresh
//   key 8     — exit back to main menu (universal back button)
//
// Performance note: full sensor reads (especially colour averaging
// and camera UART) are slow. To keep the UI responsive we refresh
// the screen every ~250 ms and only sample the page-relevant sensors
// each tick. The camera is only polled when page 4 is active.
void runDevDiagnosticsScreen() {
  const uint8_t PAGE_COUNT = 4;
  uint8_t page = 0;

  // Cached last camera reading — camera reads are slow (~hundreds of
  // ms over UART) so we only update when on page 4, and reuse the
  // last value otherwise.
  CameraRGB lastCam = {0, 0, 0, false};
  bool      camEverRead = false;

  while (true) {
    // Light the sensor this page reads from: the AS7341 pages (2 & 3, i.e.
    // page index 1 & 2) use the AS7341's own on-board LED with the external
    // LEDs off; every other page uses the idle/camera light (external LEDs on,
    // on-board off). The on/off helpers are state-tracked, so re-asserting the
    // same state each tick is a cheap no-op that never blinks the LED.
    if (page == 1 || page == 2) {
      illuminatorOff();
      colorOnboardLedOn();
    } else {
      colorOnboardLedOff();
      illuminatorOn();
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    // Header: title + page indicator, always shown
    char header[24];
    snprintf(header, sizeof(header), "DEV %u/%u", (unsigned)(page + 1),
             (unsigned)PAGE_COUNT);
    u8g2.drawStr(0, 10, header);
    u8g2.drawStr(60, 10, "(hidden)");
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_5x7_tf);

    if (page == 0) {
      // ---- Page 1: pH + temp + TDS ----
      float temp  = pHReadTemperature();
      float phV   = pHReadVoltage();
      float ph    = voltageToPH(phV, temp);
      float tdsV  = tdsReadVoltage();
      float ec    = voltageToEC(tdsV, temp);
      float ppm   = ec * 0.5f;

      char buf[28];
      snprintf(buf, sizeof(buf), "Temp:   %.1f C", temp);
      u8g2.drawStr(0, 22, buf);
      snprintf(buf, sizeof(buf), "pH V:   %.4f V", phV);
      u8g2.drawStr(0, 31, buf);
      snprintf(buf, sizeof(buf), "pH:     %.2f", ph);
      u8g2.drawStr(0, 40, buf);
      snprintf(buf, sizeof(buf), "TDS V:  %.4f V", tdsV);
      u8g2.drawStr(0, 49, buf);
      snprintf(buf, sizeof(buf), "EC: %.0f  ppm:%.0f", ec, ppm);
      u8g2.drawStr(0, 58, buf);
    }
    else if (page == 1) {
      // ---- Page 2: RGB raw ----
      RawRGBC raw = colorReadRaw();

      char buf[28];
      u8g2.drawStr(0, 22, "TCS34725 raw");
      snprintf(buf, sizeof(buf), "R: %u", raw.r);
      u8g2.drawStr(0, 32, buf);
      snprintf(buf, sizeof(buf), "G: %u", raw.g);
      u8g2.drawStr(64, 32, buf);
      snprintf(buf, sizeof(buf), "B: %u", raw.b);
      u8g2.drawStr(0, 42, buf);
      snprintf(buf, sizeof(buf), "C: %u", raw.c);
      u8g2.drawStr(64, 42, buf);
      snprintf(buf, sizeof(buf), "ATIME:0x%02X GAIN:%u",
               (unsigned)colorGetIntegrationTime(),
               (unsigned)colorGetGain());
      u8g2.drawStr(0, 53, buf);
    }
    else if (page == 2) {
      // ---- Page 3: RGB normalised + lux + CCT + illuminator ----
      RawRGBC       raw  = colorReadRaw();
      NormalisedRGB norm = colorNormalise(raw);
      float         lux  = colorCalcLux(raw);
      uint16_t      cct  = colorCalcCCT(raw);

      char buf[28];
      u8g2.drawStr(0, 22, "TCS34725 derived");
      snprintf(buf, sizeof(buf), "Norm R:%u G:%u B:%u",
               norm.r, norm.g, norm.b);
      u8g2.drawStr(0, 32, buf);
      snprintf(buf, sizeof(buf), "#%02X%02X%02X", norm.r, norm.g, norm.b);
      u8g2.drawStr(0, 41, buf);
      snprintf(buf, sizeof(buf), "Lux:%.0f  CCT:%uK", lux, cct);
      u8g2.drawStr(0, 50, buf);
      snprintf(buf, sizeof(buf), "L1:%u/255 L2:%u/255",
               (unsigned)illuminatorGetBrightness(),
               (unsigned)illuminator2GetBrightness());
      u8g2.drawStr(0, 59, buf);
    }
    else {
      // ---- Page 4: Camera + BLE ----
      // Refresh camera on every tick of this page so devs see live
      // changes. Cached value persists for label correctness if a
      // read fails mid-session.
      CameraRGB cam = cameraRead();
      if (cam.valid) {
        lastCam = cam;
        camEverRead = true;
      }

      u8g2.drawStr(0, 22, "ESP32-CAM");
      char buf[28];
      if (!camOnline) {
        u8g2.drawStr(0, 32, "Status: OFFLINE");
      } else if (camEverRead && lastCam.valid) {
        snprintf(buf, sizeof(buf), "R:%u G:%u B:%u",
                 lastCam.r, lastCam.g, lastCam.b);
        u8g2.drawStr(0, 32, buf);
        snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                 lastCam.r, lastCam.g, lastCam.b);
        u8g2.drawStr(0, 41, buf);
      } else {
        u8g2.drawStr(0, 32, "Status: online");
        u8g2.drawStr(0, 41, "(read failed)");
      }

      // BLE state
      bool conn = isBluetoothConnected();
      const char* bleStr;
      if (!bleSettings.advertisingEnabled)      bleStr = "BLE: OFF";
      else if (conn)                            bleStr = "BLE: Connected";
      else                                      bleStr = "BLE: Adv";
      u8g2.drawStr(0, 53, bleStr);
      snprintf(buf, sizeof(buf), "TX:%ddBm", (int)bleSettings.txPower);
      u8g2.drawStr(72, 53, buf);
    }

    // Footer hint
    u8g2.drawStr(0, 63, "UP/DN:page k8:exit");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();

    // ---- Input ----
    // Shorter delay than menus so live values update visibly.
    int key = scanKey();

    if (key == 8) {
      break;   // exit to main menu
    } else if (key == 2) {
      page = (page == 0) ? (PAGE_COUNT - 1) : (page - 1);
      delay(150);   // debounce — UP/DOWN are easy to over-press
    } else if (key == 10) {
      page = (page + 1) % PAGE_COUNT;
      delay(150);
    } else if (key == 15) {
      // SELECT — used as a manual refresh trigger. We already redraw
      // every loop tick so this is mostly a "I want it right now"
      // affordance; still useful to flush any pending BLE work.
      BLE.poll();
      delay(100);
    } else {
      // No key: short delay before next refresh.
      delay(250);
    }

    // Keep BLE alive so a connected central doesn't time out while
    // a developer is parked on this screen.
    BLE.poll();
  }

  // Restore idle illumination on exit (external LEDs on, on-board LED off).
  colorOnboardLedOff();
  illuminatorOn();

  inDevDiagnostics = false;
  setMenu(&mainMenu);
}

// ============================================
// MENU ACTION CALLBACKS
// ============================================

// ---- Loading screen shared constants ----
// Used by BOTH the test loading screen (startTest, below) and the boot loading
// screen (drawBootFrame, further down). They must be declared before the first
// use, which is startTest(), so they live here rather than in the boot section.
static const uint8_t BOOT_STEPS_TOTAL = 6;   // boot init steps
static const uint8_t TEST_STEPS_TOTAL = 5;   // test acquisition steps

// Status tags (kept short for the small font)
#define BOOT_OK     "OK"
#define BOOT_WARN   "WARN"
#define BOOT_FAIL   "FAILED"

// Title bar shown on every test-loading frame.
#define TEST_TITLE "-- Testing ---------"

// Forward declaration: drawProgressFrame() is defined further down with the
// boot loading screen, but startTest() (below) uses it for the test loading
// screen. Arduino's auto-prototype generation is unreliable for static
// functions, so declare it explicitly here.
static void drawProgressFrame(const char* title, const char* stepLabel,
                              const char* statusLabel, uint8_t stepsCompleted,
                              uint8_t stepsTotal);

void startTest() {
  // ---- Step 1/5: pH & Temp ----
  // Mirror the boot loading screen: each frame announces the step about to
  // run and shows the PREVIOUS step's result badge. prevStatus carries that
  // result forward. The pH/temp path has no error sentinel (it always returns
  // a plausible value), so step 1 is treated as OK.
  drawProgressFrame(TEST_TITLE, "pH & Temp...", "", 0, TEST_STEPS_TOTAL);
  BLE.poll();
  const char* prevStatus = BOOT_OK;

  // ---- Notify connected central that a test has begun ----
  if (isBluetoothConnected()) {
    StaticJsonDocument<64> startDoc;
    startDoc["device"] = DEVICE_NAME;
    startDoc["type"]   = "test_started";
    sendJsonData(startDoc);
    BLE.poll();
  }

  // ---- Collect all sensor data ----
  // Power the TDS probe NOW so the DFR0504 isolator's input stage starts
  // settling immediately. The pH/temp reads below cover part of that
  // settle window "for free"; we then top up to a full TDS_POWER_SETTLE_MS
  // before sampling. Because the probe is already on, tdsRead() detects
  // "already powered" and samples without re-running its own power cycle.
  // This mirrors why the live calibration screen reads correctly: the
  // isolator gets enough continuous powered time to lock before sampling.
  tdsPowerOn();

  float temp = pHReadTemperature();
  float pH   = pHRead(temp);

  // Top up the isolator settle. pH/temp reads already consumed part of it,
  // but delay a full window here to stay robust if those reads are fast.
  delay(TDS_POWER_SETTLE_MS);

  // ---- Step 2/5: TDS & EC ---- (pH/Temp done → OK)
  drawProgressFrame(TEST_TITLE, "TDS & EC...", prevStatus, 1, TEST_STEPS_TOTAL);
  BLE.poll();

  Serial.print("[Test-TDS] powered="); Serial.println(tdsIsPowered() ? "YES" : "NO");
  Serial.print("[Test-TDS] vLow=");   Serial.print(tdsCalData.low.voltage, 4);
  Serial.print("  vHigh=");           Serial.println(tdsCalData.high.voltage, 4);
  Serial.print("[Test-TDS] magic=0x"); Serial.println(tdsCalData.magic, HEX);
  float ecVoltage = tdsReadVoltage();
  Serial.print("[Test-TDS] ecVoltage="); Serial.println(ecVoltage, 4);

  //float ecVoltage = tdsReadVoltage();
  Serial.print("EC Voltage: ");
  Serial.print(ecVoltage);
  float ec = voltageToEC(ecVoltage, temp);

  float tds  = ecToTDS(ec);   // probe already on → no internal re-cycle
  //float ec   = tdsToEC(tds);    // CELL conductivity (what is in the cup)

  // Neat-urine conductivity and specific gravity, derived from the SAME reading
  // (no extra probe cycle). ecSample scales the cell EC up by the fixed dilution
  // factor; sg maps that to specific gravity via the fitted model. ec == 0 is the
  // calibration-fault sentinel, so SG is forced to 0.0 there too (not a bogus 1.000).
  float ecSample = ec * TDS_DILUTION_FACTOR;
  float sg       = (ec > 0.0f) ? ecToSG(ecSample) : 0.0f;

  tdsPowerOff();                // back to idle (probe dark between tests)

  // tdsRead() returns exactly 0.0 only when the calibration is degenerate
  // (both cal points share a voltage) — flag that as a WARN.
  prevStatus = (tds == 0.0f) ? BOOT_WARN : BOOT_OK;

  // ---- Step 3/5: Colour (RGB) ----
  // IMPORTANT: draw this frame NOW, before illuminatorOff()/colorOnboardLedOn().
  // The OLED (0x3C) and AS7341 (0x39) share one I2C bus, so no OLED write may
  // happen between colorOnboardLedOn() and colorReadRawAveraged().
  drawProgressFrame(TEST_TITLE, "Colour (RGB)...", prevStatus, 2, TEST_STEPS_TOTAL);
  BLE.poll();

  // ---- Ambient-light-leak guard (every test) ----
  // Before lighting the AS7341, verify the box is dark with EVERY light off. A
  // significant lights-off reading means room light is leaking in past the lid
  // and will bias both the AS7341 and the camera. colorCheckAmbientLeak() draws
  // no OLED and leaves all lights off — the exact state the AS7341 read below
  // starts from — so this is safe to run on the shared I2C bus here. A leak is
  // non-fatal: we warn (serial + a brief OLED notice) and downgrade the colour
  // step to WARN, but still take the reading.
  AmbientLeak amb = colorCheckAmbientLeak();
  if (amb.leak) {
    Serial.print("[Test] WARNING: ambient light leak (peak=");
    Serial.print(amb.peak);
    Serial.println(") — result may be unreliable; seal the box.");
    drawProgressFrame(TEST_TITLE, "Light leak!", BOOT_WARN, 2, TEST_STEPS_TOTAL);
    BLE.poll();
    delay(1200);
  }

  // ---- Colour sensor (AS7341) — flash to its OWN on-board LED ----
  // The AS7341 reads under its on-board LED, with the external D9/D10 white
  // LEDs switched OFF — their broad white light would otherwise swamp the
  // spectral sensor. Pulse the on-board LED on just for this integration, then
  // hand illumination back to the external LEDs for the camera below.
  illuminatorOn();                      // external LEDs off for the AS7341
  //colorOnboardLedOn();                   // AS7341's own illuminant on
  delay(COLOR_FLASH_SETTLE_MS);          // let the on-board LED + sensor settle
  RawRGBC       raw = colorReadRawAveraged();
  colorOnboardLedOff();                  // AS7341 done — on-board LED off

  // colorReadRawAveraged() returns an all-zero struct when the AS7341 doesn't
  // respond (it self-recovers the I2C bus first, so the OLED draw below is
  // safe). All channels zero → FAIL; saturation on any band → WARN.
  if (raw.r == 0 && raw.g == 0 && raw.b == 0 && raw.c == 0) {
    prevStatus = BOOT_FAIL;
  } else if (raw.satAnalog || raw.satDigital || amb.leak) {
    prevStatus = BOOT_WARN;
  } else {
    prevStatus = BOOT_OK;
  }

  NormalisedRGB rgb = colorNormalise(raw);
  float         lux = colorCalcLux(raw);
  uint16_t      cct = colorCalcCCT(raw);

  char hexColor[8];
  snprintf(hexColor, sizeof(hexColor), "#%02X%02X%02X", rgb.r, rgb.g, rgb.b);

  // ---- Step 4/5: Camera ----
  // Draw BEFORE the blocking read so the user sees feedback during the
  // up-to-~4s capture/timeout. (Camera is UART, not I2C — safe to draw here.)
  drawProgressFrame(TEST_TITLE, "Camera...", prevStatus, 3, TEST_STEPS_TOTAL);
  BLE.poll();

  // Camera (ESP32-CAM) — independent colour reading via image processing.
  // Lit by the external white LEDs, which we switch ON now and leave on for
  // the whole capture (the AS7341's on-board LED is already off, so it can't
  // appear as a hot-spot in the frame). Returns valid=false silently if the
  // ESP32 isn't connected/fails.
  illuminatorOn();                       // external LEDs on for the camera
  delay(CAM_LIGHT_SETTLE_MS);            // let the LEDs + camera AEC/AWB settle
  CameraRGB cam = cameraRead();
  char hexCam[8] = "";
  if (cam.valid) {
    snprintf(hexCam, sizeof(hexCam), "#%02X%02X%02X", cam.r, cam.g, cam.b);
  }

  // Camera is optional hardware — offline/timeout is a WARN, never a FAIL.
  prevStatus = cam.valid ? BOOT_OK : BOOT_WARN;

  // Camera done — leave the external LEDs on (idle/menu light); the AS7341
  // on-board LED is already off. The live/menu and dev-diagnostics screens
  // expect the sample to stay lit by the external LEDs.
  illuminatorOn();

  // ---- Step 5/5: Finalize & send ----
  drawProgressFrame(TEST_TITLE, "Sending...", prevStatus, 4, TEST_STEPS_TOTAL);
  BLE.poll();

  // ---- Serial log ----
  Serial.println("[Test] ===== New Test Result =====");
  Serial.print("[Test] Device:  "); Serial.println(DEVICE_NAME);
  Serial.print("[Test] Temp:    "); Serial.print(temp, 1); Serial.println(" C");
  Serial.print("[Test] pH:      "); Serial.println(pH, 2);
  Serial.print("[Test] TDS:     "); Serial.print(tds, 0); Serial.println(" ppm");
  Serial.print("[Test] EC:      "); Serial.print(ec, 2);  Serial.println(" uS/cm");
  Serial.print("[Test] EC neat: "); Serial.print(ecSample, 1); Serial.println(" uS/cm");
  Serial.print("[Test] SG:      "); Serial.println(sg, 4);
  Serial.print("[Test] Raw   R="); Serial.print(raw.r);
  Serial.print(" G="); Serial.print(raw.g);
  Serial.print(" B="); Serial.print(raw.b);
  Serial.print(" C="); Serial.println(raw.c);
  Serial.print("[Test] Norm  R="); Serial.print(rgb.r);
  Serial.print(" G="); Serial.print(rgb.g);
  Serial.print(" B="); Serial.println(rgb.b);
  Serial.print("[Test] Color:   "); Serial.println(hexColor);
  Serial.print("[Test] Lux:     "); Serial.println(lux, 1);
  Serial.print("[Test] CCT:     "); Serial.print(cct); Serial.println(" K");
  if (cam.valid) {
    Serial.print("[Test] Cam   R="); Serial.print(cam.r);
    Serial.print(" G=");              Serial.print(cam.g);
    Serial.print(" B=");              Serial.println(cam.b);
    Serial.print("[Test] CamHex: "); Serial.println(hexCam);
  } else {
    Serial.println("[Test] Cam:     (offline)");
  }
  Serial.println("[Test] ==================================");

  // ---- Build JSON payload ----
  StaticJsonDocument<1024> doc;   // bumped from 768 to leave headroom for ec_sample + sg

  doc["device"]  = DEVICE_NAME;
  doc["version"] = DEVICE_VERSION;
  doc["type"]    = "urinalysis";

  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["temp_c"]   = temp;
  sensors["pH"]       = pH;
  sensors["tds_ppm"]  = tds;
  sensors["ec_us_cm"] = ec;
  sensors["ec_sample_us_cm"] = ecSample;   // neat-urine conductivity (dilution-corrected)
  sensors["sg"]              = sg;          // specific gravity (0.0 = calibration fault)

  JsonObject color = sensors.createNestedObject("color");
  color["r"]   = rgb.r;
  color["g"]   = rgb.g;
  color["b"]   = rgb.b;
  color["hex"] = hexColor;
  color["lux"] = lux;
  color["cct"] = cct;

  // Camera (ESP32-CAM via UART) — only included if the read succeeded.
  if (cam.valid) {
    JsonObject camera = sensors.createNestedObject("camera");
    camera["r"]   = cam.r;
    camera["g"]   = cam.g;
    camera["b"]   = cam.b;
    camera["hex"] = hexCam;
  }

  // ---- Auto-send via BLE if connected ----
  bool sent = false;
  if (isBluetoothConnected()) {
    sendJsonData(doc);
    BLE.poll();   // flush the notification immediately
    sent = true;
  }

  // ---- Completion frame ---- (full bar)
  // "Sent" if a central received the payload; "Standalone" (WARN) otherwise —
  // not a failure, the device works without a phone connected.
  const char* sendStatus = sent ? BOOT_OK : BOOT_WARN;
  drawProgressFrame(TEST_TITLE, sent ? "Sent." : "Standalone.",
                    sendStatus, TEST_STEPS_TOTAL, TEST_STEPS_TOTAL);
  delay(400);

  // ---- Page 1: pH, Temp, TDS, EC ----
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Show a small BLE status badge in the title bar
  if (sent) {
    u8g2.drawStr(0, 10, "-- Result 1/2 [BT]-");
  } else {
    u8g2.drawStr(0, 10, "-- Result 1/2 ------");
  }
  u8g2.drawHLine(0, 12, 128);

  char buf[32];
  snprintf(buf, sizeof(buf), "pH:   %.2f", pH);
  u8g2.drawStr(0, 26, buf);
  snprintf(buf, sizeof(buf), "Temp: %.1f C", temp);
  u8g2.drawStr(0, 36, buf);
  snprintf(buf, sizeof(buf), "TDS:  %.0f ppm", tds);
  u8g2.drawStr(0, 46, buf);
  snprintf(buf, sizeof(buf), "EC:   %.2f uS/cm", ec);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 56, buf);
  u8g2.drawStr(0, 63, "SEL=next  UP/DN=exit");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.sendBuffer();

  // Wait: SELECT → page 2, UP/DN → exit early
  while (true) {
    BLE.poll();
    int k = scanKey();
    if (k == 15) break;
    if (k == 2 || k == 10 || k == 8) { setMenu(&mainMenu); return; }
    delay(80);
  }

  // ---- Page 2: RGB, Hex, Lux, CCT ----
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "-- Result 2/2 ------");
  u8g2.drawHLine(0, 12, 128);

  snprintf(buf, sizeof(buf), "R:%-3d G:%-3d B:%-3d", rgb.r, rgb.g, rgb.b);
  u8g2.drawStr(0, 26, buf);
  u8g2.drawStr(0, 38, hexColor);

  char luxBuf[20], cctBuf[20];
  snprintf(luxBuf, sizeof(luxBuf), "Lux: %.1f", lux);
  snprintf(cctBuf, sizeof(cctBuf), "CCT: %u K", cct);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 50, luxBuf);
  u8g2.drawStr(0, 58, cctBuf);
  u8g2.drawStr(70, 63, "SEL=done");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.sendBuffer();

  while (true) {
    BLE.poll();
    int k = scanKey();
    if (k == 15 || k == 8) break;
    delay(80);
  }

  setMenu(&mainMenu);
}

void openSettings()       { setMenu(&settingsMenu); }
void backToMain()         { setMenu(&mainMenu); }
void openSensors()        { setMenu(&sensorsMenu); }

// Color Sensors umbrella navigation
void openColorSensors()      { setMenu(&colorSensorsMenu); }
void backToColorSensors()    { setMenu(&colorSensorsMenu); }
void openBothColorCalibration() { inBothColorCal = true; }

void openPH()             { setMenu(&pHMenu); }
void openPHCalibration()  { calBegin(); inPHCal = true; }

void resetPHCalibration() {
  calResetToDefaults();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(16, 32, "pH Cal reset to");
  u8g2.drawStr(28, 44, "defaults.");
  u8g2.sendBuffer();
  delay(1500);
  setMenu(&pHMenu);
}

void openRGB()             { setMenu(&RGBMenu); }
void openRGBCalibration()  { colorCalBegin(); inRGBCal = true; }
void openIlluminatorAdjust()  { inIlluminatorAdjust  = true; }
void openIlluminator2Adjust() { inIlluminator2Adjust = true; }

void resetRGBCalibration() {
  colorCalResetToDefaults();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(16, 32, "RGB Cal reset to");
  u8g2.drawStr(28, 44, "defaults.");
  u8g2.sendBuffer();
  delay(1500);
  setMenu(&RGBMenu);
}

void openTDS()             { setMenu(&TDSMenu); }
void openTDSCalibration()  { tdsCalBegin(); inTDSCal = true; }

void resetTDSCalibration() {
  tdsCalResetToDefaults();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(16, 32, "TDS Cal reset to");
  u8g2.drawStr(28, 44, "defaults.");
  u8g2.sendBuffer();
  delay(1500);
  setMenu(&TDSMenu);
}

void openCamera()             { setMenu(&cameraMenu); }
void openCameraCalibration()  { camCalBegin(); inCameraCal = true; }

void resetCameraCalibration() {
  camCalResetToDefaults();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(8, 32, "Cam Cal reset to");
  u8g2.drawStr(28, 44, "defaults.");
  u8g2.sendBuffer();
  delay(1500);
  setMenu(&cameraMenu);
}

void openBluetoothSettings() { inBTSettings = true; }

// ============================================
// SETUP
// ============================================

// ============================================
// BOOT LOADING SCREEN
// ============================================
//
// Layout (128x64):
//   y=10  Title: "-- Booting ----------"
//   y=12  Divider
//   y=22  Step label (current sensor being initialised)
//   y=34  Status tag for just-completed step ("OK" / "WARN" / "FAILED")
//   y=44  Progress bar outline + fill  (x=0..127, h=8, y=44..51)
//   y=62  "Step N/N" counter
//
// The bar is drawn as an outline box on first call for each step, then
// filled proportionally from the left as each step completes.
//
// The step-count constants and status-tag macros live above startTest()
// (search "Loading screen shared constants") because both the test loading
// screen and this boot screen reference them, and startTest() comes first.
// ============================================

// Progress bar geometry
static const int BAR_X  = 0;
static const int BAR_Y  = 44;
static const int BAR_W  = 127;
static const int BAR_H  = 8;

/**
 * Draw a single progress frame (shared by the boot and test loading screens).
 *
 * @param title       Title bar text (e.g. "-- Booting ----------").
 * @param stepLabel   Short description of the step currently running
 *                    (shown mid-screen while the work is in progress).
 * @param statusLabel Result of the PREVIOUS step: BOOT_OK, BOOT_WARN,
 *                    BOOT_FAIL, or "" to hide.
 * @param stepsCompleted  How many steps have finished (0..stepsTotal).
 * @param stepsTotal  Total number of steps (drives the bar fill + counter).
 */
static void drawProgressFrame(const char* title,
                              const char* stepLabel,
                              const char* statusLabel,
                              uint8_t stepsCompleted,
                              uint8_t stepsTotal) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Title
  u8g2.drawStr(0, 10, title);
  u8g2.drawHLine(0, 12, 128);

  // Current step
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 22, stepLabel);

  // Previous step status (right-aligned feel: just draw after label)
  if (statusLabel && statusLabel[0] != '\0') {
    // Colour-code by result: invert box for FAIL/WARN, plain text for OK
    if (strcmp(statusLabel, BOOT_OK) == 0) {
      u8g2.drawStr(0, 34, "[OK]");
    } else if (strcmp(statusLabel, BOOT_WARN) == 0) {
      // Inverted box for WARN
      u8g2.drawBox(0, 25, 40, 11);
      u8g2.setDrawColor(0);
      u8g2.drawStr(2, 34, "WARN");
      u8g2.setDrawColor(1);
    } else {
      // Inverted box for FAIL
      u8g2.drawBox(0, 25, 50, 11);
      u8g2.setDrawColor(0);
      u8g2.drawStr(2, 34, "FAILED");
      u8g2.setDrawColor(1);
    }
  }

  // Progress bar
  u8g2.drawFrame(BAR_X, BAR_Y, BAR_W + 1, BAR_H);
  if (stepsCompleted > 0 && stepsTotal > 0) {
    int fillW = (int)((long)BAR_W * stepsCompleted / stepsTotal);
    if (fillW > 0) {
      u8g2.drawBox(BAR_X + 1, BAR_Y + 1, fillW, BAR_H - 2);
    }
  }

  // Step counter
  char counter[16];
  snprintf(counter, sizeof(counter), "Step %u/%u",
           (unsigned)stepsCompleted, (unsigned)stepsTotal);
  u8g2.drawStr(0, 62, counter);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.sendBuffer();
}

/**
 * Boot-screen convenience wrapper around drawProgressFrame(). Keeps every
 * existing boot call site unchanged.
 */
static void drawBootFrame(const char* stepLabel,
                          const char* statusLabel,
                          uint8_t stepsCompleted) {
  drawProgressFrame("-- Booting ----------", stepLabel, statusLabel,
                    stepsCompleted, BOOT_STEPS_TOTAL);
}

/**
 * Show a fault-summary screen after booting with one or more failures.
 * Lists each failed/warned sensor and waits for SELECT to continue.
 *
 * faultMask bits (LSB = step 0):
 *   bit 0 = Keypad, bit 1 = Camera, bit 2 = BLE,
 *   bit 3 = pH,     bit 4 = RGB,    bit 5 = TDS
 * warnMask uses the same bit positions.
 */
static void showBootFaultSummary(uint8_t faultMask, uint8_t warnMask) {
  // Build list of affected sensors
  static const char* sensorNames[BOOT_STEPS_TOTAL] = {
    "Keypad", "Camera", "BLE", "pH", "RGB", "TDS"
  };

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "-- Boot Warnings ----");
  u8g2.drawHLine(0, 12, 128);

  u8g2.setFont(u8g2_font_5x7_tf);
  int y = 24;
  for (int i = 0; i < BOOT_STEPS_TOTAL; i++) {
    if ((faultMask >> i) & 1) {
      char line[28];
      snprintf(line, sizeof(line), "[FAIL] %s", sensorNames[i]);
      u8g2.drawStr(0, y, line);
      y += 10;
      if (y > 54) break;
    } else if ((warnMask >> i) & 1) {
      char line[28];
      snprintf(line, sizeof(line), "[WARN] %s", sensorNames[i]);
      u8g2.drawStr(0, y, line);
      y += 10;
      if (y > 54) break;
    }
  }

  u8g2.drawStr(0, 62, "SEL to continue");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.sendBuffer();

  // Wait for SELECT
  while (scanKey() != 15) {
    delay(80);
  }
}

// ============================================
// SETUP
// ============================================

void setup() {
  Serial.begin(9600);

  // ---- Free the shared I2C bus BEFORE the first OLED write ----
  // The OLED (0x3C) and AS7341 (0x39) share one I2C bus. If the previous power
  // cycle was interrupted mid-read, a slave can still be holding SDA low, which
  // would hang u8g2.begin()/sendBuffer() below. Recover the bus first so the
  // loading screen — and every later transaction — starts from a clean bus.
  i2cBusRecover();

  // Display MUST come up before any other init so the loading screen works.
  u8g2.begin();

  // ---- SH1106 2-pixel column fix ----
  // The SH1106 has 132 columns of GDDRAM but only displays 128.
  // U8g2 maps framebuffer col 0 → hardware col 2, leaving hardware
  // cols 0 and 1 unwritten. They retain power-on garbage and appear
  // as two glitchy pixels on the left edge of the screen.
  // Fix: write a single zero byte to each of those two columns on
  // every page via raw I2C, once at startup.
  {
    const uint8_t SH1106_ADDR = 0x3C;
    for (uint8_t page = 0; page < 8; page++) {
      // Set page address, then column address = 0
      Wire.beginTransmission(SH1106_ADDR);
      Wire.write(0x00);           // control byte: command stream (Co=0, D/C#=0)
      Wire.write(0xB0 | page);    // Set Page Address
      Wire.write(0x00);           // Set Low Column Address  = 0
      Wire.write(0x10);           // Set High Column Address = 0
      Wire.endTransmission();
      // Write 2 zero (black) bytes — clears hardware cols 0 and 1
      Wire.beginTransmission(SH1106_ADDR);
      Wire.write(0x40);           // control byte: data stream (Co=0, D/C#=1)
      Wire.write(0x00);           // col 0 = all pixels off
      Wire.write(0x00);           // col 1 = all pixels off
      Wire.endTransmission();
    }
  }

  // Keypad uses I2C (PCF8574); init early so scanKey() works in fault screen.
  // ---- Step 1: Keypad ----
  drawBootFrame("Keypad...", "", 0);
  keypadInit();
  // keypadInit() is void and will not fail silently — it just won't scan
  // if the PCF8574 isn't present. We treat it as always-OK here; hardware
  // faults surface as "no key response" at runtime.
  uint8_t faultMask = 0;
  uint8_t warnMask  = 0;
  drawBootFrame("Camera...", BOOT_OK, 1);

  // ---- Step 2: Camera (ESP32-CAM via UART) ----
  // This step can take up to ~8 s (ESP32 cold-boot grace period).
  // cameraSensorInit() does its own polling loop, so we just call it.
  cameraSensorInit();
  // camOnline is set by cameraSensorInit() — false means offline/not present.
  // Camera is optional hardware; treat offline as WARN, not FAIL.
  const char* camStatus = camOnline ? BOOT_OK : BOOT_WARN;
  if (!camOnline) warnMask |= (1 << 1);
  drawBootFrame("BLE...", camStatus, 2);

  // ---- Step 3: BLE ----
  bool bleOk = bluetoothInit();
  if (!bleOk) faultMask |= (1 << 2);
  drawBootFrame("pH Sensor...", bleOk ? BOOT_OK : BOOT_FAIL, 3);

  // ---- Step 4: pH Sensor ----
  pHSensorInit();
  // pHSensorInit() is void; failure (no hardware) manifests as bad readings.
  // It always falls back to EEPROM defaults so treat as OK for boot purposes.
  drawBootFrame("RGB Sensor...", BOOT_OK, 4);

  // ---- Step 5: RGB / Colour Sensor ----
  bool colorOk = colorSensorInit();
  colorCalPrint();
  if (!colorOk) faultMask |= (1 << 4);
  drawBootFrame("TDS Sensor...", colorOk ? BOOT_OK : BOOT_FAIL, 5);

  // ---- Step 6: TDS Sensor ----
  tdsSensorInit();
  // Same as pH — void, defaults to EEPROM; treat as OK for boot.
  drawBootFrame("Done.", BOOT_OK, 6);
  delay(400);   // brief pause so user sees the completed bar

  // ---- Fault summary ----
  if (faultMask || warnMask) {
    showBootFaultSummary(faultMask, warnMask);
  }

  // ---- Serial banner ----
  Serial.println("========================================");
  Serial.print("Device:  "); Serial.println(DEVICE_NAME);
  Serial.print("Type:    "); Serial.println(DEVICE_TYPE);
  Serial.print("Version: "); Serial.println(DEVICE_VERSION);
  if (faultMask) {
    Serial.print("[Boot] FAULTS (mask=0x");
    Serial.print(faultMask, HEX);
    Serial.println(")");
  }
  if (warnMask) {
    Serial.print("[Boot] WARNINGS (mask=0x");
    Serial.print(warnMask, HEX);
    Serial.println(")");
  }
  Serial.println("System initialized");
  Serial.println("========================================");

  setMenu(&mainMenu);
}

// ============================================
// LOOP
// ============================================

void loop() {
  // ---- Full-screen takeovers ----
  // Each flag is set by its menu callback and cleared by its screen runner.

  if (inPHCal) {
    runCalibrationScreen();
    return;
  }

  if (inRGBCal) {
    runRGBCalibrationScreen();
    return;
  }

  if (inTDSCal) {
    runTDSCalibrationScreen();
    return;
  }

  if (inCameraCal) {
    runCameraCalibrationScreen();
    return;
  }

  if (inBothColorCal) {
    runBothColorCalibrationScreen();
    return;
  }

  if (inIlluminatorAdjust) {
    runIlluminatorAdjustScreen();
    return;
  }

  if (inIlluminator2Adjust) {
    runIlluminator2AdjustScreen();
    return;
  }

  if (inBTSettings) {
    runBluetoothSettingsScreen();
    inBTSettings = false;
    return;
  }

  if (inDevDiagnostics) {
    runDevDiagnosticsScreen();
    return;
  }

  // ---- Normal menu loop ----
  bluetoothUpdate();

  if (hasNewData) {
    JsonDocument receivedData = getReceivedJson();
    Serial.println("Received BLE JSON:");
    serializeJsonPretty(receivedData, Serial);
    Serial.println();
    hasNewData = false;
  }

  drawMenu(u8g2);

  // ---- BLE status overlay on main menu ----
  // The main menu has only 2 items (y=26, y=38), leaving y=50..63 free.
  // We overdraw into the u8g2 buffer and call sendBuffer() again so the
  // status line appears without a visible flicker caused by an extra clear.
  if (getMenu() == &mainMenu) {
    bool connected = isBluetoothConnected();
    char bleLine[28];
    if (!bleSettings.advertisingEnabled) {
      snprintf(bleLine, sizeof(bleLine), "BT: OFF");
    } else if (connected) {
      snprintf(bleLine, sizeof(bleLine), "BT: Connected");
    } else {
      snprintf(bleLine, sizeof(bleLine), "BT: Advertising...");
    }
    u8g2.setFont(u8g2_font_5x7_tf);
    // Draw a thin divider then the status text
    u8g2.drawHLine(0, 48, 128);
    u8g2.drawStr(2, 58, bleLine);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.sendBuffer();
  }


  int key = scanKey();

  // ---- Hidden developer unlock ----
  // Only armed on the main menu. Five consecutive presses of
  // DEV_UNLOCK_KEY (key 16) triggers the dev diagnostics screen.
  // Any other key press resets the counter.
  if (getMenu() == &mainMenu) {
    if (key == DEV_UNLOCK_KEY) {
      devUnlockProgress++;
      if (devUnlockProgress >= DEV_UNLOCK_COUNT) {
        devUnlockProgress = 0;
        inDevDiagnostics  = true;
        delay(120);
        return;
      }
    } else if (key != 0) {
      // Any other actual keypress aborts the combo.
      devUnlockProgress = 0;
    }
  } else {
    devUnlockProgress = 0;
  }

  switch (key) {
    case 2:  menuUp();          break;
    case 10: menuDown();        break;
    case 15: menuSelect();      break;
    case 8:  goBackUniversal(); break;   // universal back button
    default: break;
  }

  delay(120);
}
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

static bool inBTSettings = false;
static bool inPHCal      = false;
static bool inRGBCal     = false;
static bool inTDSCal     = false;
static bool inCameraCal  = false;

// ============================================
// FORWARD DECLARATIONS
// ============================================

void startTest();
void openSettings();
void backToMain();

void openSensors();
// pH
void openPH();
void openPHCalibration();
void resetPHCalibration();
// RGB
void openRGB();
void openRGBCalibration();
void resetRGBCalibration();
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
void runBluetoothSettingsScreen();

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
    {"RGB Sensor",         openRGB},
    {"TDS Sensor",         openTDS},
    {"Camera Sensor",      openCamera},
    {"Back to Main Menu",  backToMain},
  },
  5
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
    {"RGB Cal Reset",      resetRGBCalibration},
    {"Back to Main Menu",  backToMain},
  },
  3
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
    {"Back to Main Menu",  backToMain},
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
  while (true) {
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

  inRGBCal = false;
  setMenu(&RGBMenu);
}

// ============================================
// TDS CALIBRATION SCREEN
// ============================================
//
// Two-step sequence: LOW (342 ppm) → HIGH (1000 ppm) → DONE
//
// Layout:
//   y=10  Title
//   y=12  Divider
//   y=24  Step instruction (which solution to use right now)
//   y=36  Live voltage reading (watch it settle before capturing)
//   y=48  Live TDS estimate using current calibration
//   y=62  Action hint
//
// Controls:
//   SELECT (15)  — capture / confirm save
//   UP/DN (2/10) — cancel at any point
//
void runTDSCalibrationScreen() {
  while (true) {
    float liveV   = tdsReadVoltage();
    float liveTDS = voltageToDTS(liveV, pHReadTemperature());

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "-- TDS Calibration -");
    u8g2.drawHLine(0, 12, 128);
    u8g2.drawStr(0, 24, tdsCalStepLabel());

    if (tdsCalStep != TDS_CAL_DONE) {
      char vBuf[22], tBuf[22];
      snprintf(vBuf, sizeof(vBuf), "Live: %.4f V", liveV);
      snprintf(tBuf, sizeof(tBuf), "Est:  %.0f ppm", liveTDS);
      u8g2.drawStr(0, 36, vBuf);
      u8g2.drawStr(0, 48, tBuf);
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
        snprintf(lBuf, sizeof(lBuf), "Lo %.0fppm@%.3fV",
                 tdsCalData.low.tds,  tdsCalData.low.voltage);
        snprintf(hBuf, sizeof(hBuf), "Hi %.0fppm@%.3fV",
                 tdsCalData.high.tds, tdsCalData.high.voltage);
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
          u8g2.drawStr(8, 32, "342ppm captured!");
        } else if (tdsCalStep == TDS_CAL_DONE) {
          u8g2.drawStr(4, 32, "1000ppm captured!");
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
  while (true) {
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

  inCameraCal = false;
  setMenu(&cameraMenu);
}

// ============================================
// MENU ACTION CALLBACKS
// ============================================

void startTest() {
  // ---- Splash: Reading sensors ----
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "-- Start Test ------");
  u8g2.drawHLine(0, 12, 128);
  u8g2.drawStr(16, 34, "Reading sensors...");
  u8g2.sendBuffer();

  // ---- Notify connected central that a test has begun ----
  if (isBluetoothConnected()) {
    StaticJsonDocument<64> startDoc;
    startDoc["device"] = DEVICE_NAME;
    startDoc["type"]   = "test_started";
    sendJsonData(startDoc);
    BLE.poll();
  }

  // ---- Collect all sensor data ----
  float temp = pHReadTemperature();
  float pH   = pHRead(temp);
  float tds  = tdsRead(temp);
  float ec   = tdsToEC(tds);

  RawRGBC       raw = colorReadRawAveraged();
  NormalisedRGB rgb = colorNormalise(raw);
  float         lux = colorCalcLux(raw);
  uint16_t      cct = colorCalcCCT(raw);

  char hexColor[8];
  snprintf(hexColor, sizeof(hexColor), "#%02X%02X%02X", rgb.r, rgb.g, rgb.b);

  // Camera (ESP32-CAM) — independent colour reading via image processing.
  // Returns valid=false silently if the ESP32 isn't connected or fails.
  CameraRGB cam = cameraRead();
  char hexCam[8] = "";
  if (cam.valid) {
    snprintf(hexCam, sizeof(hexCam), "#%02X%02X%02X", cam.r, cam.g, cam.b);
  }

  // ---- Serial log ----
  Serial.println("[Test] ===== New Test Result =====");
  Serial.print("[Test] Device:  "); Serial.println(DEVICE_NAME);
  Serial.print("[Test] Temp:    "); Serial.print(temp, 1); Serial.println(" C");
  Serial.print("[Test] pH:      "); Serial.println(pH, 2);
  Serial.print("[Test] TDS:     "); Serial.print(tds, 0); Serial.println(" ppm");
  Serial.print("[Test] EC:      "); Serial.print(ec, 2);  Serial.println(" uS/cm");
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
  StaticJsonDocument<768> doc;

  doc["device"]  = DEVICE_NAME;
  doc["version"] = DEVICE_VERSION;
  doc["type"]    = "urinalysis";

  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["temp_c"]   = temp;
  sensors["pH"]       = pH;
  sensors["tds_ppm"]  = tds;
  sensors["ec_us_cm"] = ec;

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
    if (k == 2 || k == 10) { setMenu(&mainMenu); return; }
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
    if (scanKey() == 15) break;
    delay(80);
  }

  setMenu(&mainMenu);
}

void openSettings()       { setMenu(&settingsMenu); }
void backToMain()         { setMenu(&mainMenu); }
void openSensors()        { setMenu(&sensorsMenu); }

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
// ============================================

static const uint8_t BOOT_STEPS_TOTAL = 6;

// Boot status tags (kept short for the small font)
#define BOOT_OK     "OK"
#define BOOT_WARN   "WARN"
#define BOOT_FAIL   "FAILED"

// Progress bar geometry
static const int BAR_X  = 0;
static const int BAR_Y  = 44;
static const int BAR_W  = 127;
static const int BAR_H  = 8;

/**
 * Draw a single boot-progress frame.
 *
 * @param stepLabel   Short description of the step currently running
 *                    (shown mid-screen while the init is in progress).
 * @param statusLabel Result of the PREVIOUS step: BOOT_OK, BOOT_WARN,
 *                    BOOT_FAIL, or "" to hide.
 * @param stepsCompleted  How many steps have finished (0..BOOT_STEPS_TOTAL).
 */
static void drawBootFrame(const char* stepLabel,
                          const char* statusLabel,
                          uint8_t stepsCompleted) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Title
  u8g2.drawStr(0, 10, "-- Booting ----------");
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
  if (stepsCompleted > 0) {
    int fillW = (int)((long)BAR_W * stepsCompleted / BOOT_STEPS_TOTAL);
    if (fillW > 0) {
      u8g2.drawBox(BAR_X + 1, BAR_Y + 1, fillW, BAR_H - 2);
    }
  }

  // Step counter
  char counter[16];
  snprintf(counter, sizeof(counter), "Step %u/%u",
           (unsigned)stepsCompleted, (unsigned)BOOT_STEPS_TOTAL);
  u8g2.drawStr(0, 62, counter);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.sendBuffer();
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

  if (inBTSettings) {
    runBluetoothSettingsScreen();
    inBTSettings = false;
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
  switch (key) {
    case 2:  menuUp();     break;
    case 10: menuDown();   break;
    case 15: menuSelect(); break;
    default: break;
  }

  delay(120);
}
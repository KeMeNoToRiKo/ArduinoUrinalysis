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
#include <U8g2lib.h>
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

void openBluetoothSettings();

// Screen runners
void runCalibrationScreen();
void runRGBCalibrationScreen();
void runTDSCalibrationScreen();
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
    {"Back to Main Menu",  backToMain},
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
      if (strLen < maxLen - 2) {
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
//   3 — Back
//
void runBluetoothAdvancedScreen();   // forward declaration

void runBluetoothSettingsScreen() {
  static const int N = 4;
  int cursor = 0;

  while (true) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "BT Settings");
    u8g2.drawHLine(0, 12, 128);

    for (int i = 0; i < N; i++) {
      int y = 26 + i * 12;
      if (y > 63) break;

      char line[32] = "";
      switch (i) {
        case 0: snprintf(line, sizeof(line), "BT:  %s",
                         bleSettings.advertisingEnabled ? "ON" : "OFF");  break;
        case 1: snprintf(line, sizeof(line), "Name: %.12s", bleSettings.localName); break;
        case 2: snprintf(line, sizeof(line), "Advanced Settings");               break;
        case 3: snprintf(line, sizeof(line), "[Back]");                          break;
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
    u8g2.drawStr(0, 62, "UP/DN:move SEL:pick key8:back");
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
        case 3:
          setMenu(&settingsMenu);
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
  Serial.println("[Test] ==================================");

  // ---- Build JSON payload ----
  StaticJsonDocument<512> doc;

  doc["device"]  = DEVICE_NAME;
  doc["version"] = DEVICE_VERSION;
  doc["type"]    = "urinalysis";

  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["temp_c"]   = serialized(String(temp, 1));
  sensors["pH"]       = serialized(String(pH,   2));
  sensors["tds_ppm"]  = serialized(String(tds,  0));
  sensors["ec_us_cm"] = serialized(String(ec,   2));

  JsonObject color = sensors.createNestedObject("color");
  color["r"]   = rgb.r;
  color["g"]   = rgb.g;
  color["b"]   = rgb.b;
  color["hex"] = hexColor;
  color["lux"] = serialized(String(lux, 1));
  color["cct"] = cct;

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

void openBluetoothSettings() { inBTSettings = true; }

// ============================================
// SETUP
// ============================================

void setup() {
  Serial.begin(9600);
  u8g2.begin();
  keypadInit();
  bluetoothInit();
  pHSensorInit();
  colorSensorInit();
  colorCalPrint();
  tdsSensorInit();

  Serial.println("========================================");
  Serial.print("Device: ");  Serial.println(DEVICE_NAME);
  Serial.print("Type: ");    Serial.println(DEVICE_TYPE);
  Serial.print("Version: "); Serial.println(DEVICE_VERSION);
  Serial.println("System initialized");
  Serial.println("========================================");

  setMenu(&mainMenu);
  delay(500);
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

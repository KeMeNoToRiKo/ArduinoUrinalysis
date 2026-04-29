#ifndef CAMERA_SENSOR_H
#define CAMERA_SENSOR_H

#include <Arduino.h>

// ============================================
// CAMERA SENSOR (ESP32-CAM via UART)
// ============================================
//
// The ESP32-CAM with OV3660 captures a frame, averages the centre ROI
// in RGB565, and applies its own dark/white calibration before
// returning a final 8-bit RGB triplet to the Arduino.
//
// Calibration data is persisted on the ESP32 side (NVS / Preferences),
// so no Arduino EEPROM space is consumed.
//
// Wire protocol — all commands are line-terminated with '\n'.
//   Arduino → ESP32:    Command        Response
//   PING              → PONG
//   READ              → RGB,r,g,b      (calibrated, 0..255 each)
//   CAL_DARK          → OK,r,g,b       (raw averaged values, for display)
//   CAL_WHITE         → OK,r,g,b
//   CAL_SAVE          → OK
//   CAL_RESET         → OK
//   CAL_GET           → CAL,dR,dG,dB,wR,wG,wB,valid
//
// On any failure the ESP32 replies "ERR,<reason>".
// ============================================

// ---- Hardware: which Arduino UART talks to the ESP32 ----
// Arduino UNO R4 WiFi: Serial1 = pins 0 (RX) / 1 (TX) — hardware UART.
#define CAM_SERIAL          Serial1
#define CAM_BAUD            115200

// ---- Timeouts (ms) ----
#define CAM_BOOT_DELAY_MS   8000   // ESP32 cold-boot grace period
#define CAM_PING_TIMEOUT_MS 1000
#define CAM_READ_TIMEOUT_MS 4000   // capture + frame settle on ESP32 side
#define CAM_CAL_TIMEOUT_MS  5000

// ============================================
// DATA STRUCTURES
// ============================================

/**
 * Calibrated 8-bit RGB returned by the ESP32-CAM.
 * `valid` is false if communication failed or the ESP32 reported an error.
 */
struct CameraRGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  bool    valid;
};

// ============================================
// CALIBRATION STATE MACHINE
// ============================================
//
// Mirrors the colourSensor flow so the menu UI follows the same pattern:
//   IDLE → DARK → WHITE → DONE → (save) → IDLE
//
enum CamCalStep {
  CAM_CAL_IDLE = 0,
  CAM_CAL_DARK,    // Cover the lens (LED off on ESP32)
  CAM_CAL_WHITE,   // Show a white reference (LED on for illumination)
  CAM_CAL_DONE     // Both points captured; ready to save
};

extern CamCalStep camCalStep;

// Last captured raw values from the most recent CAL_DARK or CAL_WHITE.
// Useful for the calibration screen so the user can see what was captured.
extern uint16_t camLastCalR;
extern uint16_t camLastCalG;
extern uint16_t camLastCalB;

// Tracks whether the ESP32-CAM responded during init.
// If false, all camera reads will return valid=false and the test routine
// can quietly skip the camera block instead of stalling.
extern bool camOnline;

// ============================================
// CORE FUNCTIONS
// ============================================

/**
 * Open Serial1 at CAM_BAUD, wait for the ESP32 to finish booting, and
 * confirm it responds to PING. Sets camOnline accordingly.
 * Safe to call even if the ESP32 isn't connected.
 */
void cameraSensorInit();

/**
 * Send a PING and return true if PONG comes back within the timeout.
 * Updates camOnline as a side effect.
 */
bool cameraIsReady();

/**
 * Trigger a capture on the ESP32 and return the calibrated 8-bit RGB.
 * Returns {0,0,0,false} on timeout or ESP32 error.
 */
CameraRGB cameraRead();

// ============================================
// CALIBRATION
// ============================================

/**
 * Start a new dark/white calibration sequence.
 * Sets camCalStep = CAM_CAL_DARK. Does not talk to the ESP32 yet.
 */
void camCalBegin();

/**
 * Perform the capture for the current step on the ESP32 and advance the
 * state machine. Updates camLastCalR/G/B with the captured raw values.
 */
void camCalCapture();

/**
 * Persist the calibration on the ESP32 (only valid when state == DONE).
 * Returns the state machine to IDLE on success.
 */
void camCalSave();

/**
 * Discard the in-progress calibration and return to IDLE.
 * The ESP32 keeps its previously-saved calibration unchanged.
 */
void camCalCancel();

/**
 * Tell the ESP32 to reset its calibration to factory defaults.
 */
void camCalResetToDefaults();

/**
 * Return a human-readable label for the current calibration step.
 */
const char* camCalStepLabel();

/**
 * Print the ESP32's current calibration data to Serial (debugging).
 */
void camCalPrint();


#endif // CAMERA_SENSOR_H
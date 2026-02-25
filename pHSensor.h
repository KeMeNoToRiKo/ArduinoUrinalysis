#ifndef PH_SENSOR_H
#define PH_SENSOR_H

#include <Arduino.h>
#include <EEPROM.h>

// ============================================
// PIN CONFIGURATION
// ============================================

// Analog pin connected to the pH sensor signal output
#define PH_SENSOR_PIN A0

// Optional: analog pin for DS18B20 / NTC thermistor temperature reading.
// If you don't have a temperature sensor, the module falls back to 25 °C.
// Set to -1 to disable.
#define TEMP_SENSOR_PIN -1

// ============================================
// CALIBRATION BUFFER SETTINGS
// ============================================

// The three standard calibration points (pH @ 25 °C)
#define CAL_PH_LOW    4.00f   // pH 4.00 buffer
#define CAL_PH_MID    6.86f   // pH 6.86 buffer
#define CAL_PH_HIGH   9.18f   // pH 9.18 buffer

// Number of ADC samples to average for a stable reading
#define PH_SAMPLE_COUNT  10
#define PH_SAMPLE_DELAY  10    // ms between samples

// ============================================
// EEPROM STORAGE
// ============================================

// EEPROM start address for calibration data.
// Adjust if other modules also use EEPROM.
#define PH_EEPROM_ADDR  0x00

// Magic number written to EEPROM so we know calibration data is valid
#define PH_EEPROM_MAGIC 0xA5

// ============================================
// TEMPERATURE COMPENSATION
// ============================================

// Nernst slope constant at 25 °C (mV/pH)
// Adjusted for temperature: slope = 0.05916 * (T_K / 298.15)
#define NERNST_SLOPE_25C  0.05916f   // V/pH at 25 °C

// ============================================
// DATA STRUCTURES
// ============================================

/**
 * Stores a single calibration point: the raw ADC voltage
 * measured while the probe was in a known-pH buffer.
 */
struct CalibrationPoint {
  float voltage;   // volts measured at this buffer
  float pH;        // known pH of the buffer
};

/**
 * Complete 3-point calibration data.
 * Saved to and loaded from EEPROM.
 */
struct pHCalibration {
  uint8_t magic;             // validity marker
  CalibrationPoint low;      // pH 4.00 point
  CalibrationPoint mid;      // pH 6.86 point
  CalibrationPoint high;     // pH 9.18 point
};

// ============================================
// CALIBRATION STATE MACHINE
// ============================================

/**
 * Used by the interactive calibration UI so the main sketch can
 * step through each buffer one at a time via menu callbacks.
 */
enum CalibrationStep {
  CAL_IDLE = 0,
  CAL_LOW,       // waiting for pH 4.00 reading
  CAL_MID,       // waiting for pH 6.86 reading
  CAL_HIGH,      // waiting for pH 9.18 reading
  CAL_DONE       // all three points captured; ready to save
};

extern CalibrationStep calStep;   // current calibration step
extern pHCalibration   calData;   // working calibration data

// ============================================
// INTERPOLATION MODE
// ============================================

enum InterpolationMode {
  INTERP_LAGRANGE,      // Quadratic through all 3 points (default, smoother)
  INTERP_PIECEWISE      // Two straight-line segments (simpler)
};

// ============================================
// FUNCTION DECLARATIONS
// ============================================

/**
 * Switch between Lagrange polynomial and piecewise linear interpolation.
 * Default on startup is INTERP_LAGRANGE.
 */
void setInterpolationMode(InterpolationMode mode);

/**
 * Get the currently active interpolation mode.
 */
InterpolationMode getInterpolationMode();

/**
 * Initialise the pH sensor module.
 * Loads stored calibration from EEPROM (if valid) or applies
 * default factory values.
 */
void pHSensorInit();

/**
 * Read the raw ADC voltage from the pH probe (averaged).
 * @return Voltage in volts (0–5 V or 0–3.3 V depending on board)
 */
float pHReadVoltage();

/**
 * Convert a raw voltage to a pH value using the current calibration.
 * Applies a piecewise-linear interpolation across the three cal points
 * and a simple temperature-compensation correction.
 *
 * @param voltage     Raw probe voltage in volts
 * @param temperature Measured temperature in °C (default 25.0)
 * @return Calculated pH value
 */
float voltageToPH(float voltage, float temperature = 25.0f);

/**
 * Convenience wrapper: read voltage then convert to pH.
 * @param temperature Measured temperature in °C (default 25.0)
 * @return pH value
 */
float pHRead(float temperature = 25.0f);

/**
 * Read temperature from an attached sensor (if configured).
 * Returns 25.0 if TEMP_SENSOR_PIN == -1 or read fails.
 */
float pHReadTemperature();

// ---- Calibration helpers ----

/**
 * Begin a new calibration sequence.
 * Resets the state machine to CAL_LOW.
 */
void calBegin();

/**
 * Capture the current probe reading for the active calibration step.
 * Call once per step after the probe has stabilised in the buffer.
 * Advances calStep automatically.
 */
void calCapture();

/**
 * Save the completed calibration to EEPROM.
 * Should only be called when calStep == CAL_DONE.
 */
void calSave();

/**
 * Discard any in-progress calibration and reload from EEPROM.
 */
void calCancel();

/**
 * Load calibration from EEPROM into calData.
 * @return true if valid data was found, false if defaults were applied.
 */
bool calLoad();

/**
 * Reset calibration to built-in factory defaults and save to EEPROM.
 * Factory defaults assume a linear 0 V = pH 14, 3.3 V = pH 0 mapping
 * which will be inaccurate but safe to start from.
 */
void calResetToDefaults();

/**
 * Return a human-readable label for the current calibration step.
 * Useful for printing on the OLED during the calibration UI.
 */
const char* calStepLabel();

/**
 * Print current calibration data to Serial (debugging).
 */
void calPrint();

#endif // PH_SENSOR_H
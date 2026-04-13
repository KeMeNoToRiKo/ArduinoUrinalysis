#ifndef TDS_SENSOR_H
#define TDS_SENSOR_H

#include <Arduino.h>
#include <EEPROM.h>

// ============================================
// PIN CONFIGURATION
// ============================================

// Analog pin connected to the TDS module SIG output
#define TDS_SENSOR_PIN A1

// ============================================
// ADC & BOARD
// ============================================

// Arduino Uno / R4: 5.0 V
// Arduino Nano 33 / MKR: 3.3 V  — change if needed
#define TDS_ADC_REF_VOLTAGE  5.0f
#define TDS_ADC_MAX          1023.0f

// ============================================
// SAMPLING
// ============================================

#define TDS_SAMPLE_COUNT  30     // samples to collect per reading window
#define TDS_SAMPLE_DELAY  10     // ms between samples

// ============================================
// CONVERSION CONSTANTS
// ============================================

// Reference temperature for TDS conversion (°C)
// All TDS values are normalised to 25 °C.
#define TDS_REFERENCE_TEMP   25.0f

// Temperature compensation coefficient per °C
// Standard value for most conductivity probes: 0.02 (2% per °C)
#define TDS_TEMP_COEFF       0.02f

// Empirical conversion factor: EC (µS/cm) → TDS (ppm)
// Most meters use 0.5 (NaCl-based) or 0.64 (KCl-based).
// The DFRobot / TDS Meter V1.0 datasheet uses 0.5.
#define TDS_CONVERSION_FACTOR  0.5f

// ============================================
// EEPROM STORAGE
// ============================================

// Sits after the colour sensor block (colour uses 0x80..~0xA0).
#define TDS_EEPROM_ADDR   0xC0
#define TDS_EEPROM_MAGIC  0xD4

// ============================================
// DATA STRUCTURES
// ============================================

/**
 * A single TDS calibration point.
 * Captured while the probe is submerged in a known-concentration solution.
 */
struct TDSCalibrationPoint {
  float voltage;   // Raw probe voltage at this reference (V)
  float tds;       // Known TDS of the reference solution (ppm)
};

/**
 * Full TDS calibration block — two-point linear calibration.
 *
 * The module output is roughly linear across typical urine TDS ranges
 * (50–1200 ppm), so two points give excellent accuracy.
 *
 * If you only have one reference solution, use single-point (offset) mode:
 * set both points to the same tds value and call tdsCalibrateOnePoint().
 *
 * Saved to / loaded from EEPROM.
 */
struct TDSCalibration {
  uint8_t             magic;   // validity marker
  TDSCalibrationPoint low;     // lower reference point  (e.g. 342 ppm / 707 µS)
  TDSCalibrationPoint high;    // upper reference point  (e.g. 1000 ppm)
  float               kFactor; // user-trimmed gain multiplier (default 1.0)
};

// ============================================
// CALIBRATION STATE MACHINE
// ============================================

enum TDSCalStep {
  TDS_CAL_IDLE = 0,
  TDS_CAL_LOW,    // Waiting for low-reference reading
  TDS_CAL_HIGH,   // Waiting for high-reference reading
  TDS_CAL_DONE    // Both points captured; ready to save
};

extern TDSCalStep    tdsCalStep;
extern TDSCalibration tdsCalData;

// ============================================
// FUNCTION DECLARATIONS
// ============================================

// ---- Core ----

/**
 * Initialise the TDS sensor module.
 * Loads stored calibration from EEPROM (if valid) or applies defaults.
 */
void tdsSensorInit();

// ---- Reading ----

/**
 * Read and return the averaged raw ADC voltage from the TDS probe.
 * Uses median filtering to reject electrical noise spikes.
 */
float tdsReadVoltage();

/**
 * Convert a raw probe voltage to TDS (ppm).
 * Applies two-point linear calibration and temperature compensation.
 *
 * @param voltage      Raw probe voltage in volts
 * @param temperature  Sample temperature in °C (default 25.0)
 * @return             TDS value in ppm
 */
float voltageToDTS(float voltage, float temperature = TDS_REFERENCE_TEMP);

/**
 * Convenience wrapper: read voltage then convert to TDS (ppm).
 * @param temperature  Sample temperature in °C (default 25.0)
 * @return             TDS value in ppm
 */
float tdsRead(float temperature = TDS_REFERENCE_TEMP);

/**
 * Convert TDS (ppm) to estimated electrical conductivity (µS/cm).
 * EC = TDS / TDS_CONVERSION_FACTOR
 */
float tdsToEC(float tds);

// ---- Calibration ----

/**
 * Begin a new two-point calibration sequence.
 * Resets state to TDS_CAL_LOW.
 * Standard reference solutions: 342 ppm (low) and 1000 ppm (high).
 */
void tdsCalBegin();

/**
 * Capture the current probe reading for the active calibration step.
 * Call once per step after the probe has stabilised in the solution.
 * Advances tdsCalStep automatically.
 */
void tdsCalCapture();

/**
 * Save the completed calibration to EEPROM.
 * Only valid when tdsCalStep == TDS_CAL_DONE.
 */
void tdsCalSave();

/**
 * Discard in-progress calibration and reload from EEPROM.
 */
void tdsCalCancel();

/**
 * Load calibration from EEPROM into tdsCalData.
 * @return true if valid data was found, false if defaults were applied.
 */
bool tdsCalLoad();

/**
 * Reset calibration to built-in factory defaults and save to EEPROM.
 * Defaults use the theoretical voltage/TDS relationship for the V1.0 module
 * at 5 V supply — usable but a proper calibration is recommended.
 */
void tdsCalResetToDefaults();

/**
 * Return a human-readable label for the current calibration step.
 */
const char* tdsCalStepLabel();

/**
 * Print current calibration data to Serial (debugging).
 */
void tdsCalPrint();

// ---- K-factor trim ----

/**
 * Manually adjust the gain multiplier applied after two-point calibration.
 * Useful for fine-tuning without repeating the full calibration sequence.
 * Value is stored in tdsCalData.kFactor and saved to EEPROM on next calSave().
 *
 * @param k  Multiplier (1.0 = no change, 1.05 = +5%, 0.95 = -5%)
 */
void tdsSetKFactor(float k);

/**
 * Return the current K-factor.
 */
float tdsGetKFactor();

#endif // TDS_SENSOR_H
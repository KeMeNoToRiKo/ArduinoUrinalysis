#ifndef COLOR_SENSOR_H
#define COLOR_SENSOR_H

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>

// ============================================
// TCS34725 I2C ADDRESS & REGISTERS
// ============================================

#define TCS34725_I2C_ADDR       0x29

#define TCS34725_COMMAND_BIT    0x80
#define TCS34725_REG_ENABLE     0x00
#define TCS34725_REG_ATIME      0x01
#define TCS34725_REG_CONTROL    0x0F
#define TCS34725_REG_ID         0x12
#define TCS34725_REG_STATUS     0x13
#define TCS34725_REG_CDATAL     0x14   // Clear channel low byte
#define TCS34725_REG_RDATAL     0x16   // Red channel low byte
#define TCS34725_REG_GDATAL     0x18   // Green channel low byte
#define TCS34725_REG_BDATAL     0x1A   // Blue channel low byte

#define TCS34725_ENABLE_PON     0x01   // Power ON
#define TCS34725_ENABLE_AEN     0x02   // ADC Enable
#define TCS34725_STATUS_AVALID  0x01   // ADC data valid flag

// Valid TCS34725 chip IDs
#define TCS34725_ID_TCS34725    0x44
#define TCS34725_ID_TCS34727    0x4D

// ============================================
// INTEGRATION TIME (ATIME register)
// Lower = faster, less sensitive; Higher = slower, more sensitive
// Integration time (ms) = (256 - ATIME) * 2.4ms
// ============================================

#define TCS34725_ATIME_2_4MS    0xFF   //   2.4 ms — 1 cycle,  max count 1024
#define TCS34725_ATIME_24MS     0xF6   //  24   ms — 10 cycles, max count 10240
#define TCS34725_ATIME_50MS     0xEB   //  50   ms — 20 cycles, max count 20480
#define TCS34725_ATIME_101MS    0xD5   // 101   ms — 42 cycles, max count 43008
#define TCS34725_ATIME_154MS    0xC0   // 154   ms — 64 cycles, max count 65535
#define TCS34725_ATIME_700MS    0x00   // 700   ms — 256 cycles, max count 65535

// Default integration time
#define TCS34725_DEFAULT_ATIME  TCS34725_ATIME_154MS

// ============================================
// GAIN (CONTROL register)
// ============================================

#define TCS34725_GAIN_1X        0x00
#define TCS34725_GAIN_4X        0x01
#define TCS34725_GAIN_16X       0x02
#define TCS34725_GAIN_60X       0x03

// Default gain
#define TCS34725_DEFAULT_GAIN   TCS34725_GAIN_4X

// ============================================
// SAMPLING
// ============================================

#define COLOR_SAMPLE_COUNT  5        // Readings to average per measurement
#define COLOR_SAMPLE_DELAY  20       // ms between samples (allow ADC to settle)

// ============================================
// EEPROM STORAGE
// ============================================

// Sits after the BLE block (BLE uses 0x40..~0x6F).
// Adjust if your layout changes.
#define COLOR_EEPROM_ADDR   0x80
#define COLOR_EEPROM_MAGIC  0xC7

// ============================================
// DATA STRUCTURES
// ============================================

/**
 * Raw 16-bit RGBC readings from the TCS34725 ADC.
 */
struct RawRGBC {
  uint16_t r;
  uint16_t g;
  uint16_t b;
  uint16_t c;   // Clear (unfiltered) channel
};

/**
 * Normalised [0–255] RGB values derived from a raw RGBC read.
 * Calculated by dividing each channel by the clear channel, then scaling.
 */
struct NormalisedRGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

/**
 * A single white-balance calibration point.
 * Captured by pointing the sensor at a known reference surface.
 */
struct ColorCalibrationPoint {
  uint16_t r;   // Raw red channel value at this reference
  uint16_t g;   // Raw green channel value at this reference
  uint16_t b;   // Raw blue channel value at this reference
  uint16_t c;   // Raw clear channel value at this reference
};

/**
 * Full colour calibration block.
 * Two-point white/dark balance lets us correct for sensor offset and gain.
 *
 *   corrected = (raw - dark) / (white - dark)
 *
 * Saved to / loaded from EEPROM.
 */
struct ColorCalibration {
  uint8_t               magic;       // Validity marker
  ColorCalibrationPoint white;       // Reading under a white (reference) surface
  ColorCalibrationPoint dark;        // Reading with sensor covered (zero reference)
  uint8_t               atime;       // Integration time register value
  uint8_t               gain;        // Gain register value
};

// ============================================
// CALIBRATION STATE MACHINE
// ============================================

/**
 * Mirrors the CalibrationStep approach in pHSensor — lets the menu
 * walk the user through each step one button-press at a time.
 */
enum ColorCalStep {
  COLOR_CAL_IDLE = 0,
  COLOR_CAL_DARK,    // Waiting for covered/dark reading
  COLOR_CAL_WHITE,   // Waiting for white-reference reading
  COLOR_CAL_DONE     // Both points captured; ready to save
};

extern ColorCalStep colorCalStep;
extern ColorCalibration colorCalData;

// ============================================
// FUNCTION DECLARATIONS
// ============================================

// ---- Core ----

/**
 * Initialise the TCS34725 over I2C.
 * Loads calibration from EEPROM (or applies defaults if none found).
 * Returns true on success, false if the sensor is not detected.
 */
bool colorSensorInit();

/**
 * Apply the active atime and gain settings to the sensor.
 * Call after changing colorCalData.atime or colorCalData.gain at runtime.
 */
void colorSensorApplySettings();

// ---- Reading ----

/**
 * Read a single RGBC sample from the TCS34725.
 * Blocks until the ADC data-valid flag is set (up to ~700 ms with slow ATIME).
 */
RawRGBC colorReadRaw();

/**
 * Read and average COLOR_SAMPLE_COUNT raw samples.
 */
RawRGBC colorReadRawAveraged();

/**
 * Convert a raw RGBC reading into normalised [0–255] RGB,
 * applying the stored white/dark calibration.
 *
 * @param raw    Raw RGBC reading to convert
 * @return       Calibration-corrected 8-bit RGB
 */
NormalisedRGB colorNormalise(const RawRGBC& raw);

/**
 * Convenience wrapper: read averaged raw RGBC, apply calibration,
 * return normalised RGB.
 */
NormalisedRGB colorRead();

/**
 * Estimate the correlated colour temperature (CCT) in Kelvin
 * from a raw RGBC reading using Hernandez-Andres' formula.
 * Returns 0 if the clear channel is zero (sensor covered / dark).
 */
uint16_t colorCalcCCT(const RawRGBC& raw);

/**
 * Estimate illuminance (lux) from a raw RGBC reading
 * using the manufacturer's simplified lux equation.
 * Returns 0 if the clear channel is zero.
 */
float colorCalcLux(const RawRGBC& raw);

/**
 * Print a full colour report to Serial:
 * raw RGBC, normalised RGB (hex + decimal), lux, and CCT.
 */
void colorPrintReport(const RawRGBC& raw, const NormalisedRGB& norm);

// ---- Calibration ----

/**
 * Begin a new calibration sequence; resets state to COLOR_CAL_DARK.
 */
void colorCalBegin();

/**
 * Capture the current sensor reading for the active calibration step.
 * Advances colorCalStep automatically.
 */
void colorCalCapture();

/**
 * Save the completed calibration to EEPROM.
 * Only valid when colorCalStep == COLOR_CAL_DONE.
 */
void colorCalSave();

/**
 * Discard in-progress calibration and reload from EEPROM.
 */
void colorCalCancel();

/**
 * Load calibration from EEPROM into colorCalData.
 * @return true if valid data was found, false if defaults were applied.
 */
bool colorCalLoad();

/**
 * Reset calibration to built-in factory defaults and save to EEPROM.
 * Defaults assume a neutral white reference and a zero dark offset —
 * usable but a proper calibration will give much better results.
 */
void colorCalResetToDefaults();

/**
 * Return a human-readable label for the current calibration step.
 * Useful for printing on the OLED during the calibration UI.
 */
const char* colorCalStepLabel();

/**
 * Print current calibration data to Serial (debugging).
 */
void colorCalPrint();

// ---- Settings helpers ----

/**
 * Set the integration time (ATIME register value).
 * Use one of the TCS34725_ATIME_* constants.
 * Applies immediately and saves to calibration struct.
 */
void colorSetIntegrationTime(uint8_t atime);

/**
 * Set the analogue gain.
 * Use one of the TCS34725_GAIN_* constants.
 * Applies immediately and saves to calibration struct.
 */
void colorSetGain(uint8_t gain);

/**
 * Return the current integration time register value.
 */
uint8_t colorGetIntegrationTime();

/**
 * Return the current gain register value.
 */
uint8_t colorGetGain();

#endif // COLOR_SENSOR_H
#include "pHSensor.h"

// ============================================
// GLOBALS
// ============================================

CalibrationStep calStep = CAL_IDLE;
pHCalibration   calData;

// ADC reference voltage for the board.
// Arduino Uno / R4: 5.0 V
// Arduino Nano 33 / MKR: 3.3 V  -- change if needed.
static const float ADC_REF_VOLTAGE = 5.0f;
static const float ADC_MAX         = 1023.0f;

// ============================================
// INITIALISATION
// ============================================

void pHSensorInit() {
  pinMode(PH_SENSOR_PIN, INPUT);

#if TEMP_SENSOR_PIN >= 0
  pinMode(TEMP_SENSOR_PIN, INPUT);
#endif

  if (!calLoad()) {
    Serial.println("[pH] No valid EEPROM calibration found — using defaults.");
    calResetToDefaults();   // saves defaults back to EEPROM
  } else {
    Serial.println("[pH] Calibration loaded from EEPROM.");
    calPrint();
  }
}

// ============================================
// READING
// ============================================

float pHReadVoltage() {
  long sum = 0;
  for (int i = 0; i < PH_SAMPLE_COUNT; i++) {
    sum += analogRead(PH_SENSOR_PIN);
    delay(PH_SAMPLE_DELAY);
  }
  float avgADC = (float)sum / PH_SAMPLE_COUNT;
  return avgADC * (ADC_REF_VOLTAGE / ADC_MAX);
}

// Active interpolation mode (change via setInterpolationMode())
static InterpolationMode interpMode = INTERP_LAGRANGE;

void setInterpolationMode(InterpolationMode mode) {
  interpMode = mode;
  Serial.print("[pH] Interpolation mode set to: ");
  Serial.println(mode == INTERP_LAGRANGE ? "Lagrange polynomial" : "Piecewise linear");
}

InterpolationMode getInterpolationMode() {
  return interpMode;
}

// ---- Internal helpers ----

/**
 * Piecewise-linear interpolation.
 * Splits the voltage range at the midpoint and fits a separate line
 * through each pair of adjacent calibration points.
 */
static float piecewiseLinear(float voltage,
                              float vLow,  float pLow,
                              float vMid,  float pMid,
                              float vHigh, float pHigh) {
  if (vMid == vLow && vHigh == vMid) return pMid;  // degenerate guard

  if (voltage >= vMid) {
    if (vHigh == vMid) return pMid;
    float t = (voltage - vMid) / (vHigh - vMid);
    return pMid + t * (pHigh - pMid);
  } else {
    if (vMid == vLow) return pLow;
    float t = (voltage - vLow) / (vMid - vLow);
    return pLow + t * (pMid - pLow);
  }
}

/**
 * Lagrange quadratic interpolation through all three calibration points.
 *
 * Fits a single smooth quadratic curve that passes exactly through every
 * calibration point — no kink at the midpoint like piecewise linear has.
 *
 *   pH = pLow  * (V-vMid)(V-vHigh) / (vLow-vMid)(vLow-vHigh)
 *      + pMid  * (V-vLow)(V-vHigh) / (vMid-vLow)(vMid-vHigh)
 *      + pHigh * (V-vLow)(V-vMid)  / (vHigh-vLow)(vHigh-vMid)
 */
static float lagrangePolynomial(float voltage,
                                 float vLow,  float pLow,
                                 float vMid,  float pMid,
                                 float vHigh, float pHigh) {
  float d0 = (vLow  - vMid) * (vLow  - vHigh);
  float d1 = (vMid  - vLow) * (vMid  - vHigh);
  float d2 = (vHigh - vLow) * (vHigh - vMid);

  // Guard against degenerate calibration (two identical voltages)
  if (fabsf(d0) < 1e-6f || fabsf(d1) < 1e-6f || fabsf(d2) < 1e-6f) {
    // Fall back to piecewise linear if points are too close
    return piecewiseLinear(voltage, vLow, pLow, vMid, pMid, vHigh, pHigh);
  }

  float L0 = pLow  * ((voltage - vMid)  * (voltage - vHigh)) / d0;
  float L1 = pMid  * ((voltage - vLow)  * (voltage - vHigh)) / d1;
  float L2 = pHigh * ((voltage - vLow)  * (voltage - vMid))  / d2;

  return L0 + L1 + L2;
}

// ---- Public conversion function ----

/**
 * Convert a raw probe voltage to pH using the active interpolation mode,
 * then apply a Nernst temperature compensation correction.
 */
float voltageToPH(float voltage, float temperature) {
  // Temperature compensation factor (ratio of actual to 25 °C Nernst slope)
  float tempKelvin = temperature + 273.15f;
  float tempFactor = tempKelvin / 298.15f;   // 1.0 at 25 °C

  float vLow  = calData.low.voltage;
  float vMid  = calData.mid.voltage;
  float vHigh = calData.high.voltage;
  float pLow  = calData.low.pH;
  float pMid  = calData.mid.pH;
  float pHigh = calData.high.pH;

  float pH;

  if (interpMode == INTERP_LAGRANGE) {
    pH = lagrangePolynomial(voltage, vLow, pLow, vMid, pMid, vHigh, pHigh);
  } else {
    pH = piecewiseLinear(voltage, vLow, pLow, vMid, pMid, vHigh, pHigh);
  }

  // Temperature compensation: scale deviation from the neutral midpoint
  // pH_corrected = pMid + (pH_raw - pMid) / tempFactor
  pH = pMid + (pH - pMid) / tempFactor;

  return constrain(pH, 0.0f, 14.0f);
}

float pHRead(float temperature) {
  float v = pHReadVoltage();
  return voltageToPH(v, temperature);
}

float pHReadTemperature() {
#if TEMP_SENSOR_PIN >= 0
  // Basic NTC / analogue thermistor read.
  // Replace with your actual temperature sensor library call if needed.
  int raw = analogRead(TEMP_SENSOR_PIN);
  // Placeholder linear conversion — update for your thermistor's curve.
  float voltage = raw * (ADC_REF_VOLTAGE / ADC_MAX);
  float tempC   = (voltage - 0.5f) * 100.0f;   // e.g. LM35-style
  return tempC;
#else
  return 25.0f;   // assume room temperature if no sensor fitted
#endif
}

// ============================================
// CALIBRATION — STATE MACHINE
// ============================================

void calBegin() {
  calStep = CAL_LOW;
  Serial.println("[pH] Calibration started. Place probe in pH 4.00 buffer.");
}

void calCapture() {
  float v = pHReadVoltage();

  switch (calStep) {
    case CAL_LOW:
      calData.low.voltage = v;
      calData.low.pH      = CAL_PH_LOW;
      Serial.print("[pH] pH 4.00 captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[pH] Place probe in pH 6.86 buffer.");
      calStep = CAL_MID;
      break;

    case CAL_MID:
      calData.mid.voltage = v;
      calData.mid.pH      = CAL_PH_MID;
      Serial.print("[pH] pH 6.86 captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[pH] Place probe in pH 9.18 buffer.");
      calStep = CAL_HIGH;
      break;

    case CAL_HIGH:
      calData.high.voltage = v;
      calData.high.pH      = CAL_PH_HIGH;
      Serial.print("[pH] pH 9.18 captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[pH] All points captured. Call calSave() to store.");
      calStep = CAL_DONE;
      break;

    case CAL_DONE:
      Serial.println("[pH] Already done — call calSave() or calBegin() to restart.");
      break;

    default:
      Serial.println("[pH] calCapture() called outside calibration sequence.");
      break;
  }
}

void calSave() {
  if (calStep != CAL_DONE) {
    Serial.println("[pH] Cannot save — calibration not complete.");
    return;
  }

  calData.magic = PH_EEPROM_MAGIC;
  EEPROM.put(PH_EEPROM_ADDR, calData);
  calStep = CAL_IDLE;

  Serial.println("[pH] Calibration saved to EEPROM.");
  calPrint();
}

void calCancel() {
  calLoad();
  calStep = CAL_IDLE;
  Serial.println("[pH] Calibration cancelled. Previous data restored.");
}

bool calLoad() {
  EEPROM.get(PH_EEPROM_ADDR, calData);
  if (calData.magic != PH_EEPROM_MAGIC) {
    return false;
  }
  return true;
}

void calResetToDefaults() {
  // These default voltages are rough estimates for a typical pH analog module
  // powered at 5 V with a linear output. They will give usable readings but
  // a proper calibration with real buffers is strongly recommended.
  calData.magic          = PH_EEPROM_MAGIC;

  calData.low.pH         = CAL_PH_LOW;    // 4.00
  calData.low.voltage    = 3.05f;         // approx for most analog pH modules

  calData.mid.pH         = CAL_PH_MID;   // 6.86
  calData.mid.voltage    = 2.50f;

  calData.high.pH        = CAL_PH_HIGH;  // 9.18
  calData.high.voltage   = 2.00f;

  EEPROM.put(PH_EEPROM_ADDR, calData);
  Serial.println("[pH] Default calibration applied and saved to EEPROM.");
}

const char* calStepLabel() {
  switch (calStep) {
    case CAL_IDLE: return "Idle";
    case CAL_LOW:  return "Put probe in pH 4.00";
    case CAL_MID:  return "Put probe in pH 6.86";
    case CAL_HIGH: return "Put probe in pH 9.18";
    case CAL_DONE: return "Press SELECT to save";
    default:       return "Unknown";
  }
}

void calPrint() {
  Serial.println("[pH] --- Calibration Data ---");
  Serial.print("  Low  | pH ");  Serial.print(calData.low.pH,  2);
  Serial.print(" @ "); Serial.print(calData.low.voltage,  4); Serial.println(" V");
  Serial.print("  Mid  | pH ");  Serial.print(calData.mid.pH,  2);
  Serial.print(" @ "); Serial.print(calData.mid.voltage,  4); Serial.println(" V");
  Serial.print("  High | pH ");  Serial.print(calData.high.pH, 2);
  Serial.print(" @ "); Serial.print(calData.high.voltage, 4); Serial.println(" V");
  Serial.println("[pH] ----------------------------");
}
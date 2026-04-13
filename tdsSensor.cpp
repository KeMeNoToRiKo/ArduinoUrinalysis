#include "tdsSensor.h"

// ============================================
// GLOBALS
// ============================================

TDSCalStep    tdsCalStep = TDS_CAL_IDLE;
TDSCalibration tdsCalData;

// ============================================
// INITIALISATION
// ============================================

void tdsSensorInit() {
  pinMode(TDS_SENSOR_PIN, INPUT);

  if (!tdsCalLoad()) {
    Serial.println("[TDS] No valid EEPROM calibration found — using defaults.");
    tdsCalResetToDefaults();
  } else {
    Serial.println("[TDS] Calibration loaded from EEPROM.");
    tdsCalPrint();
  }
}

// ============================================
// READING
// ============================================

/**
 * Median filter helper.
 * Sorts a small array in-place and returns the middle value.
 * Rejects single-sample noise spikes far better than a simple average.
 */
static float medianOf(float* arr, int n) {
  // Insertion sort (fine for small n)
  for (int i = 1; i < n; i++) {
    float key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
  // Return middle element (or average of two middle elements for even n)
  if (n % 2 == 1) return arr[n / 2];
  return (arr[n / 2 - 1] + arr[n / 2]) * 0.5f;
}

float tdsReadVoltage() {
  float samples[TDS_SAMPLE_COUNT];

  for (int i = 0; i < TDS_SAMPLE_COUNT; i++) {
    samples[i] = analogRead(TDS_SENSOR_PIN) * (TDS_ADC_REF_VOLTAGE / TDS_ADC_MAX);
    delay(TDS_SAMPLE_DELAY);
  }

  return medianOf(samples, TDS_SAMPLE_COUNT);
}

float voltageToDTS(float voltage, float temperature) {
  // ---- Temperature compensation ----
  // Normalise voltage to 25 °C equivalent.
  // conductivity increases ~2% per °C above 25, so we scale voltage down
  // to what it would be at 25 °C:
  //   V_25 = V_raw / (1 + TDS_TEMP_COEFF * (T - TDS_REFERENCE_TEMP))
  float tempCompVoltage = voltage / (1.0f + TDS_TEMP_COEFF * (temperature - TDS_REFERENCE_TEMP));

  // ---- Two-point linear calibration ----
  // Map the temperature-compensated voltage onto TDS using the two
  // calibration points:  tds = slope * V + offset
  float vLow  = tdsCalData.low.voltage;
  float vHigh = tdsCalData.high.voltage;
  float tLow  = tdsCalData.low.tds;
  float tHigh = tdsCalData.high.tds;

  float tds;

  if (fabsf(vHigh - vLow) < 1e-4f) {
    // Degenerate calibration — both points identical, fall back to
    // the single-point theoretical formula used in the DFRobot example:
    //   TDS = (133.42 * V^3 - 255.86 * V^2 + 857.39 * V) * 0.5
    tds = (133.42f * tempCompVoltage * tempCompVoltage * tempCompVoltage
         - 255.86f * tempCompVoltage * tempCompVoltage
         + 857.39f * tempCompVoltage)
         * TDS_CONVERSION_FACTOR;
  } else {
    float slope  = (tHigh - tLow) / (vHigh - vLow);
    float offset = tLow - slope * vLow;
    tds = slope * tempCompVoltage + offset;
  }

  // Apply K-factor trim and clamp to a sensible range
  tds *= tdsCalData.kFactor;
  return constrain(tds, 0.0f, 9999.0f);
}

float tdsRead(float temperature) {
  float v = tdsReadVoltage();
  return voltageToDTS(v, temperature);
}

float tdsToEC(float tds) {
  return tds / TDS_CONVERSION_FACTOR;
}

// ============================================
// CALIBRATION — STATE MACHINE
// ============================================

void tdsCalBegin() {
  tdsCalStep = TDS_CAL_LOW;
  Serial.println("[TDS] Calibration started.");
  Serial.println("[TDS] Step 1: Submerge probe in 342 ppm solution, then press CAPTURE.");
}

void tdsCalCapture() {
  float v = tdsReadVoltage();

  switch (tdsCalStep) {
    case TDS_CAL_LOW:
      tdsCalData.low.voltage = v;
      tdsCalData.low.tds     = 342.0f;   // standard 342 ppm / 684 µS NaCl solution
      Serial.print("[TDS] Low reference captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[TDS] Step 2: Submerge probe in 1000 ppm solution, then press CAPTURE.");
      tdsCalStep = TDS_CAL_HIGH;
      break;

    case TDS_CAL_HIGH:
      tdsCalData.high.voltage = v;
      tdsCalData.high.tds     = 1000.0f;  // standard 1000 ppm NaCl solution
      Serial.print("[TDS] High reference captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[TDS] Both points captured. Call tdsCalSave() to store.");
      tdsCalStep = TDS_CAL_DONE;
      break;

    case TDS_CAL_DONE:
      Serial.println("[TDS] Already done — call tdsCalSave() or tdsCalBegin() to restart.");
      break;

    default:
      Serial.println("[TDS] tdsCalCapture() called outside calibration sequence.");
      break;
  }
}

void tdsCalSave() {
  if (tdsCalStep != TDS_CAL_DONE) {
    Serial.println("[TDS] Cannot save — calibration not complete.");
    return;
  }

  tdsCalData.magic = TDS_EEPROM_MAGIC;
  EEPROM.put(TDS_EEPROM_ADDR, tdsCalData);
  tdsCalStep = TDS_CAL_IDLE;

  Serial.println("[TDS] Calibration saved to EEPROM.");
  tdsCalPrint();
}

void tdsCalCancel() {
  tdsCalLoad();
  tdsCalStep = TDS_CAL_IDLE;
  Serial.println("[TDS] Calibration cancelled. Previous data restored.");
}

bool tdsCalLoad() {
  EEPROM.get(TDS_EEPROM_ADDR, tdsCalData);
  if (tdsCalData.magic != TDS_EEPROM_MAGIC) {
    return false;
  }
  return true;
}

void tdsCalResetToDefaults() {
  // Default calibration derived from the DFRobot V1.0 module datasheet
  // characteristic curve at 5 V supply.
  // Low point:  342 ppm @ ~0.75 V
  // High point: 1000 ppm @ ~1.95 V
  // These are approximate — a real calibration is strongly recommended.
  tdsCalData.magic = TDS_EEPROM_MAGIC;

  tdsCalData.low.voltage  = 0.75f;
  tdsCalData.low.tds      = 342.0f;

  tdsCalData.high.voltage = 1.95f;
  tdsCalData.high.tds     = 1000.0f;

  tdsCalData.kFactor = 1.0f;

  EEPROM.put(TDS_EEPROM_ADDR, tdsCalData);
  Serial.println("[TDS] Default calibration applied and saved to EEPROM.");
}

const char* tdsCalStepLabel() {
  switch (tdsCalStep) {
    case TDS_CAL_IDLE: return "Idle";
    case TDS_CAL_LOW:  return "Probe in 342ppm soln";
    case TDS_CAL_HIGH: return "Probe in 1000ppm soln";
    case TDS_CAL_DONE: return "Press SELECT to save";
    default:           return "Unknown";
  }
}

void tdsCalPrint() {
  Serial.println("[TDS] --- Calibration Data ---");
  Serial.print  ("  Low  | ");  Serial.print(tdsCalData.low.tds,  1);
  Serial.print  (" ppm @ ");    Serial.print(tdsCalData.low.voltage,  4); Serial.println(" V");
  Serial.print  ("  High | ");  Serial.print(tdsCalData.high.tds, 1);
  Serial.print  (" ppm @ ");    Serial.print(tdsCalData.high.voltage, 4); Serial.println(" V");
  Serial.print  ("  K-factor: "); Serial.println(tdsCalData.kFactor, 4);
  Serial.println("[TDS] ----------------------------");
}

// ============================================
// K-FACTOR TRIM
// ============================================

void tdsSetKFactor(float k) {
  tdsCalData.kFactor = k;
  Serial.print("[TDS] K-factor set to: ");
  Serial.println(k, 4);
}

float tdsGetKFactor() {
  return tdsCalData.kFactor;
}
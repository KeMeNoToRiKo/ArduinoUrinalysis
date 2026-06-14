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

/**
 * Median-of-N helper. In-place insertion sort then return middle element.
 * For small N (PH_SAMPLE_COUNT = 10) insertion sort is faster than any
 * partial-sort cleverness and uses no heap.
 */
static float medianOfPH(float* arr, int n) {
  for (int i = 1; i < n; i++) {
    float key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
  if (n % 2 == 1) return arr[n / 2];
  return (arr[n / 2 - 1] + arr[n / 2]) * 0.5f;
}

float pHReadVoltage() {
  float samples[PH_SAMPLE_COUNT];
  for (int i = 0; i < PH_SAMPLE_COUNT; i++) {
    samples[i] = analogRead(PH_SENSOR_PIN) * (ADC_REF_VOLTAGE / ADC_MAX);
    delay(PH_SAMPLE_DELAY);
  }
  return medianOfPH(samples, PH_SAMPLE_COUNT);
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
 * Find the calibration voltage corresponding to the isopotential pH (~7.00).
 *
 * The probe's isopotential point is where output voltage is independent
 * of temperature. We don't directly measure it; we estimate it from the
 * three calibration points by interpolating the voltage at PH_ISOPOTENTIAL
 * using the same Lagrange polynomial we use for pH lookup, but in reverse
 * (pH → voltage rather than voltage → pH).
 *
 * This is computed once per call to voltageToPH() rather than cached so
 * that re-calibration takes effect immediately without a separate refresh.
 */
static float computeIsoVoltage() {
  // Lagrange interpolation in (pH, voltage) space at point pH = ISO
  float pL = calData.low.pH,  vL = calData.low.voltage;
  float pM = calData.mid.pH,  vM = calData.mid.voltage;
  float pH_ = calData.high.pH, vH = calData.high.voltage;
  float P  = PH_ISOPOTENTIAL;

  float d0 = (pL - pM) * (pL - pH_);
  float d1 = (pM - pL) * (pM - pH_);
  float d2 = (pH_ - pL) * (pH_ - pM);

  if (fabsf(d0) < 1e-6f || fabsf(d1) < 1e-6f || fabsf(d2) < 1e-6f) {
    return vM;   // safe fallback: assume iso voltage ≈ mid-buffer voltage
  }

  return vL  * ((P - pM) * (P - pH_)) / d0
       + vM  * ((P - pL) * (P - pH_)) / d1
       + vH  * ((P - pL) * (P - pM))  / d2;
}

/**
 * Convert a raw probe voltage to pH.
 *
 * Step 1: Temperature-compensate the voltage IN VOLTAGE SPACE around the
 *         isopotential point. The Nernst slope scales as T_K/298.15, so a
 *         given voltage deviation from V_iso corresponds to a smaller pH
 *         deviation at higher T. We rescale the voltage so the subsequent
 *         interpolation (which encodes the 25 °C-equivalent calibration
 *         curve) sees the "as if at 25 °C" voltage.
 *
 *           V_25 = V_iso + (V - V_iso) / (T_K / 298.15)
 *
 *         Note: this relies on the calibration having been performed at
 *         (or near) 25 °C, which is the assumption every pH module datasheet
 *         makes for its three-point calibration procedure.
 *
 * Step 2: Interpolate. Lagrange quadratic inside the calibrated range,
 *         linear extension outside (Lagrange extrapolation curls badly).
 *
 * Step 3: Clamp to [0, 14].
 */
float voltageToPH(float voltage, float temperature) {
  // ---- Step 1: temperature compensation in voltage space ----
  float tempKelvin = temperature + 273.15f;
  float tempFactor = tempKelvin / 298.15f;     // 1.0 at 25 °C
  if (tempFactor < 0.5f) tempFactor = 0.5f;    // sanity floor (-136 °C!)

  float vIso     = computeIsoVoltage();
  float voltage25 = vIso + (voltage - vIso) / tempFactor;

  // ---- Step 2: interpolation ----
  float vLow  = calData.low.voltage;
  float vMid  = calData.mid.voltage;
  float vHigh = calData.high.voltage;
  float pLow  = calData.low.pH;
  float pMid  = calData.mid.pH;
  float pHigh = calData.high.pH;

  // For typical glass probes voltage DECREASES as pH rises, so the
  // calibration order in voltage space is vLow > vMid > vHigh. Determine
  // the inside/outside region by comparing against the extreme cal voltages.
  float vMin = fminf(vLow, vHigh);
  float vMax = fmaxf(vLow, vHigh);

  float pH;

  if (voltage25 >= vMin && voltage25 <= vMax) {
    // Inside calibrated range — use chosen interpolation mode.
    if (interpMode == INTERP_LAGRANGE) {
      pH = lagrangePolynomial(voltage25, vLow, pLow, vMid, pMid, vHigh, pHigh);
    } else {
      pH = piecewiseLinear(voltage25, vLow, pLow, vMid, pMid, vHigh, pHigh);
    }
  } else {
    // Outside calibrated range — linear extension of the nearest segment.
    // Lagrange extrapolation curls aggressively and can even go non-
    // monotonic, so we never use it past the cal points.
    if (vLow > vHigh) {
      // Standard probe orientation: high V = low pH
      if (voltage25 > vLow) {
        // Below pH_LOW — extend the low/mid line
        float slope = (pMid - pLow) / (vMid - vLow);
        pH = pLow + slope * (voltage25 - vLow);
      } else {
        // Above pH_HIGH — extend the mid/high line
        float slope = (pHigh - pMid) / (vHigh - vMid);
        pH = pHigh + slope * (voltage25 - vHigh);
      }
    } else {
      // Inverted orientation (rare, but support it). Mirror logic.
      if (voltage25 < vLow) {
        float slope = (pMid - pLow) / (vMid - vLow);
        pH = pLow + slope * (voltage25 - vLow);
      } else {
        float slope = (pHigh - pMid) / (vHigh - vMid);
        pH = pHigh + slope * (voltage25 - vHigh);
      }
    }
  }

  // ---- Step 3: clamp ----
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

/**
 * Sample the probe over PH_CAL_STABILITY_WINDOW_MS and report:
 *   - the median voltage across the window
 *   - the peak-to-peak spread (max - min) across the window
 *
 * If the spread is large, the probe hasn't equilibrated yet and the
 * caller should refuse to capture.
 */
static void measureStability(float& median, float& spread) {
  const int N = PH_CAL_STABILITY_WINDOW_MS / PH_CAL_STABILITY_SAMPLE_MS;
  // N is at most ~20 for the default 2 s / 100 ms settings — fine on stack.
  float buf[N];
  for (int i = 0; i < N; i++) {
    // Each sample is itself a median of PH_SAMPLE_COUNT raw reads, so
    // we get robust filtering at two levels: in-sample noise rejection
    // here, and across-sample equilibration detection in the caller.
    buf[i] = pHReadVoltage();
    if (i < N - 1) delay(PH_CAL_STABILITY_SAMPLE_MS);
  }

  // Min / max across the window
  float vMin = buf[0], vMax = buf[0];
  for (int i = 1; i < N; i++) {
    if (buf[i] < vMin) vMin = buf[i];
    if (buf[i] > vMax) vMax = buf[i];
  }
  spread = vMax - vMin;

  median = medianOfPH(buf, N);
}

bool calCapture() {
  if (calStep != CAL_LOW && calStep != CAL_MID && calStep != CAL_HIGH) {
    if (calStep == CAL_DONE) {
      Serial.println("[pH] Already done — call calSave() or calBegin() to restart.");
    } else {
      Serial.println("[pH] calCapture() called outside calibration sequence.");
    }
    return false;
  }

  float v, spread;
  measureStability(v, spread);

  Serial.print("[pH] Stability: spread = ");
  Serial.print(spread * 1000.0f, 1);
  Serial.println(" mV over window.");

  if (spread > PH_CAL_STABILITY_MAX_SPREAD) {
    Serial.print("[pH] REJECTED: probe not stable (need <");
    Serial.print(PH_CAL_STABILITY_MAX_SPREAD * 1000.0f, 0);
    Serial.println(" mV). Wait for equilibration and retry.");
    return false;
  }

  switch (calStep) {
    case CAL_LOW:
      calData.low.voltage = v;
      calData.low.pH      = CAL_PH_LOW;
      Serial.print("[pH] pH 4.00 captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[pH] Place probe in pH 6.86 buffer.");
      calStep = CAL_MID;
      return true;

    case CAL_MID:
      calData.mid.voltage = v;
      calData.mid.pH      = CAL_PH_MID;
      Serial.print("[pH] pH 6.86 captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[pH] Place probe in pH 9.18 buffer.");
      calStep = CAL_HIGH;
      return true;

    case CAL_HIGH:
      calData.high.voltage = v;
      calData.high.pH      = CAL_PH_HIGH;
      Serial.print("[pH] pH 9.18 captured. Voltage = ");
      Serial.println(v, 4);
      Serial.println("[pH] All points captured. Call calSave() to store.");
      calStep = CAL_DONE;
      return true;

    default:
      return false;   // unreachable, silences warning
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
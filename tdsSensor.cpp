#include "tdsSensor.h"

// ============================================
// GLOBALS
// ============================================

TDSCalStep     tdsCalStep = TDS_CAL_IDLE;
TDSCalibration tdsCalData;

// ADC reference voltage / resolution for the board.
// MUST match every other analog module (pHSensor uses the same): the sketch
// never calls analogReadResolution(), so the ADC is 10-bit (1023) at 5.0 V.
// Changing either here would silently corrupt pH and temperature readings too.
static const float ADC_REF_VOLTAGE = 5.0f;
static const float ADC_MAX         = 1023.0f;

// Tracks whether the high-side power gate is currently energised. Lets
// tdsReadVoltage() decide between "just sample" (probe already on, e.g. the
// calibration screen or the test sequence pinned it) and a self-contained
// on -> settle -> read -> off cycle for one-shot reads.
static bool tdsPowered = false;

// Inactive drive level for the power-gate pin (logical opposite of active).
#define TDS_POWER_INACTIVE_LEVEL  ((TDS_POWER_ACTIVE_LEVEL) == HIGH ? LOW : HIGH)

// ============================================
// INITIALISATION
// ============================================

void tdsSensorInit() {
  // Configure the power gate FIRST and force the probe OFF, so we never leave
  // the MOSFET in an indeterminate state at boot.
  pinMode(TDS_POWER_PIN, OUTPUT);
  tdsPowerOff();

  pinMode(TDS_SENSOR_PIN, INPUT);

  if (!tdsCalLoad()) {
    Serial.println("[TDS] No valid EEPROM calibration found — using defaults.");
    tdsCalResetToDefaults();   // saves defaults back to EEPROM
  } else {
    Serial.println("[TDS] Calibration loaded from EEPROM.");
    tdsCalPrint();
  }
}

// ============================================
// POWER GATE
// ============================================

void tdsPowerOn() {
  digitalWrite(TDS_POWER_PIN, TDS_POWER_ACTIVE_LEVEL);
  tdsPowered = true;
}

void tdsPowerOff() {
  digitalWrite(TDS_POWER_PIN, TDS_POWER_INACTIVE_LEVEL);
  tdsPowered = false;
}

void tdsPowerOnAndSettle() {
  tdsPowerOn();
  delay(TDS_POWER_SETTLE_MS);
}

bool tdsIsPowered() {
  return tdsPowered;
}

// ============================================
// READING
// ============================================

/**
 * Median-of-N helper. In-place insertion sort then return the middle element.
 * For small N insertion sort beats anything fancier and uses no heap. A median
 * rejects mains hum and the odd ADC/ESD spike far better than a mean.
 * (Identical approach to the pH module.)
 */
static float medianOfTDS(float* arr, int n) {
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

float tdsReadVoltage() {
  // Auto power management. If the probe is already powered (calibration screen
  // or test sequence pinned it ON) we just sample — no toggling, no settle
  // delay, no MOSFET thrash. If it is OFF, run a self-contained power cycle so
  // a one-shot read "just works" without external power management.
  bool selfPower = !tdsPowered;
  if (selfPower) {
    tdsPowerOnAndSettle();
  }

  float samples[TDS_SAMPLE_COUNT];
  for (int i = 0; i < TDS_SAMPLE_COUNT; i++) {
    samples[i] = analogRead(TDS_SENSOR_PIN) * (ADC_REF_VOLTAGE / ADC_MAX);
    delay(TDS_SAMPLE_DELAY);
  }
  float v = medianOfTDS(samples, TDS_SAMPLE_COUNT);

  if (selfPower) {
    tdsPowerOff();
  }
  return v;
}

float voltageToEC(float voltage, float temperature) {
  // ---- Anchors ----
  float vLow      = tdsCalData.low.voltage;
  float vHigh     = tdsCalData.high.voltage;
  float ecLowNom  = tdsCalData.low.ec;    // nominal @ TDS_TEMP_REF_C (e.g. 1413)
  float ecHighNom = tdsCalData.high.ec;   // nominal @ TDS_TEMP_REF_C (e.g. 12880)

  float dV = vHigh - vLow;

  // Degenerate-calibration guard: if both anchors share a voltage the slope is
  // undefined. Return EXACTLY 0.0 — the sketch treats a 0.0 reading as the
  // calibration-fault sentinel (BOOT_WARN on the test page).
  if (fabsf(dV) < 1e-4f) {
    return 0.0f;
  }

  // 1) Correct each anchor from its nominal (ref-temp) value to the ACTUAL
  //    conductivity it had at the calibration temperature. The probe captured
  //    vLow/vHigh while the standards were at calTempC, so the line we fit must
  //    pass through (voltage, actual-conductivity-at-calTempC) to represent the
  //    sensor's true raw response.
  float kCal      = 1.0f + TDS_TEMP_COEFF * (tdsCalData.calTempC - TDS_TEMP_REF_C);
  if (kCal < 0.1f) kCal = 0.1f;           // defensive floor against absurd cal temps
  float ecLowAct  = ecLowNom  * kCal;
  float ecHighAct = ecHighNom * kCal;

  // 2) Map raw volts -> conductivity as the probe sees it at the SAMPLE's
  //    temperature (linear two-point fit through the corrected anchors).
  float slope      = (ecHighAct - ecLowAct) / dV;
  float ecApparent = ecLowAct + slope * (voltage - vLow);

  // 3) Normalise that back to TDS_TEMP_REF_C using the SAMPLE temperature.
  //    Conductivity rises ~2 %/degC, so EC(T) = EC_ref * (1 + COEFF*(T - ref));
  //    divide it out to recover EC_ref.
  float kSample = 1.0f + TDS_TEMP_COEFF * (temperature - TDS_TEMP_REF_C);
  if (kSample < 0.1f) kSample = 0.1f;     // defensive floor against absurd sample temps
  float ecRef = ecApparent / kSample;

  // Conductivity cannot be negative (a below-range voltage clamps to 0, which
  // for a urine test correctly reads as "no/invalid sample").
  return fmaxf(ecRef, 0.0f);
}

float ecToTDS(float ec) {
  return ec * TDS_PPM_FACTOR;
}

float tdsToEC(float tds) {
  return tds / TDS_PPM_FACTOR;
}

float tdsRead(float temperature) {
  float v  = tdsReadVoltage();
  float ec = voltageToEC(v, temperature);   // CELL EC (no dilution)
  // If the calibration is degenerate, voltageToEC() returned 0.0 and this
  // propagates to exactly 0.0 ppm — the sketch's WARN sentinel.
  return ecToTDS(ec);
}

float tdsReadECSample(float temperature) {
  // CELL EC scaled up to the neat sample's conductivity by the fixed dilution
  // factor. With TDS_DILUTION_FACTOR == 1.0 this equals the cell EC.
  return voltageToEC(tdsReadVoltage(), temperature) * TDS_DILUTION_FACTOR;
}

// ============================================
// SPECIFIC GRAVITY
// ============================================

float ecToSG(float ecSample) {
  // Linear model on the NEAT-sample conductivity: SG = offset + slope * EC.
  // Slope/offset live in EEPROM and MUST be fitted against a refractometer
  // (see header). A single end-to-end slope absorbs cell constant, dilution
  // factor, and conductivity's blindness to non-ionic urea.
  return tdsCalData.sgOffset + tdsCalData.sgSlope * ecSample;
}

float tdsToSG(float tds) {
  // `tds` is a CELL ppm reading (as from tdsRead). Convert it back to cell EC,
  // scale to neat EC by the dilution factor, then map to SG. Keeping the
  // dilution here means tdsToSG(tdsRead(t)) == tdsReadSG(t).
  float ecCell   = tdsToEC(tds);
  float ecSample = ecCell * TDS_DILUTION_FACTOR;
  return ecToSG(ecSample);
}

float tdsReadSG(float temperature) {
  return ecToSG(tdsReadECSample(temperature));
}

void tdsSetSGCalibration(float slope, float offset) {
  tdsCalData.sgSlope  = slope;
  tdsCalData.sgOffset = offset;
  tdsCalData.magic    = TDS_EEPROM_MAGIC;
  EEPROM.put(TDS_EEPROM_ADDR, tdsCalData);
  Serial.print("[TDS] SG calibration updated: offset ");
  Serial.print(offset, 4);
  Serial.print(", slope ");
  Serial.print(slope, 9);
  Serial.println(" /(uS/cm). Saved to EEPROM.");
}

// ============================================
// CALIBRATION — STATE MACHINE
// ============================================

void tdsCalBegin() {
  // Keep the existing (loaded/last-saved) calibration in place until each point
  // is actually overwritten, so the live EC estimate stays sensible while the
  // user works through the steps.
  tdsCalStep = TDS_CAL_LOW;
  Serial.println("[TDS] Calibration started. Place probe in 1413 uS/cm standard.");
}

/**
 * Sample the probe over TDS_CAL_STABILITY_WINDOW_MS and report the median
 * voltage and the peak-to-peak spread across the window. A large spread means
 * the probe has not equilibrated, so the caller refuses to capture.
 *
 * During calibration the probe is pinned ON by the calibration screen, so each
 * tdsReadVoltage() below just samples (no per-call power cycling).
 */
static void measureStabilityTDS(float& median, float& spread) {
  const int N = TDS_CAL_STABILITY_WINDOW_MS / TDS_CAL_STABILITY_SAMPLE_MS;
  float buf[N];
  for (int i = 0; i < N; i++) {
    // Each sample is itself a median of TDS_SAMPLE_COUNT raw reads: robust
    // filtering at two levels — in-sample noise rejection here, and
    // across-sample equilibration detection below.
    buf[i] = tdsReadVoltage();
    if (i < N - 1) delay(TDS_CAL_STABILITY_SAMPLE_MS);
  }

  float vMin = buf[0], vMax = buf[0];
  for (int i = 1; i < N; i++) {
    if (buf[i] < vMin) vMin = buf[i];
    if (buf[i] > vMax) vMax = buf[i];
  }
  spread = vMax - vMin;

  median = medianOfTDS(buf, N);
}

bool tdsCalCapture(float standardTempC) {
  if (tdsCalStep != TDS_CAL_LOW && tdsCalStep != TDS_CAL_HIGH) {
    if (tdsCalStep == TDS_CAL_DONE) {
      Serial.println("[TDS] Already done — call tdsCalSave() or tdsCalBegin() to restart.");
    } else {
      Serial.println("[TDS] tdsCalCapture() called outside calibration sequence.");
    }
    return false;
  }

  float v, spread;
  measureStabilityTDS(v, spread);

  Serial.print("[TDS] Stability: spread = ");
  Serial.print(spread * 1000.0f, 1);
  Serial.println(" mV over window.");

  if (spread > TDS_CAL_STABILITY_MAX_SPREAD) {
    Serial.print("[TDS] REJECTED: probe not stable (need <");
    Serial.print(TDS_CAL_STABILITY_MAX_SPREAD * 1000.0f, 0);
    Serial.println(" mV). Wait for equilibration and retry.");
    return false;
  }

  // Record the temperature the standards are at, so voltageToEC() can correct
  // the (nominal, ref-temp) anchors to their true conductivity at this temp.
  // Both points are assumed captured at the same temperature (same session,
  // back-to-back); the value from the most recent capture is the one stored.
  tdsCalData.calTempC = standardTempC;

  // Store the RAW median voltage and the standard's NOMINAL conductivity. The
  // temperature correction is applied at read time from calTempC, not baked
  // into the stored ec (so the readout stays the familiar 1413 / 12880).
  switch (tdsCalStep) {
    case TDS_CAL_LOW:
      tdsCalData.low.voltage = v;
      tdsCalData.low.ec      = TDS_CAL_EC_LOW;
      Serial.print("[TDS] 1413 uS/cm captured. Voltage = ");
      Serial.print(v, 4);
      Serial.print(" V @ ");
      Serial.print(standardTempC, 1);
      Serial.println(" C");
      Serial.println("[TDS] Place probe in 12.88 mS/cm standard.");
      tdsCalStep = TDS_CAL_HIGH;
      return true;

    case TDS_CAL_HIGH:
      tdsCalData.high.voltage = v;
      tdsCalData.high.ec      = TDS_CAL_EC_HIGH;
      Serial.print("[TDS] 12.88 mS/cm captured. Voltage = ");
      Serial.print(v, 4);
      Serial.print(" V @ ");
      Serial.print(standardTempC, 1);
      Serial.println(" C");
      Serial.println("[TDS] All points captured. Call tdsCalSave() to store.");
      tdsCalStep = TDS_CAL_DONE;
      return true;

    default:
      return false;   // unreachable, silences warning
  }
}

void tdsCalSave() {
  if (tdsCalStep != TDS_CAL_DONE) {
    Serial.println("[TDS] Cannot save — calibration not complete.");
    return;
  }

  // sgSlope / sgOffset are already present in tdsCalData (from load or defaults)
  // and are untouched by the EC calibration above, so this save preserves the
  // specific-gravity model.
  tdsCalData.magic = TDS_EEPROM_MAGIC;
  EEPROM.put(TDS_EEPROM_ADDR, tdsCalData);
  tdsCalStep = TDS_CAL_IDLE;

  Serial.println("[TDS] Calibration saved to EEPROM.");
  tdsCalPrint();
}

void tdsCalCancel() {
  tdsCalLoad();   // restore last-saved data (init guarantees it is valid)
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
  tdsCalData.magic        = TDS_EEPROM_MAGIC;

  tdsCalData.low.ec       = TDS_CAL_EC_LOW;       // 1413 uS/cm
  tdsCalData.low.voltage  = TDS_DEFAULT_V_LOW;    // rough pre-cal estimate

  tdsCalData.high.ec      = TDS_CAL_EC_HIGH;      // 12880 uS/cm
  tdsCalData.high.voltage = TDS_DEFAULT_V_HIGH;   // rough pre-cal estimate

  tdsCalData.sgSlope      = TDS_SG_SLOPE_DEFAULT; // PLACEHOLDER — fit me
  tdsCalData.sgOffset     = TDS_SG_OFFSET_DEFAULT;

  tdsCalData.calTempC     = TDS_TEMP_REF_C;       // assume ref temp until recalibrated

  EEPROM.put(TDS_EEPROM_ADDR, tdsCalData);
  Serial.println("[TDS] Default calibration applied and saved to EEPROM.");
}

const char* tdsCalStepLabel() {
  switch (tdsCalStep) {
    case TDS_CAL_IDLE: return "Idle";
    case TDS_CAL_LOW:  return "Put in 1413 uS/cm";
    case TDS_CAL_HIGH: return "Put in 12.88 mS/cm";
    case TDS_CAL_DONE: return "Press SELECT to save";
    default:           return "Unknown";
  }
}

void tdsCalPrint() {
  Serial.println("[TDS] --- Calibration Data ---");
  Serial.print("  Low  | ");  Serial.print(tdsCalData.low.ec, 0);
  Serial.print(" uS/cm @ ");  Serial.print(tdsCalData.low.voltage, 4);
  Serial.println(" V");
  Serial.print("  High | ");  Serial.print(tdsCalData.high.ec, 0);
  Serial.print(" uS/cm @ ");  Serial.print(tdsCalData.high.voltage, 4);
  Serial.println(" V");
  Serial.print("  Cal T| ");  Serial.print(tdsCalData.calTempC, 1);
  Serial.println(" C");
  Serial.print("  SG   | offset "); Serial.print(tdsCalData.sgOffset, 4);
  Serial.print(", slope ");          Serial.print(tdsCalData.sgSlope, 9);
  Serial.println(" /(uS/cm)");
  Serial.print("  Dil  | x");        Serial.println(TDS_DILUTION_FACTOR, 2);
  Serial.println("[TDS] ----------------------------");
}
#ifndef TDS_SENSOR_H
#define TDS_SENSOR_H

#include <Arduino.h>
#include <EEPROM.h>

// ============================================================================
// CONDUCTIVITY / TDS / SPECIFIC-GRAVITY SENSOR
//   DFRobot Gravity: Analog Electrical Conductivity Sensor / Meter Kit V2, K=1
//   (SKU: DFR0300)
// ----------------------------------------------------------------------------
// Signal path:   probe -> (analog signal isolator, if fitted) -> A1
// Power gate:    D2 -> high-side switch
//                  D2 -> NPN (S8050 / "D331") base
//                  NPN collector pulls P-MOSFET (NDP6020P) gate LOW
//                  P-MOSFET source(+5V) -> drain feeds probe VCC
//                So the gate is ACTIVE-HIGH: D2 HIGH energises the probe.
//
// What this module produces, in order:
//   raw ADC volts
//      -> CELL EC (uS/cm, normalised to TDS_TEMP_REF_C)     <- what the probe sees
//      -> SAMPLE EC (uS/cm) = CELL EC * TDS_DILUTION_FACTOR  <- the neat urine
//      -> TDS (ppm)          (NaCl-equivalent, for the app's tds_ppm field)
//      -> Specific gravity (SG)  (from SAMPLE EC, via the fitted SG model)
//
// EC is the physically-measured quantity. TDS and SG are DERIVED from it.
// Calibration is performed in EC space against the two K=1 reference solutions
// (1413 uS/cm and 12.88 mS/cm), exactly as DFRobot specifies for this sensor,
// so the numbers line up 1:1 with the bottle labels.
//
// ----------------------------------------------------------------------------
// WHY A K=1 SENSOR NEEDS DILUTION FOR URINE  (read this once):
//   A K=1 cell is linear and trustworthy only up to ~20 mS/cm. Real/synthetic
//   urine runs from ~3 mS/cm (very dilute, SG<1.005) to ~40 mS/cm (very
//   concentrated, SG>1.035). The top half of the SG scale therefore SATURATES a
//   K=1 cell and reads low. The fix is to dilute every sample by a FIXED, known
//   factor so even the most concentrated sample lands inside the cell's linear
//   band, then carry that factor through to SG. See TDS_DILUTION_FACTOR below
//   and the lab note shipped with this module.
//
// ----------------------------------------------------------------------------
// ADC CONTRACT (matches pHSensor): the ADC reference (5.0 V) and resolution
// (10-bit / 1023) MUST stay identical to every other analog module. The sketch
// never calls analogReadResolution(), so the ADC is in its 10-bit default;
// changing it here would silently corrupt pH AND temperature readings too.
// ============================================================================


// ============================================
// PIN CONFIGURATION
// ============================================

// Analog input carrying the conductivity signal.
#define TDS_SENSOR_PIN   A1

// High-side power-gate control pin (D2 HIGH -> probe powered, see header).
// If your transistor wiring inverts this (probe powers when D2 is LOW), flip
// the single line below to LOW — nothing else needs to change.
#define TDS_POWER_PIN            2
#define TDS_POWER_ACTIVE_LEVEL   HIGH

// Time the probe needs to settle after power-up before a reading is
// trustworthy. The DFR0300 is a continuous analog circuit, but the probe's
// electrode double-layer and the supply inrush still need to stabilise;
// sampling early gives a value that is still creeping. Generous on purpose — a
// test runs this once, not in a hot loop. Raise it if the first reading after
// power-on still looks like it is climbing.
#define TDS_POWER_SETTLE_MS    2000

// ============================================
// CALIBRATION REFERENCE SOLUTIONS  (DFR0300 K=1 standards)
// ============================================

// Certified conductivity of the two K=1 standards, in uS/cm.
//   Low  = 1413 uS/cm  (0.01 mol/L KCl)
//   High = 12880 uS/cm (= 12.88 mS/cm, 0.1 mol/L KCl)
#define TDS_CAL_EC_LOW     1413.0f
#define TDS_CAL_EC_HIGH    12880.0f    // 12.88 mS/cm

// >>> READ THIS — it is the single most important number to get right. <<<
// The reference temperature at which the two standard values above are
// CERTIFIED, and the temperature every reading is normalised back to.
//
// 1413 uS/cm and 12.88 mS/cm are, in virtually every catalogue (DFRobot, Hanna,
// Atlas, ...) and in DFRobot's own DFR0300 library, the values AT 25 C. That is
// why the default here is 25.0 — NOT the 20.0 you might assume.
//
//   * If your bottles read "1413 uS/cm @ 25 C" / "12.88 mS/cm @ 25 C"  -> leave 25.0
//   * If your bottles genuinely read "@ 20 C"                          -> change to 20.0
//
// Getting this wrong shifts BOTH anchors by ~2 %/degC of the gap, i.e. ~10 % at
// the 5 degC difference between 20 C and 25 C. Confirm against the label.
#define TDS_TEMP_REF_C     25.0f

// Linear temperature coefficient of conductivity (per degC). ~2 %/degC is the
// near-universal value for dilute aqueous ionic solutions, including KCl
// standards and urine electrolytes (DFRobot's library uses 0.0185; the KCl
// 20<->25 C step is ~0.021; 0.02 sits cleanly between). Used to normalise a
// reading at the sample temperature back to TDS_TEMP_REF_C, AND to correct the
// standards to their actual conductivity at the calibration temperature.
#define TDS_TEMP_COEFF     0.02f

// ============================================
// SAMPLE DILUTION
// ============================================
//
// Multiply the measured CELL conductivity by this to recover the neat sample's
// conductivity. It is the total dilution ratio:
//
//     TDS_DILUTION_FACTOR = (volume_urine + volume_water) / volume_urine
//
//   * No dilution (probe straight into neat urine)        -> 1.0   (default)
//   * 1 part urine : 3 parts deionised water (recommended)-> 4.0
//   * 1 part urine : 4 parts deionised water              -> 5.0
//
// Keep it at 1.0 until you actually start diluting — at 1.0 this module behaves
// exactly like a direct reader and changes nothing downstream. When you adopt
// the dilution workflow, set it to the ratio you pour. The specific-gravity
// model (below) is defined on the *neat* conductivity, so the same SG slope
// stays valid no matter which dilution factor you choose later.
//
// NOTE on linearity: conductivity is not perfectly linear with dilution (ions
// interact less when spread out), so neat = measured * factor slightly
// OVER-estimates a heavily-concentrated sample. This is harmless here because
// the SG model is fitted against this same pipeline — any fixed bias is
// absorbed by the fit, exactly like the rest of this firmware treats the KNN
// chain. Just keep the factor FIXED once you have fitted SG.
#define TDS_DILUTION_FACTOR   2.0f

// ============================================
// TDS (ppm) DERIVATION
// ============================================
//
// EC (uS/cm) -> TDS (ppm). 0.5 is the NaCl-equivalent factor certified
// reference solutions are labelled with, and matches the rest of the firmware.
// TDS is reported for continuity (the app's "tds_ppm" field); SG does NOT go
// through ppm.
#define TDS_PPM_FACTOR     0.5f

// ============================================
// SAMPLING / FILTERING
// ============================================

// A median of N reads rejects mains hum and the odd ADC/ESD spike far better
// than a mean (one outlier can drag a mean by tens of mV; the median ignores
// it). Same philosophy as the pH module.
#define TDS_SAMPLE_COUNT   15
#define TDS_SAMPLE_DELAY   5     // ms between raw samples

// ---- Calibration stability gate ----
// On capture, the probe is watched over a short window and the point is only
// accepted once it has stopped moving. Prevents locking in a calibration point
// while the reading is still settling — a classic source of bad calibrations.
#define TDS_CAL_STABILITY_WINDOW_MS   2000   // total watch window (ms)
#define TDS_CAL_STABILITY_SAMPLE_MS    100   // gap between window samples (ms)
#define TDS_CAL_STABILITY_MAX_SPREAD   0.015f // max peak-to-peak (V) to accept

// ============================================
// SPECIFIC GRAVITY MODEL
// ============================================
//
//   SG = sgOffset + sgSlope * EC_sample      (EC_sample = neat uS/cm @ ref temp)
//
// Urine SG rises ~linearly with dissolved-solids concentration over the
// clinical band, and EC is ~linear in ionic concentration, so a straight line
// is the right shape. The SLOPE is sample- and setup-specific and MUST be fitted
// against a refractometer (the clinical gold standard for urine SG): the
// project dataset gives target SG per class but contains NO conductivity column,
// so there is nothing to regress against until you measure it yourself. A single
// end-to-end slope cleanly absorbs every fixed effect at once — cell constant,
// the dilution factor, and conductivity's blindness to non-ionic urea.
//
// The defaults below are a NON-FUNCTIONAL PLACEHOLDER, chosen only so the chain
// produces SG-shaped numbers (~1.00–1.04) before you calibrate. SG is meaningless
// until you replace sgSlope/sgOffset with refractometer-fitted values, either by
// editing the defaults or via tdsSetSGCalibration(). See the shipped lab note.
#define TDS_SG_SLOPE_DEFAULT    1.069461526e-06f
#define TDS_SG_OFFSET_DEFAULT   0.9968f
// ============================================
// EEPROM STORAGE
// ============================================
//
// Device EEPROM map:
//   0x00  pH calibration   (PH_EEPROM_ADDR)
//   0x20  TDS calibration   <-- here  (must stay < 0x40)
//   0x40  BLE settings      (BLE_EEPROM_ADDR)
#define TDS_EEPROM_ADDR    0x20

// Bumped (0x7D -> 0x7E) because the calibration semantics changed with the move
// to the K=1 sensor: the high anchor is now 12880 uS/cm (was 1413), the struct
// carries a calibration temperature, and the SG slope is now per-uS/cm (was
// per-ppm). The bump guarantees any stale pre-K=1 blob is rejected and the user
// is forced to recalibrate with the new standards.
#define TDS_EEPROM_MAGIC   0x7F

// Safe pre-calibration defaults (raw volts at the two standards), derived from
// the DFR0300 transfer function (~V/164 mV per mS/cm, K~1). Only need to be
// roughly plausible so readings are not nonsense before a real calibration.
#define TDS_DEFAULT_V_LOW    0.23f   // ~ raw volts in 1413 uS/cm
#define TDS_DEFAULT_V_HIGH   2.11f   // ~ raw volts in 12.88 mS/cm

// ============================================
// DATA STRUCTURES
// ============================================

/**
 * One calibration point: the raw probe voltage measured in a standard, plus
 * that standard's NOMINAL conductivity at TDS_TEMP_REF_C (kept nominal so the
 * on-screen / Serial readout shows the familiar 1413 and 12880, regardless of
 * the temperature the calibration was actually performed at).
 */
struct TDSCalPoint {
  float voltage;   // raw volts captured in this standard (at calTempC)
  float ec;        // nominal conductivity of the standard (uS/cm @ TDS_TEMP_REF_C)
};

/**
 * Complete calibration, persisted to EEPROM. Holds the two EC anchors, the
 * temperature the standards were at during capture (so anchors can be corrected
 * to the sample temperature precisely), and the SG model. SG params live here
 * so they survive power cycles and are NOT wiped by an EC re-calibration.
 *
 * Layout is kept <= 32 bytes so it cannot run into the BLE block at 0x40
 * (enforced by the static_assert below).
 */
struct TDSCalibration {
  uint8_t     magic;     // validity marker
  TDSCalPoint low;       // 1413 uS/cm point
  TDSCalPoint high;      // 12880 uS/cm point
  float       sgSlope;   // SG per (uS/cm of neat sample)  (refractometer-fitted)
  float       sgOffset;  // SG baseline (pure water)
  float       calTempC;  // temperature of the standards during capture (degC)
};

// Guarantee the struct stays inside its 0x20..0x3F EEPROM window (BLE = 0x40).
// If a future field overflows this, compilation FAILS loudly instead of
// silently corrupting BLE settings.
static_assert(sizeof(TDSCalibration) <= 0x20,
              "TDSCalibration must fit in EEPROM 0x20..0x3F (BLE starts at 0x40)");

// ============================================
// CALIBRATION STATE MACHINE
// ============================================

/**
 * Drives the interactive two-point calibration screen.
 * IDLE -(begin)-> LOW -(capture)-> HIGH -(capture)-> DONE -(save)-> IDLE
 */
enum TDSCalStep {
  TDS_CAL_IDLE = 0,
  TDS_CAL_LOW,    // waiting for the 1413 uS/cm reading
  TDS_CAL_HIGH,   // waiting for the 12.88 mS/cm reading
  TDS_CAL_DONE    // both captured; ready to save
};

extern TDSCalStep     tdsCalStep;   // current calibration step
extern TDSCalibration tdsCalData;   // working / loaded calibration

// ============================================
// FUNCTION DECLARATIONS
// ============================================

/**
 * Initialise the module: configure the power-gate pin (probe left OFF) and the
 * analog input, then load calibration from EEPROM (or apply + save defaults).
 */
void tdsSensorInit();

// ---- Power gate ----
void tdsPowerOn();           // energise probe (high-side gate ON); records state
void tdsPowerOff();          // de-energise probe (gate OFF); probe dark between tests
void tdsPowerOnAndSettle();  // power ON and block for the full settle window
bool tdsIsPowered();         // true while the probe is currently powered

// ---- Reading ----

/**
 * Median-filtered RAW probe voltage (volts), uncompensated.
 * Power handling is automatic: if the probe is already powered (calibration
 * screen / test sequence pinned it ON) this just samples; if it is OFF it runs a
 * self-contained ON -> settle -> read -> OFF cycle so one-shot reads "just work".
 */
float tdsReadVoltage();

/**
 * Convert a raw probe voltage to CELL conductivity (uS/cm), normalised to
 * TDS_TEMP_REF_C. This is the conductivity of whatever is physically in the cup
 * (i.e. the diluted liquid, if you are diluting) — dilution is NOT applied here.
 *
 * Math:
 *   1. Correct each stored anchor to its true conductivity at the calibration
 *      temperature (calTempC): ec_actual = ec_nominal * (1 + COEFF*(calTempC-ref)).
 *   2. Fit the line raw-volts -> actual-conductivity through the two anchors and
 *      evaluate it at `voltage` -> the sample's conductivity at ITS temperature.
 *   3. Normalise that to TDS_TEMP_REF_C using the sample temperature.
 *
 * Returns exactly 0.0 if the calibration is degenerate (both anchors share a
 * voltage, slope undefined) — callers treat 0.0 as a fault sentinel.
 *
 * @param voltage     raw probe voltage (volts)
 * @param temperature SAMPLE temperature in degC (default = ref => no comp.)
 */
float voltageToEC(float voltage, float temperature = TDS_TEMP_REF_C);

/** CELL EC (uS/cm) -> TDS (ppm). */
float ecToTDS(float ec);

/** TDS (ppm) -> CELL EC (uS/cm). Exact inverse of ecToTDS(). */
float tdsToEC(float tds);

/**
 * Read the probe and return CELL TDS in ppm (temperature-compensated).
 * Returns exactly 0.0 only when the calibration is degenerate.
 */
float tdsRead(float temperature = TDS_TEMP_REF_C);

/**
 * Read the probe and return the NEAT SAMPLE conductivity (uS/cm @ ref temp),
 * i.e. CELL EC * TDS_DILUTION_FACTOR. This is the conductivity attributable to
 * the original undiluted urine. With TDS_DILUTION_FACTOR == 1.0 this equals the
 * cell EC.
 */
float tdsReadECSample(float temperature = TDS_TEMP_REF_C);

// ---- Specific gravity ----

/** Neat-sample EC (uS/cm) -> specific gravity, using the stored SG model. */
float ecToSG(float ecSample);

/**
 * CELL TDS (ppm, e.g. as returned by tdsRead()) -> specific gravity.
 * The dilution factor is applied internally (ppm -> cell EC -> neat EC -> SG),
 * so this is consistent with ecToSG() and tdsReadSG().
 */
float tdsToSG(float tds);

/** Read the probe and return estimated urine specific gravity. */
float tdsReadSG(float temperature = TDS_TEMP_REF_C);

/**
 * Set and persist the SG model parameters (e.g. after fitting against a
 * refractometer). Slope is per (uS/cm of neat sample). Writes to EEPROM.
 */
void tdsSetSGCalibration(float slope, float offset);

// ---- Calibration helpers (used by the on-device calibration screen) ----

/** Begin a new two-point calibration (step -> LOW). Keeps existing data until
 *  each point is overwritten, so the live estimate stays sensible meanwhile. */
void tdsCalBegin();

/**
 * Capture the current reading for the active step, with a stability check: the
 * reading is watched for TDS_CAL_STABILITY_WINDOW_MS and accepted only if its
 * peak-to-peak spread is under TDS_CAL_STABILITY_MAX_SPREAD volts. On success
 * the point is stored and the step advances.
 *
 * Pass the MEASURED standard temperature for best accuracy (the standards are
 * almost never at exactly TDS_TEMP_REF_C). The default keeps the old no-arg
 * call sites working, but then assumes the standard is exactly at the reference
 * temperature — only correct if you have equilibrated it there.
 *
 *   Recommended:  tdsCalCapture(pHReadTemperature());
 *
 * @param standardTempC temperature of the standard solution, degC.
 * @return true if a stable point was captured, false if rejected (retry).
 */
bool tdsCalCapture(float standardTempC = TDS_TEMP_REF_C);

/** Persist the completed calibration to EEPROM (call when step == DONE). */
void tdsCalSave();

/** Abandon an in-progress calibration and reload the last saved data. */
void tdsCalCancel();

/** Load calibration from EEPROM into tdsCalData. @return true if valid. */
bool tdsCalLoad();

/** Reset calibration (EC anchors + SG model + cal temp) to defaults and save. */
void tdsCalResetToDefaults();

/** Short, OLED-friendly label for the current calibration step. */
const char* tdsCalStepLabel();

/** Print the current calibration to Serial (debugging). */
void tdsCalPrint();

#endif // TDS_SENSOR_H
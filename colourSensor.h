#ifndef COLOR_SENSOR_H
#define COLOR_SENSOR_H

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <DFRobot_AS7341.h>   // DFRobot Gravity: AS7341 11-channel visible-light sensor

// ============================================================================
// SENSOR: DFRobot Gravity AS7341 (replaces the TCS34725)
// ============================================================================
//
// The AS7341 is an 11-channel spectral sensor. It exposes 8 narrow-band
// visible photodiodes (F1..F8), a broadband CLEAR channel, a near-IR (NIR)
// channel and a flicker channel. The 8 bands sit at roughly:
//
//   F1 = 415 nm (violet)      F5 = 555 nm (green)
//   F2 = 445 nm (blue)        F6 = 590 nm (yellow/amber)
//   F3 = 480 nm (blue)        F7 = 630 nm (orange/red)
//   F4 = 515 nm (cyan/green)  F8 = 680 nm (red)
//
// Because the ADC has only 6 physical channels, the 8 bands are read in TWO
// SMUX banks (F1-F4+Clear+NIR, then F5-F8+Clear+NIR). The DFRobot_AS7341
// library handles the SMUX configuration for us, so we never touch the raw
// SMUX sequence here.
//
// I2C address is fixed at 0x39 (handled inside the library).
//
// -----------------------------------------------------------------------------
// WHY THE PUBLIC INTERFACE IS UNCHANGED
// -----------------------------------------------------------------------------
// The rest of the firmware (ArduinoUrinalysis.ino), the BLE JSON schema, and
// the Urisis app's Fuzzy-KNN hydration classifier all consume an 8-bit R/G/B
// triplet plus lux/CCT. To swap the sensor WITHOUT touching any of that, the
// 8 spectral bands are collapsed into the existing RawRGBC {r,g,b,c} quad at
// read time (see AS7341_W_* grouping weights below). Everything downstream —
// the two-point dark/white liquid-blank calibration, the colour-correction
// matrix, the water-tint, gamma, and the HSV classification — is byte-for-byte
// the same code path it was on the TCS34725.
//
// The raw 8-band spectrum is ALSO carried in RawRGBC (f1..f8, nir) so it is
// available to the serial report and any future spectral feature without
// disturbing the RGB path.
//
// >>> CALIBRATION FLAG (read before trusting coloured-sample hue) <<<
//   The grouping weights below determine how spectral bands map to R/G/B,
//   which sets the OUTPUT HUE of coloured samples — and hue is the Urisis
//   classifier's primary feature (Flag A fires for hue <20 deg / >65 deg).
//   The defaults are sensible equal-weight groupings but were NOT fitted on
//   this rig. The CLEAR/WATER baseline is preserved automatically (a water
//   sample equals the white reference, so it normalises to 1,1,1 and the
//   existing water-tint still lands it at hue ~56 deg regardless of mapping).
//   Only COLOURED samples (yellow/amber/red) may shift. After the hardware
//   swap, capture a few known samples via the serial "Measurement Report"
//   and re-tune AS7341_W_* (or the app thresholds) if hues drift.
// ============================================================================


// ============================================
// AS7341 GAIN (AGAIN, register CFG1)
// setAGAIN(value): 0..10 -> 0.5x,1x,2x,4x,8x,16x,32x,64x,128x,256x,512x
// ============================================

#define AS7341_GAIN_0_5X   0
#define AS7341_GAIN_1X     1
#define AS7341_GAIN_2X     2
#define AS7341_GAIN_4X     3
#define AS7341_GAIN_8X     4
#define AS7341_GAIN_16X    5
#define AS7341_GAIN_32X    6
#define AS7341_GAIN_64X    7
#define AS7341_GAIN_128X   8
#define AS7341_GAIN_256X   9
#define AS7341_GAIN_512X  10

// Default gain. 64x is the DFRobot example default and a good starting point
// for a dim-LED sealed transmission box. Raise toward 256x if counts are low,
// lower if the white reference saturates.
#define AS7341_DEFAULT_GAIN  AS7341_GAIN_32X

// ============================================
// AS7341 INTEGRATION TIME (ATIME + ASTEP)
//
//   t_int (ms) = (ATIME + 1) * (ASTEP + 1) * 2.78 us
//   max ADC count = min(65535, (ATIME + 1) * (ASTEP + 1))
//
// Default: ATIME=29, ASTEP=599 -> 30 * 600 * 2.78us = 50.0 ms,
//          max count = 18000 (comfortable headroom below 16-bit).
// ============================================

#define AS7341_DEFAULT_ATIME   29       // 0..255
#define AS7341_DEFAULT_ASTEP   599      // 0..65534

// AS7341 ADC step period (datasheet): 2.78 us per (ASTEP+1) unit.
#define AS7341_ASTEP_PERIOD_US  2.78f

// ============================================
// AS7341 STATUS / SATURATION  (raw-register access)
// ============================================
//
// The DFRobot library does not expose the spectral saturation flags, so they
// are read directly. STATUS_2 (0xA3) sits above 0x80 and is reachable without
// bank switching (the library reads it the same way for the AVALID bit). The
// I2C address is fixed at 0x39. Bit positions are per the ams AS7341 datasheet
// (AVALID bit 6 cross-checked against the DFRobot and Adafruit libraries):
//
//   bit 6  AVALID        — spectral data valid
//   bit 4  ASAT_DIGITAL  — a channel hit the max ADC count (digital saturation)
//   bit 3  ASAT_ANALOG   — input intensity too high (analog saturation)
//
// A reading with either ASAT bit set is unreliable: at least one band railed,
// so its value (and any hue/ratio derived from it) is wrong. Lower the gain or
// the integration time, or dim the illuminator, and re-read.
#define AS7341_I2C_ADDR        0x39
#define AS7341_REG_STATUS_2    0xA3
#define AS7341_STATUS2_ASAT_ANALOG   (1 << 3)
#define AS7341_STATUS2_ASAT_DIGITAL  (1 << 4)
#define AS7341_STATUS2_AVALID        (1 << 6)

// Channel data registers. CH0_DATA_L..CH5_DATA_H occupy 0x95..0xA0 (six 16-bit
// channels, little-endian L then H). The AS7341 auto-increments the register
// pointer during a multi-byte read, so all 12 bytes come back in one burst.
// Like STATUS_2, these are >= 0x80 and need no bank switch. Read directly (see
// readBankRaw() in colourSensor.cpp) to bypass the DFRobot library's
// getChannelData(), which sleeps delay(50) PER CHANNEL — 600 ms of dead time
// per full F1-F8 read. Reading the registers ourselves removes that entirely.
#define AS7341_REG_CH0_DATA_L  0x95

// Maximum time to wait for AVALID (spectral data valid) after startMeasure().
// The DFRobot library's startMeasure() is non-blocking — it starts the
// integration and returns immediately. We must poll STATUS_2.AVALID ourselves
// before calling readSpectralData*(). Without this timeout the MCU hangs
// forever if the sensor stalls (I²C glitch, SMUX confusion, power issue).
//
// Upper bound: integration time at defaults (ATIME=29, ASTEP=599) is ~50 ms
// per bank. 500 ms gives 10× headroom — enough to survive any realistic I²C
// retry delay while still unblocking within one loop tick if the sensor dies.
#define AS7341_AVALID_TIMEOUT_MS  500

// ============================================
// SPECTRAL -> RGB GROUPING WEIGHTS
// ============================================
//
// Each output channel is the WEIGHTED AVERAGE of its member bands (averaged,
// not summed, so the result stays within a single channel's 16-bit range and
// cannot overflow RawRGBC). Set a weight to 0 to drop a band from a group.
//
//   BLUE   <- F1 (415), F2 (445), F3 (480)
//   GREEN  <- F4 (515), F5 (555)
//   RED    <- F6 (590), F7 (630), F8 (680)
//
// F6 (590 nm, yellow/amber) is placed in RED by default, matching how the
// TCS34725's broad red filter previously read yellows as partly red and
// keeping urine ambers warm. To split yellow toward green instead, lower
// AS7341_W_R_F6 and raise a green term. Re-tune against known samples.
#ifndef AS7341_W_B_F1
  #define AS7341_W_B_F1  1.0f   // 415 nm violet
#endif
#ifndef AS7341_W_B_F2
  #define AS7341_W_B_F2  1.0f   // 445 nm blue
#endif
#ifndef AS7341_W_B_F3
  #define AS7341_W_B_F3  1.0f   // 480 nm blue
#endif
#ifndef AS7341_W_G_F4
  #define AS7341_W_G_F4  1.0f   // 515 nm cyan/green
#endif
#ifndef AS7341_W_G_F5
  #define AS7341_W_G_F5  1.0f   // 555 nm green
#endif
#ifndef AS7341_W_R_F6
  #define AS7341_W_R_F6  1.0f   // 590 nm yellow/amber
#endif
#ifndef AS7341_W_R_F7
  #define AS7341_W_R_F7  1.0f   // 630 nm orange/red
#endif
#ifndef AS7341_W_R_F8
  #define AS7341_W_R_F8  1.0f   // 680 nm red
#endif

// ============================================
// TWO ILLUMINATORS  (external white LEDs  +  AS7341 on-board LED)
// ============================================
//
// This rig has TWO independent light sources, used by DIFFERENT sensors and
// never at the same time:
//
//   1. EXTERNAL white LEDs on D9/D10 (ILLUM_PIN / ILLUM2_PIN). These light the
//      sample for the ESP32-CAM. They are ON essentially all the time — the
//      idle/menu state and throughout every camera read and camera
//      calibration — and are switched OFF only for the brief moment the AS7341
//      integrates (their broad white light would otherwise swamp the AS7341's
//      own controlled illumination). Driven by PWM; each has its own stored
//      duty and "Light Adjust" screen; both toggle together via
//      illuminatorOn() / illuminatorOff().
//
//   2. The AS7341 ON-BOARD LED. This is the AS7341's OWN light source. It is
//      normally OFF and is FLASHED ON only while the AS7341 takes a reading or
//      captures a calibration point — with the external LEDs switched OFF at
//      the same time — so the spectral sensor sees a single, known illuminant.
//      Driven via colorOnboardLedOn() / colorOnboardLedOff().
//
// So exactly one illuminator is active per sensor:
//   AS7341 read/cal  -> external OFF, on-board ON
//   camera read/cal  -> external ON,  on-board OFF
//   idle / menus     -> external ON,  on-board OFF
//
//   ILLUM1 (D9)  -- primary external illuminator
//   ILLUM2 (D10) -- secondary external illuminator
#define ILLUM_PIN   12    // Primary illuminator (D9)
#define ILLUM2_PIN  13    // Secondary illuminator (D10)

#define ILLUM_DEFAULT_BRIGHTNESS   10
#define ILLUM2_DEFAULT_BRIGHTNESS  10

// ============================================
// ILLUMINATOR FLASH TIMING  (AS7341 vs ESP32-CAM)
// ============================================
//
// Because the two illuminators are swapped around each sensor's read (see
// above), the firmware waits COLOR_FLASH_SETTLE_MS after CHANGING the active
// illuminator before the sensor integrates/captures:
//
//   * before an AS7341 read: the external LEDs are switched OFF and the
//     on-board LED ON, then we wait this long so the AS7341 integrates under
//     the settled on-board light (matching the white reference captured the
//     same way during calibration);
//   * before a camera read: the external LEDs are switched ON, then we wait
//     this long so the frame is exposed under steady illumination.
//
// LEDs reach full output almost instantly; this small margin just guards
// against any driver/sensor settling and is negligible next to the read time.
#define COLOR_FLASH_SETTLE_MS  50

// On-board AS7341 LED drive current (1..20 -> ~4mA..42mA per the datasheet).
// This is now the AS7341's PRIMARY illuminant — flashed on for every spectral
// read/calibration — so it must be bright enough for healthy counts through
// the sample: raise it if AS7341 readings come back near zero, lower it if the
// white reference saturates. Applied by colorOnboardLedOn().
#ifndef AS7341_ONBOARD_LED_CURRENT
  #define AS7341_ONBOARD_LED_CURRENT  20
#endif

// ============================================
// SAMPLING
// ============================================

#define COLOR_SAMPLE_COUNT  5        // Readings to average per measurement
#define COLOR_SAMPLE_DELAY  10       // ms settle between averaged samples

// ============================================
// AUTOMATIC GAIN CONTROL (AGC)
// ============================================
//
// colorAutoGain() picks the highest AS7341 analogue gain that keeps the
// brightest RAW spectral channel of the CURRENT scene safely below saturation.
// It is run when capturing the WHITE (clear-water) reference during
// calibration — the brightest sample this sealed transmission box will ever
// see — so the chosen gain is stored alongside the white/dark references and
// every later test runs at that same gain with guaranteed headroom: coloured
// urine samples absorb light, so they are always dimmer than the water blank
// and cannot saturate at a gain the blank already cleared.
//
// Target window for the peak channel, expressed as a percentage of the max ADC
// count for the active ATIME/ASTEP. Below LO -> raise gain; above HI (or a
// hardware ASAT flag) -> lower gain. Because the AS7341 gain steps in 2x
// notches, a single notch can jump a dim peak straight past the window into
// saturation, so the window is deliberately wide (a notch never overshoots it).
#ifndef COLOR_AGC_TARGET_LO_PCT
  // 18%, not 25%: AGC gain steps in 2x notches, so a peak landing just under a
  // 25% floor (e.g. ~22% at 64x) gets bumped a full notch to ~44% at 128x. An
  // 18% floor accepts that 64x reading instead of over-amplifying to 128x,
  // while still keeping the white reference well clear of the noise floor.
  #define COLOR_AGC_TARGET_LO_PCT  18
#endif
#ifndef COLOR_AGC_TARGET_HI_PCT
  #define COLOR_AGC_TARGET_HI_PCT  88
#endif

// ============================================
// AMBIENT-LIGHT-LEAK CHECK
// ============================================
//
// colorCheckAmbientLeak() switches EVERY illuminant off (external white LEDs
// AND the AS7341 on-board LED), lets the sensor settle, and reads. A correctly
// sealed box is pitch black with all lights off, so the sensor should report
// only a few counts of dark current. Anything above COLOR_AMBIENT_LEAK_COUNTS
// means room light is leaking in past the lid/seal and will bias both the
// AS7341 and the ESP32-CAM — every calibration reference and every test result.
//
// It is run at every calibration capture and at the start of every test. The
// threshold is an absolute raw-count value (not a percentage) so it is easy to
// reason about against the lights-off dark-current floor; raise it if a clean,
// sealed box still trips the warning, lower it to catch fainter leaks.
#ifndef COLOR_AMBIENT_LEAK_COUNTS
  #define COLOR_AMBIENT_LEAK_COUNTS  200
#endif

// ============================================
// LUX / CCT
// ============================================
//
// IMPORTANT: the ams DN40 lux/CCT equations and the device factor 310 were
// SPECIFIC TO THE TCS34725 and do NOT apply to the AS7341. They are gone.
//
// lux here is a RELATIVE brightness proxy from the broadband CLEAR channel,
// normalised by integration time and gain so it stays (approximately)
// invariant to ATIME/ASTEP/AGAIN — change a setting and the same scene reads
// about the same number. It is NOT a calibrated photometric lux value. Scale
// it with AS7341_LUX_K if you want the displayed magnitude in a particular
// range; the value is diagnostic only and is not a classifier feature.
#ifndef AS7341_LUX_K
  #define AS7341_LUX_K  0.18f
#endif

// CCT is estimated from the mapped R/G/B via the standard sRGB->XYZ->xy chain
// and the McCamy cubic approximation. It is settings-invariant (ratio based)
// and adequate as a diagnostic, but is NOT spectrally calibrated for the
// AS7341. Diagnostic only; not a classifier feature.

// ============================================
// COLOUR-CORRECTION MATRIX (sensor RGB -> sRGB-ish)
// ============================================
//
// Applied to the white-balanced LINEAR rgb:
//
//     [ R' ]   [ RR RG RB ] [ R ]
//     [ G' ] = [ GR GG GB ] [ G ]
//     [ B' ]   [ BR BG BB ] [ B ]
//
// DEFAULT = IDENTITY. As with the TCS build, white-balancing against neutral
// clear water already cancels most sensor cast in this transmission box, so a
// matrix is not needed as a baseline. If, after the AS7341 swap, the
// per-source breakdown shows a consistent hue bias against the ESP32-CAM,
// fit a matrix here from the serial "Linear" values against known colours.
// Restore identity (diagonals 1.0, off-diagonals 0.0) to disable.
#ifndef COLOR_CCM_RR
  #define COLOR_CCM_RR  1.0f
  #define COLOR_CCM_RG  0.0f
  #define COLOR_CCM_RB  0.0f
  #define COLOR_CCM_GR  0.0f
  #define COLOR_CCM_GG  1.0f
  #define COLOR_CCM_GB  0.0f
  #define COLOR_CCM_BR  0.0f
  #define COLOR_CCM_BG  0.0f
  #define COLOR_CCM_BB  1.0f
#endif

// Output gamma applied after the matrix. 1.0 = linear (default).
#define COLOR_OUTPUT_GAMMA  1.0f

// ============================================
// WATER-CALIBRATION TINT  (unchanged rationale)
// ============================================
//
// When clear water is the white reference, white-balancing strips water's
// pale-yellow appearance and a water/clear-urine sample normalises to
// (255,255,255) -> hue 0 deg, which would (1) trigger the app's Flag A
// (abnormal colour) and (2) collapse all KNN hue memberships to 0.
//
// The fix is a small post-normalisation tint (slight G and B reduction) that
// reinstates the pale-yellow baseline so water -> hue ~56 deg (inside the
// Level-1 dilute fuzzy range 55-60 deg and safely within the 20-65 deg normal
// band). Because a water sample equals the white reference, this tint lands at
// the SAME hue regardless of the sensor or the spectral->RGB mapping, so the
// false-Flag-A-on-water fix carries over to the AS7341 unchanged.
//
// Set both to 1.0f to disable.
#ifndef COLOR_WATER_TINT_G
  #define COLOR_WATER_TINT_G  0.985f   // ~1.5% green reduction
#endif
#ifndef COLOR_WATER_TINT_B
  #define COLOR_WATER_TINT_B  0.780f   // ~22% blue reduction
#endif

// ============================================
// IR (NIR) COMPENSATION IN THE COLOUR PATH
// ============================================
//
// The AS7341 has a DEDICATED NIR channel (recorded in RawRGBC.nir and shown in
// the serial report). However, the colour-path IR compensation still uses the
// broadband (R+G+B-C)/2 ESTIMATE rather than the measured NIR. The reason is
// self-consistency: the stored white/dark references are ColorCalibrationPoint
// {r,g,b,c} only — they carry no NIR — so the sample and the references must be
// compensated by the same r/g/b/c-derived estimate for the white-balance divide
// to stay valid. (Storing per-reference NIR would let this use the true NIR; a
// possible future refinement, but it needs an EEPROM-schema bump.)
//
// As before, this is DISABLED by default: the sealed white-LED box has
// negligible IR, and the subtraction breaks the strict linearity of the
// (sample - dark) / (white - dark) balance. Enable only under IR-rich light.
//
//   0 = no IR comp in colorNormalise()  (default — correct for this rig)
//   1 = subtract the (R+G+B-C)/2 IR estimate before white balance
#ifndef COLOR_IR_COMPENSATE_RGB
  #define COLOR_IR_COMPENSATE_RGB  0
#endif

// ============================================
// EEPROM STORAGE
// ============================================
//
// Magic bumped 0xCB -> 0xA7: the colour sensor changed from TCS34725 to
// AS7341. ColorCalibration now also stores `astep`, and the white/dark
// references are captured in AS7341-MAPPED-RGB space. Any TCS34725-era
// calibration is physically meaningless for the new sensor, so bumping the
// magic forces a clean reset to defaults on first boot after the upgrade.
#define COLOR_EEPROM_ADDR   0x80
#define COLOR_EEPROM_MAGIC  0xA7


// ============================================
// DATA STRUCTURES
// ============================================

/**
 * One colorimetric reading.
 *
 * r/g/b/c are the AS7341 8-band spectrum collapsed into a TCS34725-compatible
 * RGBC quad (see AS7341_W_* weights) — this is what the whole downstream
 * pipeline consumes, exactly as before.
 *
 * f1..f8 and nir carry the underlying spectrum for the serial report and any
 * future spectral feature. They never touch EEPROM and adding them does not
 * affect any existing r/g/b/c consumer.
 */
struct RawRGBC {
  uint16_t r;
  uint16_t g;
  uint16_t b;
  uint16_t c;   // Broadband CLEAR channel (mean of the two SMUX-bank reads)

  // Underlying AS7341 spectrum (informational; not used by the RGB path).
  uint16_t f1;  // 415 nm
  uint16_t f2;  // 445 nm
  uint16_t f3;  // 480 nm
  uint16_t f4;  // 515 nm
  uint16_t f5;  // 555 nm
  uint16_t f6;  // 590 nm
  uint16_t f7;  // 630 nm
  uint16_t f8;  // 680 nm
  uint16_t nir; // near-IR

  // Hardware saturation flags for this reading (AS7341 STATUS_2 ASAT bits),
  // OR-ed across both SMUX banks. If either is set, at least one band railed
  // and the reading is unreliable. Not used by the RGB math; surfaced in the
  // report and the calibration guard.
  bool satAnalog;
  bool satDigital;
};

/**
 * Normalised [0-255] RGB derived from a raw reading.
 */
struct NormalisedRGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

/**
 * Result of a colorCheckAmbientLeak() probe (all illuminants off).
 */
struct AmbientLeak {
  uint16_t clear;   // CLEAR channel with every light off
  uint16_t peak;    // strongest of CLEAR + F1..F8 with every light off
  bool     leak;    // peak or clear exceeded COLOR_AMBIENT_LEAK_COUNTS
};

/**
 * A single white-balance calibration point, in MAPPED-RGB space.
 */
struct ColorCalibrationPoint {
  uint16_t r;
  uint16_t g;
  uint16_t b;
  uint16_t c;
};

/**
 * Full colour calibration block. Two-point white/dark liquid blank:
 *   corrected = (raw - dark) / (white - dark)
 * Saved to / loaded from EEPROM.
 */
struct ColorCalibration {
  uint8_t               magic;             // Validity marker
  ColorCalibrationPoint white;             // Clear-water reference (lights on)
  ColorCalibrationPoint dark;              // Dark reference (lights off / black liquid)
  uint8_t               atime;             // AS7341 ATIME register value
  uint16_t              astep;             // AS7341 ASTEP register value
  uint8_t               gain;              // AS7341 AGAIN value (0..10)
  uint8_t               illumBrightness;   // Primary LED (D9) PWM duty (0..255)
  uint8_t               illumBrightness2;  // Secondary LED (D10) PWM duty (0..255)
};

// ============================================
// CALIBRATION STATE MACHINE  (unchanged)
// ============================================

enum ColorCalStep {
  COLOR_CAL_IDLE = 0,
  COLOR_CAL_DARK,    // Waiting for dark (black liquid / lights-off) reading
  COLOR_CAL_WHITE,   // Waiting for white-reference (clear water) reading
  COLOR_CAL_DONE     // Both points captured; ready to save
};

extern ColorCalStep colorCalStep;
extern ColorCalibration colorCalData;

// ============================================
// FUNCTION DECLARATIONS
// ============================================

// ---- Core ----

/**
 * Recover a wedged shared I2C bus.
 *
 * The SH1106 OLED (0x3C) and the AS7341 (0x39) share the one hardware I2C bus.
 * If the MCU is reset / browns out (or a read is aborted) while a slave is
 * mid-byte, that slave keeps holding SDA LOW waiting for more clocks. With SDA
 * stuck low the master can never generate a START again, so EVERY following
 * transaction fails: the AS7341 returns garbage/zeros AND — because the OLED is
 * on the same wire — the next u8g2.sendBuffer() corrupts or hangs the display.
 *
 * This performs the standard recovery (NXP AN10216 / I2C-bus spec 3.1.16): take
 * SDA/SCL back as GPIO, pulse SCL up to 9 times (one byte + ACK) so the stuck
 * slave finishes its byte and releases SDA, synthesise a STOP, then re-attach
 * the hardware I2C peripheral with Wire.begin().
 *
 * Call it once at boot BEFORE the first OLED write, and from colorReadRaw()
 * whenever an AS7341 transaction fails. Returns true if SDA is free (high) when
 * done.
 */
bool i2cBusRecover();

/**
 * Initialise the AS7341 over I2C. Loads calibration from EEPROM (or applies
 * defaults). Initialises the white-LED illuminators and turns them on at the
 * stored brightness on exit. Returns true on success, false if the sensor is
 * not detected.
 */
bool colorSensorInit();

/**
 * Apply the active atime/astep/gain settings to the sensor. Call after
 * changing colorCalData.atime / .astep / .gain at runtime.
 */
void colorSensorApplySettings();

// ---- Reading ----

/**
 * Read one full reading: both SMUX banks (F1-F4, F5-F8), collapsed into the
 * RGBC quad and with the full spectrum populated. Blocks while the AS7341
 * integrates (the library waits for measurement complete in SPM mode).
 */
RawRGBC colorReadRaw();

/**
 * Read and average COLOR_SAMPLE_COUNT raw readings.
 */
RawRGBC colorReadRawAveraged();

/**
 * Convert a raw reading into normalised [0-255] RGB.
 *
 * Pipeline (unchanged from the TCS34725 build):
 *   1. NIR compensation (OPTIONAL; COLOR_IR_COMPENSATE_RGB, off by default).
 *   2. White/dark balance: (sample - dark) / (white - dark) per channel.
 *   3. Colour-correction matrix (COLOR_CCM_*; identity by default).
 *   4. Water-calibration tint (reinstates pale-yellow baseline).
 *   5. Output gamma (COLOR_OUTPUT_GAMMA; linear by default).
 */
NormalisedRGB colorNormalise(const RawRGBC& raw);

/**
 * Convenience: read averaged raw, apply calibration, return normalised RGB.
 */
NormalisedRGB colorRead();

// ---- Automatic gain control & ambient-leak check ----

/**
 * Auto-select the AS7341 analogue gain for the CURRENT scene so the brightest
 * raw channel lands inside the COLOR_AGC_TARGET_LO_PCT..HI_PCT window of the
 * max ADC count. Applies and stores the chosen gain in colorCalData.gain (so a
 * following colorCalSave() persists it). Intended to run while the WHITE
 * reference is lit during calibration. Returns true if the peak settled inside
 * the window, false if it hit a gain rail (the best-effort gain is still
 * applied). See the AGC notes in this header.
 */
bool colorAutoGain();

/**
 * Ambient-light-leak probe. Turns EVERY illuminant off, settles, and reads.
 * Returns the residual lights-off level and whether it exceeds
 * COLOR_AMBIENT_LEAK_COUNTS. LEAVES ALL LIGHTS OFF on return — the caller is
 * responsible for restoring whatever illumination it needs next. Run at every
 * calibration capture (via colorCalCapture) and at the start of every test.
 */
AmbientLeak colorCheckAmbientLeak();

/**
 * Estimate correlated colour temperature (CCT, Kelvin) from a raw reading.
 * AS7341 estimate via sRGB->XYZ->xy + McCamy. Diagnostic only; returns 0 if
 * the reading is degenerate (no light).
 */
uint16_t colorCalcCCT(const RawRGBC& raw);

/**
 * Estimate a RELATIVE illuminance proxy from the CLEAR channel, normalised by
 * integration time and gain (settings-invariant). NOT calibrated lux.
 * Diagnostic only; returns 0 if the clear channel is zero.
 */
float colorCalcLux(const RawRGBC& raw);

/**
 * Print a full colour report to Serial: raw spectrum + RGBC, white-balanced
 * linear RGB, normalised RGB (hex + decimal), lux proxy and CCT.
 */
void colorPrintReport(const RawRGBC& raw, const NormalisedRGB& norm);

// ---- Calibration ----

void colorCalBegin();
void colorCalCapture();
void colorCalSave();
void colorCalCancel();
bool colorCalLoad();
void colorCalResetToDefaults();
const char* colorCalStepLabel();
void colorCalPrint();

// ---- Settings helpers ----

/**
 * Set the integration time (ATIME register value). Applies immediately and
 * stores into the calibration struct. (ASTEP via colorSetAstep.)
 */
void colorSetIntegrationTime(uint8_t atime);

/**
 * Set the analogue gain (one of the AS7341_GAIN_* values, 0..10). Applies
 * immediately and stores into the calibration struct.
 */
void colorSetGain(uint8_t gain);

/**
 * Set the ASTEP register value. Applies immediately and stores into the
 * calibration struct.
 */
void colorSetAstep(uint16_t astep);

uint8_t  colorGetIntegrationTime();
uint8_t  colorGetGain();
uint16_t colorGetAstep();

// ---- External illuminator (white LEDs) ----

void illuminatorInit();
void illuminatorOn();
void illuminatorOff();
void illuminatorSetBrightness(uint8_t brightness);
uint8_t illuminatorGetBrightness();
void colorCalSaveIlluminator();

// ---- Secondary illuminator (D10) ----

void illuminator2SetBrightness(uint8_t brightness);
uint8_t illuminator2GetBrightness();
void colorCalSaveIlluminator2();

// ---- AS7341 on-board LED (the AS7341's own illuminant) ----
//
// Flashed ON (with the external LEDs OFF) only while the AS7341 takes a
// reading or captures a calibration point; OFF at all other times — the
// external white LEDs handle idle and ESP32-CAM lighting. State-tracked, so
// repeated calls in the live calibration/diagnostics loops are cheap no-ops.
void colorOnboardLedOn();
void colorOnboardLedOff();

#endif // COLOR_SENSOR_H
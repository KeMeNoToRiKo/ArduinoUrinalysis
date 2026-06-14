#include "colourSensor.h"

// ============================================
// GLOBALS
// ============================================

ColorCalStep     colorCalStep = COLOR_CAL_IDLE;
ColorCalibration colorCalData;

// The AS7341 driver instance (default I2C bus, address 0x39).
static DFRobot_AS7341 as7341;

// AS7341 on-board LED state. The library's controlLed() includes a 100 ms
// settle, so colorOnboardLedOn()/Off() track state and short-circuit when
// already in the requested state — keeping the live calibration/diagnostics
// loops fast and flicker-free.
static bool onboardLedOn = false;

// ============================================
// RAW STATUS READ  (saturation flags the library does not expose)
// ============================================
//
// Reads STATUS_2 (0xA3) directly. The transaction mirrors the DFRobot library's
// own readReg() (write register pointer, STOP, then requestFrom) so it behaves
// identically to the access the library uses for the AVALID bit. No bank switch
// is needed for registers >= 0x80. Returns 0x00 on bus error (no ASAT bits set).
static uint8_t readStatus2() {
  Wire.beginTransmission((uint8_t)AS7341_I2C_ADDR);
  Wire.write((uint8_t)AS7341_REG_STATUS_2);
  if (Wire.endTransmission() != 0) {
    return 0x00;
  }
  Wire.requestFrom((uint8_t)AS7341_I2C_ADDR, (uint8_t)1);
  if (Wire.available() < 1) {
    return 0x00;
  }
  return (uint8_t)Wire.read();
}

// ============================================
// FAST CHANNEL READ  (bypasses the library's per-channel delay)
// ============================================
//
// The DFRobot library's readSpectralDataOne()/Two() call getChannelData() once
// per channel, and getChannelData() ends with a hardcoded delay(50). Six
// channels per bank => 300 ms per bank, 600 ms per full F1-F8 read, multiplied
// again by COLOR_SAMPLE_COUNT in colorReadRawAveraged() — this was the entire
// source of the multi-second lag during calibration and tests.
//
// We read the six 16-bit channel registers (CH0..CH5, 0x95..0xA0) directly in a
// single burst instead. The AS7341 auto-increments the register pointer on a
// multi-byte read, so one requestFrom(12) returns all of them. The transaction
// mirrors readStatus2() / the library's own readReg() (pointer write, STOP,
// then requestFrom) so bus behaviour is identical — just without the naps.
//
// Channel -> field mapping is identical for both SMUX banks (the bank is
// selected by startMeasure()'s SMUX config, not by the data registers):
//   bank 1 (eF1F4ClearNIR): ch0=F1 ch1=F2 ch2=F3 ch3=F4 ch4=CLEAR ch5=NIR
//   bank 2 (eF5F8ClearNIR): ch0=F5 ch1=F6 ch2=F7 ch3=F8 ch4=CLEAR ch5=NIR
//
// Returns true on success; false on bus error or short read (out[] untouched).
static bool readBankRaw(uint16_t out[6]) {
  Wire.beginTransmission((uint8_t)AS7341_I2C_ADDR);
  Wire.write((uint8_t)AS7341_REG_CH0_DATA_L);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  uint8_t got = Wire.requestFrom((uint8_t)AS7341_I2C_ADDR, (uint8_t)12);
  if (got < 12 || Wire.available() < 12) {
    while (Wire.available()) Wire.read();   // drain any partial frame
    return false;
  }
  for (uint8_t ch = 0; ch < 6; ch++) {
    uint8_t lo = (uint8_t)Wire.read();      // CHx_DATA_L
    uint8_t hi = (uint8_t)Wire.read();      // CHx_DATA_H
    out[ch] = ((uint16_t)hi << 8) | lo;
  }
  return true;
}

// ============================================
// SETTINGS -> NUMERIC HELPERS
// ============================================

/**
 * Numeric gain factor for the current AGAIN value (0..10 -> 0.5x..512x).
 */
static float gainX() {
  switch (colorCalData.gain) {
    case AS7341_GAIN_0_5X: return 0.5f;
    case AS7341_GAIN_1X:   return 1.0f;
    case AS7341_GAIN_2X:   return 2.0f;
    case AS7341_GAIN_4X:   return 4.0f;
    case AS7341_GAIN_8X:   return 8.0f;
    case AS7341_GAIN_16X:  return 16.0f;
    case AS7341_GAIN_32X:  return 32.0f;
    case AS7341_GAIN_64X:  return 64.0f;
    case AS7341_GAIN_128X: return 128.0f;
    case AS7341_GAIN_256X: return 256.0f;
    case AS7341_GAIN_512X: return 512.0f;
  }
  return 1.0f;
}

/**
 * Human-readable gain string for the current AGAIN value.
 */
static const char* gainLabel() {
  switch (colorCalData.gain) {
    case AS7341_GAIN_0_5X: return "0.5x";
    case AS7341_GAIN_1X:   return "1x";
    case AS7341_GAIN_2X:   return "2x";
    case AS7341_GAIN_4X:   return "4x";
    case AS7341_GAIN_8X:   return "8x";
    case AS7341_GAIN_16X:  return "16x";
    case AS7341_GAIN_32X:  return "32x";
    case AS7341_GAIN_64X:  return "64x";
    case AS7341_GAIN_128X: return "128x";
    case AS7341_GAIN_256X: return "256x";
    case AS7341_GAIN_512X: return "512x";
  }
  return "?";
}

/**
 * Integration time in milliseconds for the current ATIME/ASTEP:
 *   t_int = (ATIME + 1) * (ASTEP + 1) * 2.78 us
 */
static float integrationMs() {
  return ((float)colorCalData.atime + 1.0f)
       * ((float)colorCalData.astep + 1.0f)
       * (AS7341_ASTEP_PERIOD_US / 1000.0f);
}

/**
 * Max ADC count for the current ATIME/ASTEP, capped at 16-bit:
 *   max = min(65535, (ATIME + 1) * (ASTEP + 1))
 */
static long as7341MaxCount() {
  long m = ((long)colorCalData.atime + 1L) * ((long)colorCalData.astep + 1L);
  if (m > 65535L) m = 65535L;
  if (m < 1L)     m = 1L;
  return m;
}

/**
 * Strongest of the actual ADC channels (CLEAR + F1..F8) in a reading. The
 * mapped r/g/b are weighted AVERAGES of bands, so they understate the true
 * peak — saturation and gain decisions must look at the raw channels instead.
 */
static uint16_t peakChannel(const RawRGBC& d) {
  uint16_t p = d.c;
  if (d.f1 > p) p = d.f1;
  if (d.f2 > p) p = d.f2;
  if (d.f3 > p) p = d.f3;
  if (d.f4 > p) p = d.f4;
  if (d.f5 > p) p = d.f5;
  if (d.f6 > p) p = d.f6;
  if (d.f7 > p) p = d.f7;
  if (d.f8 > p) p = d.f8;
  return p;
}

/**
 * Collapse the 8-band AS7341 spectrum into the TCS34725-compatible RGBC quad
 * using the AS7341_W_* grouping weights. Each output is a WEIGHTED AVERAGE of
 * its member bands, so the result stays inside a single channel's 16-bit
 * range and cannot overflow. The full spectrum is also stored for the report.
 */
static RawRGBC as7341Combine(const DFRobot_AS7341::sModeOneData_t& d1,
                             const DFRobot_AS7341::sModeTwoData_t& d2) {
  RawRGBC out;

  // Spectrum (informational).
  out.f1  = d1.ADF1;
  out.f2  = d1.ADF2;
  out.f3  = d1.ADF3;
  out.f4  = d1.ADF4;
  out.f5  = d2.ADF5;
  out.f6  = d2.ADF6;
  out.f7  = d2.ADF7;
  out.f8  = d2.ADF8;

  // CLEAR and NIR appear in both banks; average for a slightly steadier value.
  uint32_t clearSum = (uint32_t)d1.ADCLEAR + (uint32_t)d2.ADCLEAR;
  uint32_t nirSum   = (uint32_t)d1.ADNIR   + (uint32_t)d2.ADNIR;
  out.c   = (uint16_t)(clearSum / 2u);
  out.nir = (uint16_t)(nirSum   / 2u);

  // Weighted-average groupings. Guard the weight sums against a zeroed group.
  const float wB = AS7341_W_B_F1 + AS7341_W_B_F2 + AS7341_W_B_F3;
  const float wG = AS7341_W_G_F4 + AS7341_W_G_F5;
  const float wR = AS7341_W_R_F6 + AS7341_W_R_F7 + AS7341_W_R_F8;

  float fb = (wB > 0.0f)
           ? (AS7341_W_B_F1 * d1.ADF1 + AS7341_W_B_F2 * d1.ADF2 + AS7341_W_B_F3 * d1.ADF3) / wB
           : 0.0f;
  float fg = (wG > 0.0f)
           ? (AS7341_W_G_F4 * d1.ADF4 + AS7341_W_G_F5 * d2.ADF5) / wG
           : 0.0f;
  float fr = (wR > 0.0f)
           ? (AS7341_W_R_F6 * d2.ADF6 + AS7341_W_R_F7 * d2.ADF7 + AS7341_W_R_F8 * d2.ADF8) / wR
           : 0.0f;

  if (fr < 0.0f) fr = 0.0f; if (fr > 65535.0f) fr = 65535.0f;
  if (fg < 0.0f) fg = 0.0f; if (fg > 65535.0f) fg = 65535.0f;
  if (fb < 0.0f) fb = 0.0f; if (fb > 65535.0f) fb = 65535.0f;

  out.r = (uint16_t)(fr + 0.5f);
  out.g = (uint16_t)(fg + 0.5f);
  out.b = (uint16_t)(fb + 0.5f);

  // Saturation flags are filled in by the caller (colorReadRaw) from STATUS_2.
  out.satAnalog  = false;
  out.satDigital = false;
  return out;
}

// ============================================
// I2C BUS RECOVERY  (shared OLED + AS7341 bus)
// ============================================
//
// See the doc comment on i2cBusRecover() in colourSensor.h for the full
// rationale. In short: a slave (the AS7341) left mid-byte by a reset holds SDA
// low, wedging the whole bus so both the AS7341 AND the OLED that shares it
// fail/hang. We free it by bit-banging SCL until the slave releases SDA, then
// synthesise a STOP and hand the pins back to the hardware I2C peripheral.
//
// SDA/SCL are the Arduino core's macros for the default Wire pins (on the UNO R4
// WiFi these are the dedicated SDA/SCL header pins). SCL is toggled open-drain
// style — driven LOW, then RELEASED to INPUT_PULLUP so the bus pull-up raises it
// — so we never fight a slave that is also driving the line.
bool i2cBusRecover() {
  // Take SDA/SCL back from the IIC peripheral as plain GPIO. pinMode() re-muxes
  // the pins, so this works whether or not Wire.begin() has run yet.
  pinMode(SCL, INPUT_PULLUP);
  pinMode(SDA, INPUT_PULLUP);
  delayMicroseconds(10);

  // Bus already idle (SDA released high)? Nothing is stuck — (re)attach and go.
  if (digitalRead(SDA) == HIGH) {
    Wire.begin();
    Wire.setClock(100000);
    return true;
  }

  // Clock SCL up to 9 times (one byte + ACK) so the stuck slave finishes
  // shifting out its byte and lets SDA go. Stop early the moment SDA frees.
  for (uint8_t i = 0; i < 9; i++) {
    pinMode(SCL, OUTPUT);
    digitalWrite(SCL, LOW);          // pull SCL low
    delayMicroseconds(5);            // ~100 kHz half-period
    pinMode(SCL, INPUT_PULLUP);      // release SCL -> pull-up raises it high
    delayMicroseconds(5);
    if (digitalRead(SDA) == HIGH) break;
  }

  // Synthesise a STOP condition: SDA rises while SCL is high.
  pinMode(SDA, OUTPUT);
  digitalWrite(SDA, LOW);            // SDA low...
  delayMicroseconds(5);
  pinMode(SCL, INPUT_PULLUP);        // ...SCL released high...
  delayMicroseconds(5);
  pinMode(SDA, INPUT_PULLUP);        // ...SDA released high while SCL high = STOP
  delayMicroseconds(5);

  bool freed = (digitalRead(SDA) == HIGH);

  // Re-attach the hardware I2C peripheral for normal Wire use.
  Wire.begin();
  Wire.setClock(100000);
  return freed;
}

// ============================================
// INITIALISATION
// ============================================

/**
 * Scan the I2C bus and print every address that ACKs. Called when the AS7341
 * fails to respond, so the boot log shows exactly what is (and isn't) on the
 * bus. Wire is already initialised by this point (as7341.begin() calls it).
 */
static void i2cScanReport() {
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("[Color]   I2C device found at 0x");
      if (addr < 0x10) Serial.print("0");
      Serial.println(addr, HEX);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("[Color]   No I2C devices responded — check bus wiring, pull-ups and power.");
  } else {
    Serial.print("[Color]   ");
    Serial.print(found);
    Serial.println(" device(s) on bus. The AS7341 should appear at 0x39 (TCS34725 was 0x29).");
  }
}

bool colorSensorInit() {
  // Bring up the white-LED illuminator pins immediately so they're not
  // floating during the rest of init. Brightness is applied later once the
  // stored calibration (including illumBrightness) is loaded.
  illuminatorInit();

  // begin() initialises the I2C bus (Wire.begin()) and powers the chip.
  // Returns 0 (ERR_OK) on success; negative on bus error.
  int rc = as7341.begin();   // eSpm (single-shot, blocking) measurement mode
  if (rc != 0) {
    Serial.print("[Color] AS7341 begin() failed, rc=");
    Serial.println(rc);
    Serial.println("[Color] AS7341 did not ACK at I2C 0x39. Scanning bus...");
    i2cScanReport();
    Serial.println("[Color] Check: AS7341 wired to SDA(A4)/SCL(A5) and powered; "
                   "old TCS34725 (0x29) removed; board address is 0x39.");
    return false;
  }

  uint8_t id = as7341.readID();
  Serial.print("[Color] AS7341 detected. ID=0x");
  Serial.println(id, HEX);

  // Load or apply default calibration.
  if (!colorCalLoad()) {
    Serial.println("[Color] No valid EEPROM calibration found — using defaults.");
    colorCalResetToDefaults();
  } else {
    Serial.println("[Color] Calibration loaded from EEPROM.");
    colorCalPrint();
  }

  // Apply stored integration time / step / gain.
  colorSensorApplySettings();

  // Idle illumination. Two illuminators, used by different sensors and never
  // together: the external D9/D10 white LEDs are the always-on light (they lit
  // the ESP32-CAM and stay on through menus), while the AS7341's on-board LED
  // is the AS7341's OWN illuminant, flashed on only while the AS7341 itself
  // reads/calibrates. Start with the on-board LED OFF.
  as7341.enableLed(false);   // on-board LED off at idle (onboardLedOn stays false)

  // External white-LED illuminators on for the idle/ESP32-CAM light.
  illuminatorOn();

  Serial.println("[Color] Sensor initialised and running.");
  return true;
}

void colorSensorApplySettings() {
  as7341.setAtime(colorCalData.atime);
  as7341.setAstep(colorCalData.astep);
  as7341.setAGAIN(colorCalData.gain);

  Serial.print("[Color] ATIME=");
  Serial.print(colorCalData.atime);
  Serial.print("  ASTEP=");
  Serial.print(colorCalData.astep);
  Serial.print("  (~");
  Serial.print(integrationMs(), 1);
  Serial.print(" ms)  Gain=");
  Serial.println(gainLabel());
}

// ============================================
// READING
// ============================================

// ---------------------------------------------------------------------------
// waitAvalid() — poll STATUS_2.AVALID with a hard timeout.
//
// The DFRobot startMeasure() call is NON-BLOCKING: it writes the SMUX
// configuration and kicks the ADC, then returns immediately. AVALID (bit 6
// of STATUS_2, register 0xA3) is only set once the integration finishes and
// the result registers have been latched. Calling readSpectralData*() before
// AVALID is set causes the library's internal polling loop to spin forever,
// hanging the MCU.
//
// Returns true  when AVALID rose within the timeout.
// Returns false on timeout (sensor stalled / I²C error) — the caller should
// treat the subsequent read as invalid and return a zeroed RawRGBC.
// ---------------------------------------------------------------------------
static bool waitAvalid(unsigned long timeoutMs) {
  // A fresh integration physically cannot complete before integrationMs() has
  // elapsed, so sleep that long FIRST, then poll. This also makes any stale
  // AVALID bit harmless: startMeasure() re-arms SP_EN, but with the read path
  // now fast (no 600 ms of library delays) the two banks run close together,
  // and polling immediately could otherwise latch onto the AVALID left set by
  // the previous bank and read its data before the new frame is ready. Waiting
  // one integration period guarantees the bit we read belongs to THIS bank.
  // Costs only the integration time we are obliged to wait for regardless.
  unsigned long intMs = (unsigned long)(integrationMs() + 0.5f);
  if (intMs > 0) delay(intMs);

  unsigned long deadline = millis() + timeoutMs;
  while ((long)(deadline - millis()) > 0) {
    uint8_t st = readStatus2();
    if (st & AS7341_STATUS2_AVALID) return true;
    delay(2);
  }
  return false;
}

RawRGBC colorReadRaw() {
  RawRGBC zero;
  memset(&zero, 0, sizeof(zero));

  // ---- Bank 1: F1-F4 + Clear + NIR ----
  // startMeasure() is non-blocking — it only writes SMUX config and sets
  // SP_EN. We must wait for AVALID before reading data registers.
  as7341.startMeasure(DFRobot_AS7341::eF1F4ClearNIR);

  if (!waitAvalid(AS7341_AVALID_TIMEOUT_MS)) {
    Serial.println("[Color] ERROR: AVALID timeout on bank 1 — sensor stalled. "
                   "Recovering I2C bus and returning zeroed reading.");
    i2cBusRecover();   // free the bus so the OLED (shared wire) doesn't hang next
    return zero;
  }

  // Direct burst read of CH0..CH5 (no per-channel delay(50); see readBankRaw).
  uint16_t ch1[6];
  if (!readBankRaw(ch1)) {
    Serial.println("[Color] ERROR: I2C read failed on bank 1. "
                   "Recovering I2C bus and returning zeroed reading.");
    i2cBusRecover();
    return zero;
  }
  DFRobot_AS7341::sModeOneData_t d1;
  d1.ADF1    = ch1[0];
  d1.ADF2    = ch1[1];
  d1.ADF3    = ch1[2];
  d1.ADF4    = ch1[3];
  d1.ADCLEAR = ch1[4];
  d1.ADNIR   = ch1[5];

  // Read saturation flags for bank 1 before the next startMeasure() clears them.
  uint8_t st1 = readStatus2();

  // ---- Bank 2: F5-F8 + Clear + NIR ----
  as7341.startMeasure(DFRobot_AS7341::eF5F8ClearNIR);

  if (!waitAvalid(AS7341_AVALID_TIMEOUT_MS)) {
    Serial.println("[Color] ERROR: AVALID timeout on bank 2 — sensor stalled. "
                   "Recovering I2C bus and returning zeroed reading.");
    i2cBusRecover();
    return zero;
  }

  uint16_t ch2[6];
  if (!readBankRaw(ch2)) {
    Serial.println("[Color] ERROR: I2C read failed on bank 2. "
                   "Recovering I2C bus and returning zeroed reading.");
    i2cBusRecover();
    return zero;
  }
  DFRobot_AS7341::sModeTwoData_t d2;
  d2.ADF5    = ch2[0];
  d2.ADF6    = ch2[1];
  d2.ADF7    = ch2[2];
  d2.ADF8    = ch2[3];
  d2.ADCLEAR = ch2[4];
  d2.ADNIR   = ch2[5];

  uint8_t st2 = readStatus2();

  RawRGBC out = as7341Combine(d1, d2);

  // A band in EITHER bank railing makes the whole reading suspect.
  uint8_t st = st1 | st2;
  out.satAnalog  = (st & AS7341_STATUS2_ASAT_ANALOG)  != 0;
  out.satDigital = (st & AS7341_STATUS2_ASAT_DIGITAL) != 0;
  return out;
}

RawRGBC colorReadRawAveraged() {
  uint32_t sumR = 0, sumG = 0, sumB = 0, sumC = 0;
  uint32_t sumF1 = 0, sumF2 = 0, sumF3 = 0, sumF4 = 0;
  uint32_t sumF5 = 0, sumF6 = 0, sumF7 = 0, sumF8 = 0, sumNIR = 0;
  bool satA = false, satD = false;

  // Each colorReadRaw() blocks for a full integration on each bank, so every
  // sample is already independent; a short settle delay between them is plenty.
  for (int i = 0; i < COLOR_SAMPLE_COUNT; i++) {
    if (i > 0) delay(COLOR_SAMPLE_DELAY);
    RawRGBC s = colorReadRaw();
    sumR += s.r; sumG += s.g; sumB += s.b; sumC += s.c;
    sumF1 += s.f1; sumF2 += s.f2; sumF3 += s.f3; sumF4 += s.f4;
    sumF5 += s.f5; sumF6 += s.f6; sumF7 += s.f7; sumF8 += s.f8;
    sumNIR += s.nir;
    satA |= s.satAnalog;     // any sample saturating taints the average
    satD |= s.satDigital;
  }

  RawRGBC avg;
  avg.r = (uint16_t)(sumR / COLOR_SAMPLE_COUNT);
  avg.g = (uint16_t)(sumG / COLOR_SAMPLE_COUNT);
  avg.b = (uint16_t)(sumB / COLOR_SAMPLE_COUNT);
  avg.c = (uint16_t)(sumC / COLOR_SAMPLE_COUNT);
  avg.f1 = (uint16_t)(sumF1 / COLOR_SAMPLE_COUNT);
  avg.f2 = (uint16_t)(sumF2 / COLOR_SAMPLE_COUNT);
  avg.f3 = (uint16_t)(sumF3 / COLOR_SAMPLE_COUNT);
  avg.f4 = (uint16_t)(sumF4 / COLOR_SAMPLE_COUNT);
  avg.f5 = (uint16_t)(sumF5 / COLOR_SAMPLE_COUNT);
  avg.f6 = (uint16_t)(sumF6 / COLOR_SAMPLE_COUNT);
  avg.f7 = (uint16_t)(sumF7 / COLOR_SAMPLE_COUNT);
  avg.f8 = (uint16_t)(sumF8 / COLOR_SAMPLE_COUNT);
  avg.nir = (uint16_t)(sumNIR / COLOR_SAMPLE_COUNT);
  avg.satAnalog  = satA;
  avg.satDigital = satD;
  return avg;
}

// ============================================
// AUTOMATIC GAIN CONTROL (AGC)
// ============================================
//
// Pick the highest AS7341 analogue gain that keeps the brightest RAW channel of
// the CURRENT scene below saturation. Run on the WHITE (clear-water) reference
// during calibration — the brightest thing this sealed transmission box ever
// sees — so the gain stored with that reference also leaves every (always
// dimmer) coloured test sample comfortable headroom. See colourSensor.h.
//
// Two MONOTONIC phases avoid the oscillation a naive up/down search hits when
// no gain lands the peak exactly inside the window (gains step by 2x, so one
// notch can jump a dim peak straight past the window into saturation):
//   1. raise gain while the peak is clearly too dim AND not saturated;
//   2. lower gain while saturated OR too bright.
// Each phase walks the 11 gain steps at most once, so the search always ends.
bool colorAutoGain() {
  const long maxCount = as7341MaxCount();
  const long lo = (maxCount * (long)COLOR_AGC_TARGET_LO_PCT) / 100L;
  const long hi = (maxCount * (long)COLOR_AGC_TARGET_HI_PCT) / 100L;

  // Averaged (not single-shot) reads so the gain decision is stable: a lone
  // noisy sample right at the LO/HI boundary could otherwise tip AGC a full 2x
  // notch. AGC only runs once per white-reference capture, so the extra
  // COLOR_SAMPLE_COUNT integrations per step are an acceptable one-time cost.

  // Phase 1 — too dim: raise gain.
  for (uint8_t i = 0; i < 11 && colorCalData.gain < AS7341_GAIN_64X; i++) {
    RawRGBC d = colorReadRawAveraged();
    if (d.satAnalog || d.satDigital) break;          // already bright enough
    if ((long)peakChannel(d) >= lo)   break;          // reached the window
    colorSetGain(colorCalData.gain + 1);
  }

  // Phase 2 — too bright / saturated: lower gain.
  for (uint8_t i = 0; i < 11 && colorCalData.gain > AS7341_GAIN_0_5X; i++) {
    RawRGBC d = colorReadRawAveraged();
    if (!(d.satAnalog || d.satDigital) && (long)peakChannel(d) <= hi) break;
    colorSetGain(colorCalData.gain - 1);
  }

  // Final read for the verdict + log.
  RawRGBC f   = colorReadRawAveraged();
  long    peak = (long)peakChannel(f);
  bool    ok   = !(f.satAnalog || f.satDigital) && peak >= lo && peak <= hi;

  Serial.print("[Color] AGC -> gain=");
  Serial.print(gainLabel());
  Serial.print("  peak=");
  Serial.print(peak);
  Serial.print("/");
  Serial.print(maxCount);
  Serial.println(ok ? "  (in window)" : "  (best effort — at gain limit)");
  return ok;
}

// ============================================
// AMBIENT-LIGHT-LEAK CHECK
// ============================================
//
// Switch EVERY illuminant off and read: a sealed box is pitch black with the
// lights off, so anything above COLOR_AMBIENT_LEAK_COUNTS is room light leaking
// past the lid and will bias every reference and result. Leaves all lights OFF;
// the caller restores whatever illumination it needs next. See colourSensor.h.
AmbientLeak colorCheckAmbientLeak() {
  illuminatorOff();
  colorOnboardLedOff();
  delay(COLOR_FLASH_SETTLE_MS);          // let the LEDs fully extinguish + settle

  RawRGBC d = colorReadRaw();

  AmbientLeak r;
  r.clear = d.c;
  r.peak  = peakChannel(d);
  r.leak  = ((long)r.peak  > (long)COLOR_AMBIENT_LEAK_COUNTS) ||
            ((long)r.clear > (long)COLOR_AMBIENT_LEAK_COUNTS);

  if (r.leak) {
    Serial.print("[Color] AMBIENT LEAK: lights-off peak=");
    Serial.print(r.peak);
    Serial.print(" clear=");
    Serial.print(r.clear);
    Serial.print(" (> ");
    Serial.print((long)COLOR_AMBIENT_LEAK_COUNTS);
    Serial.println(") — room light is leaking into the box; seal the lid.");
  }
  return r;
}

// ============================================
// NORMALISATION  (opt. IR comp -> white/dark balance -> CCM -> tint -> gamma -> 8-bit)
// ============================================

/**
 * Prepare one RGBC point for the white-balance divide, returning float R/G/B.
 *
 * When COLOR_IR_COMPENSATE_RGB is enabled, a broadband IR term is estimated as
 * IR = (R + G + B - C) / 2 (clamped >= 0) and subtracted equally from R, G, B.
 * The SAME estimate is used for the sample and both stored references so the
 * divide stays self-consistent — the stored references only carry r/g/b/c, so
 * the estimate (rather than the AS7341's measured NIR) is used here on purpose.
 * The measured NIR is still available in RawRGBC.nir for the serial report.
 *
 * When disabled (default for this sealed white-LED box) the channels pass
 * straight through and the (sample - dark) / (white - dark) balance stays
 * perfectly linear.
 */
static void irCompPoint(uint16_t r, uint16_t g, uint16_t b, uint16_t c,
                        float& outR, float& outG, float& outB) {
#if COLOR_IR_COMPENSATE_RGB
  long ir = ((long)r + (long)g + (long)b - (long)c) / 2;
  if (ir < 0) ir = 0;
#else
  const long ir = 0;
  (void)c;
#endif
  float fr = (float)r - (float)ir;
  float fg = (float)g - (float)ir;
  float fb = (float)b - (float)ir;
  outR = (fr < 0.0f) ? 0.0f : fr;
  outG = (fg < 0.0f) ? 0.0f : fg;
  outB = (fb < 0.0f) ? 0.0f : fb;
}

/**
 * Apply the 3x3 colour-correction matrix (COLOR_CCM_*) in place. Identity = no-op.
 */
static void applyCCM(float& r, float& g, float& b) {
  float nr = COLOR_CCM_RR * r + COLOR_CCM_RG * g + COLOR_CCM_RB * b;
  float ng = COLOR_CCM_GR * r + COLOR_CCM_GG * g + COLOR_CCM_GB * b;
  float nb = COLOR_CCM_BR * r + COLOR_CCM_BG * g + COLOR_CCM_BB * b;
  r = nr;
  g = ng;
  b = nb;
}

/**
 * Apply the configured output gamma. COLOR_OUTPUT_GAMMA == 1.0 is a no-op.
 */
static float applyOutputGamma(float v) {
  if (COLOR_OUTPUT_GAMMA == 1.0f) return v;
  if (v <= 0.0f) return 0.0f;
  return powf(v, COLOR_OUTPUT_GAMMA);
}

NormalisedRGB colorNormalise(const RawRGBC& raw) {
  NormalisedRGB norm = {0, 0, 0};

  // Per-channel white-minus-dark span. A floor prevents an uncalibrated unit
  // (collapsed span) from amplifying ADC noise into +/-255 swings.
  const float MIN_SPAN = 100.0f;

  // 1) (Optional) IR-compensate the sample AND both references identically.
  float sR, sG, sB;
  irCompPoint(raw.r, raw.g, raw.b, raw.c, sR, sG, sB);

  float wR, wG, wB;
  irCompPoint(colorCalData.white.r, colorCalData.white.g,
              colorCalData.white.b, colorCalData.white.c, wR, wG, wB);

  float dR, dG, dB;
  irCompPoint(colorCalData.dark.r, colorCalData.dark.g,
              colorCalData.dark.b, colorCalData.dark.c, dR, dG, dB);

  // 2) White-balance spans (white - dark, per channel).
  float spanR = wR - dR;
  float spanG = wG - dG;
  float spanB = wB - dB;

  bool degraded = false;
  if (fabsf(spanR) < MIN_SPAN) { spanR = MIN_SPAN; degraded = true; }
  if (fabsf(spanG) < MIN_SPAN) { spanG = MIN_SPAN; degraded = true; }
  if (fabsf(spanB) < MIN_SPAN) { spanB = MIN_SPAN; degraded = true; }

  if (degraded) {
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 5000UL) {
      Serial.println("[Color] WARNING: white-dark span < 100 counts on at least "
                     "one channel — re-calibrate for accurate output.");
      lastWarn = millis();
    }
  }

  // 3) White balance -> linear reflectance in [0, 1].
  float r = (sR - dR) / spanR;
  float g = (sG - dG) / spanG;
  float b = (sB - dB) / spanB;
  r = constrain(r, 0.0f, 1.0f);
  g = constrain(g, 0.0f, 1.0f);
  b = constrain(b, 0.0f, 1.0f);

  // 4) Colour-correction matrix (identity = no-op).
  applyCCM(r, g, b);
  r = constrain(r, 0.0f, 1.0f);
  g = constrain(g, 0.0f, 1.0f);
  b = constrain(b, 0.0f, 1.0f);

  // 4b) Water-calibration tint — reinstate the pale-yellow baseline that
  //     white-balancing against clear water removes (water -> hue ~56 deg,
  //     avoiding a false Flag A and a collapsed KNN hue term). See header.
  g *= COLOR_WATER_TINT_G;
  b *= COLOR_WATER_TINT_B;
  g = constrain(g, 0.0f, 1.0f);
  b = constrain(b, 0.0f, 1.0f);

  // 5) Output gamma (linear by default), then scale to 8-bit with rounding.
  r = applyOutputGamma(r);
  g = applyOutputGamma(g);
  b = applyOutputGamma(b);

  norm.r = (uint8_t)(constrain(r, 0.0f, 1.0f) * 255.0f + 0.5f);
  norm.g = (uint8_t)(constrain(g, 0.0f, 1.0f) * 255.0f + 0.5f);
  norm.b = (uint8_t)(constrain(b, 0.0f, 1.0f) * 255.0f + 0.5f);
  return norm;
}

NormalisedRGB colorRead() {
  RawRGBC raw = colorReadRawAveraged();
  return colorNormalise(raw);
}

// ============================================
// DERIVED METRICS  (AS7341)
// ============================================

/**
 * Correlated Colour Temperature (CCT, Kelvin) — diagnostic estimate.
 *
 * The TCS34725's ams DN40 CCT polynomial does NOT apply to the AS7341, so CCT
 * is estimated from the mapped R/G/B via the standard sRGB->XYZ->xy transform
 * and the McCamy cubic. Ratio-based, so it is invariant to ATIME/ASTEP/gain.
 * Returns 0 when the reading is degenerate. NOT spectrally calibrated.
 */
uint16_t colorCalcCCT(const RawRGBC& raw) {
  float maxc = (float)as7341MaxCount();
  float r = (float)raw.r / maxc;
  float g = (float)raw.g / maxc;
  float b = (float)raw.b / maxc;

  if (r + g + b < 1e-4f) return 0;

  // sRGB primaries -> CIE XYZ (D65).
  float X = 0.4124f * r + 0.3576f * g + 0.1805f * b;
  float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  float Z = 0.0193f * r + 0.1192f * g + 0.9505f * b;

  float sum = X + Y + Z;
  if (sum < 1e-6f) return 0;

  float x = X / sum;
  float y = Y / sum;

  // McCamy (1992): n = (x - 0.3320) / (y - 0.1858)
  float denom = (y - 0.1858f);
  if (fabsf(denom) < 1e-6f) return 0;
  float n = (x - 0.3320f) / denom;

  float cct = 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;

  if (cct < 1000.0f)  cct = 1000.0f;
  if (cct > 25000.0f) cct = 25000.0f;
  return (uint16_t)cct;
}

/**
 * Relative illuminance proxy — diagnostic only (NOT calibrated lux).
 *
 * Derived from the broadband CLEAR channel, normalised by integration time and
 * gain so it stays approximately invariant to ATIME/ASTEP/AGAIN. Returns 0 if
 * the clear channel is zero (no light).
 */
float colorCalcLux(const RawRGBC& raw) {
  if (raw.c == 0) return 0.0f;

  float denom = integrationMs() * gainX();
  if (denom < 1e-3f) return 0.0f;

  float lux = AS7341_LUX_K * (float)raw.c / denom;
  if (lux < 0.0f) lux = 0.0f;
  return lux;
}

void colorPrintReport(const RawRGBC& raw, const NormalisedRGB& norm) {
  // Recompute the white-balanced LINEAR triplet so the serial log exposes the
  // intermediate stage. Record "Linear" against a known-true sRGB colour to
  // fit the COLOR_CCM_* matrix (see header).
  float sR, sG, sB, wR, wG, wB, dR, dG, dB;
  irCompPoint(raw.r, raw.g, raw.b, raw.c, sR, sG, sB);
  irCompPoint(colorCalData.white.r, colorCalData.white.g,
              colorCalData.white.b, colorCalData.white.c, wR, wG, wB);
  irCompPoint(colorCalData.dark.r, colorCalData.dark.g,
              colorCalData.dark.b, colorCalData.dark.c, dR, dG, dB);
  float spanR = wR - dR; if (fabsf(spanR) < 100.0f) spanR = 100.0f;
  float spanG = wG - dG; if (fabsf(spanG) < 100.0f) spanG = 100.0f;
  float spanB = wB - dB; if (fabsf(spanB) < 100.0f) spanB = 100.0f;
  float linR = constrain((sR - dR) / spanR, 0.0f, 1.0f);
  float linG = constrain((sG - dG) / spanG, 0.0f, 1.0f);
  float linB = constrain((sB - dB) / spanB, 0.0f, 1.0f);

  Serial.println("[Color] --- Measurement Report ---");
  Serial.print  ("  Spectrum F1="); Serial.print(raw.f1);
  Serial.print  (" F2=");           Serial.print(raw.f2);
  Serial.print  (" F3=");           Serial.print(raw.f3);
  Serial.print  (" F4=");           Serial.print(raw.f4);
  Serial.print  (" F5=");           Serial.print(raw.f5);
  Serial.print  (" F6=");           Serial.print(raw.f6);
  Serial.print  (" F7=");           Serial.print(raw.f7);
  Serial.print  (" F8=");           Serial.println(raw.f8);
  Serial.print  ("  NIR=");         Serial.print(raw.nir);
  Serial.print  ("  Clear=");       Serial.println(raw.c);
  Serial.print  ("  Mapped R=");    Serial.print(raw.r);
  Serial.print  ("  G=");           Serial.print(raw.g);
  Serial.print  ("  B=");           Serial.println(raw.b);
  Serial.print  ("  Linear R=");    Serial.print(linR, 4);
  Serial.print  ("  G=");           Serial.print(linG, 4);
  Serial.print  ("  B=");           Serial.println(linB, 4);
  Serial.print  ("  Norm   R=");    Serial.print(norm.r);
  Serial.print  ("  G=");           Serial.print(norm.g);
  Serial.print  ("  B=");           Serial.println(norm.b);
  Serial.print  ("  Hex   #");
  if (norm.r < 0x10) Serial.print("0"); Serial.print(norm.r, HEX);
  if (norm.g < 0x10) Serial.print("0"); Serial.print(norm.g, HEX);
  if (norm.b < 0x10) Serial.print("0"); Serial.println(norm.b, HEX);
  Serial.print  ("  Lux*  ");   Serial.print(colorCalcLux(raw), 1);
  Serial.println(" (relative)");
  Serial.print  ("  CCT*  ");   Serial.print(colorCalcCCT(raw)); Serial.println(" K (approx)");
  Serial.print  ("  SAT   analog="); Serial.print(raw.satAnalog ? "YES" : "no");
  Serial.print  ("  digital=");      Serial.println(raw.satDigital ? "YES" : "no");
  if (raw.satAnalog || raw.satDigital) {
    Serial.println("  >>> SATURATED — reading unreliable; lower gain/integration "
                   "or dim the illuminator. <<<");
  }
  Serial.println("[Color] ---------------------------------");
}

// ============================================
// CALIBRATION — STATE MACHINE
// ============================================

void colorCalBegin() {
  colorCalStep = COLOR_CAL_DARK;
  Serial.println("[Color] Calibration started.");
  Serial.println("[Color] Step 1: Fill cuvette with BLACK liquid (food colouring + water), then press CAPTURE.");
}

void colorCalCapture() {
  // Ambient-leak guard — runs at EVERY calibration step. Kills all illuminants
  // and reads; in a sealed box the sensor should see only dark current, so a
  // reading above the threshold means room light is leaking in and will corrupt
  // whichever reference we are about to capture. Leaves every light OFF.
  AmbientLeak amb = colorCheckAmbientLeak();
  if (amb.leak) {
    Serial.println("[Color] WARNING: capturing a calibration reference with an "
                   "ambient light leak — seal the box and re-capture.");
  }

  // colorCheckAmbientLeak() left every light off — already correct for the DARK
  // reference (a true black level). For the WHITE reference, relight the AS7341's
  // own on-board LED and let AGC pick the gain on this brightest reference before
  // we capture it; the chosen gain is stored with the references (colorCalSave)
  // and every later, always-dimmer, test runs at it with guaranteed headroom.
  // (The DARK reference was captured at the previous gain, but dark is ~0 counts
  // at any gain, so the small mismatch is negligible in (sample-dark)/(white-dark).)
  if (colorCalStep == COLOR_CAL_WHITE) {
    colorOnboardLedOn();
    delay(COLOR_FLASH_SETTLE_MS);
    colorAutoGain();
  }

  RawRGBC raw = colorReadRawAveraged();

  switch (colorCalStep) {
    case COLOR_CAL_DARK:
      colorCalData.dark.r = raw.r;
      colorCalData.dark.g = raw.g;
      colorCalData.dark.b = raw.b;
      colorCalData.dark.c = raw.c;
      Serial.println("[Color] Dark reference (black liquid) captured:");
      Serial.print  ("  R="); Serial.print(raw.r);
      Serial.print  ("  G="); Serial.print(raw.g);
      Serial.print  ("  B="); Serial.print(raw.b);
      Serial.print  ("  C="); Serial.println(raw.c);
      Serial.println("[Color] Step 2: Replace with CLEAR WATER, then press CAPTURE.");
      colorCalStep = COLOR_CAL_WHITE;
      break;

    case COLOR_CAL_WHITE:
      colorCalData.white.r = raw.r;
      colorCalData.white.g = raw.g;
      colorCalData.white.b = raw.b;
      colorCalData.white.c = raw.c;
      Serial.println("[Color] White reference captured:");
      Serial.print  ("  R="); Serial.print(raw.r);
      Serial.print  ("  G="); Serial.print(raw.g);
      Serial.print  ("  B="); Serial.print(raw.b);
      Serial.print  ("  C="); Serial.println(raw.c);

      // Saturation guard. Two checks:
      //  (a) hardware ASAT flag from STATUS_2 — definitive: a band actually
      //      railed during this capture.
      //  (b) heuristic: the strongest RAW band within 10% of the theoretical
      //      max count for the current ATIME/ASTEP (catches "about to rail").
      if (raw.satAnalog || raw.satDigital) {
        Serial.print("[Color] WARNING: white reference SATURATED (");
        if (raw.satAnalog)  Serial.print("analog ");
        if (raw.satDigital) Serial.print("digital ");
        Serial.println(")— lower gain/integration or dim the illuminator, then recapture.");
      }
      {
        // Judge headroom against the PEAK raw channel (CLEAR + F1..F8), NOT the
        // mapped r/g/b. Those are weighted band AVERAGES (see peakChannel()):
        // a single band one notch from the rail is diluted by its 2-3 group
        // members to well below satThresh, so the old r/g/b test silently
        // passed near-clipped white references. peakChannel() is also exactly
        // what colorAutoGain() targets, so the two stay consistent.
        long maxCount  = as7341MaxCount();
        long satThresh = (maxCount * 9L) / 10L;
        long peak      = (long)peakChannel(raw);
        if (peak > satThresh) {
          Serial.print("[Color] WARNING: white reference near saturation (peak ");
          Serial.print(peak);
          Serial.print("/");
          Serial.print(maxCount);
          Serial.println(") — reduce gain or integration time for a cleaner cal.");
        }
      }

      Serial.println("[Color] Both points captured. Call colorCalSave() to store.");
      colorCalStep = COLOR_CAL_DONE;
      break;

    case COLOR_CAL_DONE:
      Serial.println("[Color] Already done — call colorCalSave() or colorCalBegin() to restart.");
      break;

    default:
      Serial.println("[Color] colorCalCapture() called outside calibration sequence.");
      break;
  }
}

void colorCalSave() {
  if (colorCalStep != COLOR_CAL_DONE) {
    Serial.println("[Color] Cannot save — calibration not complete.");
    return;
  }

  colorCalData.magic = COLOR_EEPROM_MAGIC;
  EEPROM.put(COLOR_EEPROM_ADDR, colorCalData);
  colorCalStep = COLOR_CAL_IDLE;

  Serial.println("[Color] Calibration saved to EEPROM.");
  colorCalPrint();
}

void colorCalCancel() {
  colorCalLoad();
  // AGC may have changed the live gain while capturing the white reference;
  // re-apply the restored calibration so the sensor hardware matches the data.
  colorSensorApplySettings();
  colorCalStep = COLOR_CAL_IDLE;
  Serial.println("[Color] Calibration cancelled. Previous data restored.");
}

bool colorCalLoad() {
  EEPROM.get(COLOR_EEPROM_ADDR, colorCalData);
  if (colorCalData.magic != COLOR_EEPROM_MAGIC) {
    return false;
  }
  return true;
}

void colorCalResetToDefaults() {
  colorCalData.magic = COLOR_EEPROM_MAGIC;

  // Dark: assume the sensor reads near-zero with no light / black liquid.
  colorCalData.dark.r = 0;
  colorCalData.dark.g = 0;
  colorCalData.dark.b = 0;
  colorCalData.dark.c = 0;

  // White: a placeholder mid-range value under the default settings. Real
  // counts depend on LED brightness and geometry; the boot log warns loudly
  // to run a proper white/dark calibration.
  colorCalData.white.r = 8000;
  colorCalData.white.g = 8000;
  colorCalData.white.b = 8000;
  colorCalData.white.c = 8000;

  colorCalData.atime = AS7341_DEFAULT_ATIME;
  colorCalData.astep = AS7341_DEFAULT_ASTEP;
  colorCalData.gain  = AS7341_DEFAULT_GAIN;

  colorCalData.illumBrightness  = ILLUM_DEFAULT_BRIGHTNESS;
  colorCalData.illumBrightness2 = ILLUM2_DEFAULT_BRIGHTNESS;

  EEPROM.put(COLOR_EEPROM_ADDR, colorCalData);
  Serial.println("[Color] Default calibration applied and saved to EEPROM.");
  Serial.println("[Color] NOTE: defaults are placeholders — run a real "
                 "white/dark calibration for accurate readings.");
}

const char* colorCalStepLabel() {
  switch (colorCalStep) {
    case COLOR_CAL_IDLE:  return "Idle";
    case COLOR_CAL_DARK:  return "Black liquid (dark ref)";
    case COLOR_CAL_WHITE: return "Fill cuvette w/ water";
    case COLOR_CAL_DONE:  return "Press SELECT to save";
    default:              return "Unknown";
  }
}

void colorCalPrint() {
  Serial.println("[Color] --- Calibration Data ---");
  Serial.print  ("  Dark  | R="); Serial.print(colorCalData.dark.r);
  Serial.print  ("  G=");         Serial.print(colorCalData.dark.g);
  Serial.print  ("  B=");         Serial.print(colorCalData.dark.b);
  Serial.print  ("  C=");         Serial.println(colorCalData.dark.c);
  Serial.print  ("  White | R="); Serial.print(colorCalData.white.r);
  Serial.print  ("  G=");         Serial.print(colorCalData.white.g);
  Serial.print  ("  B=");         Serial.print(colorCalData.white.b);
  Serial.print  ("  C=");         Serial.println(colorCalData.white.c);
  Serial.print  ("  ATIME : ");   Serial.print(colorCalData.atime);
  Serial.print  ("  ASTEP : ");   Serial.print(colorCalData.astep);
  Serial.print  ("  (~");         Serial.print(integrationMs(), 1);
  Serial.println(" ms)");
  Serial.print  ("  Gain  : ");   Serial.println(gainLabel());
  Serial.println("[Color] ----------------------------");
}

// ============================================
// SETTINGS HELPERS
// ============================================
//
// The AS7341 applies new ATIME/ASTEP/AGAIN on the next startMeasure(), and
// colorReadRaw() always issues a fresh startMeasure() before reading, so there
// is no stale-frame problem to flush (unlike the free-running TCS34725).

void colorSetIntegrationTime(uint8_t atime) {
  colorCalData.atime = atime;
  as7341.setAtime(atime);
  Serial.print("[Color] ATIME set to ");
  Serial.print(atime);
  Serial.print("  (~");
  Serial.print(integrationMs(), 1);
  Serial.println(" ms)");
}

void colorSetAstep(uint16_t astep) {
  colorCalData.astep = astep;
  as7341.setAstep(astep);
  Serial.print("[Color] ASTEP set to ");
  Serial.print(astep);
  Serial.print("  (~");
  Serial.print(integrationMs(), 1);
  Serial.println(" ms)");
}

void colorSetGain(uint8_t gain) {
  colorCalData.gain = gain;
  as7341.setAGAIN(gain);
  Serial.print("[Color] Gain set to ");
  Serial.println(gainLabel());
}

uint8_t colorGetIntegrationTime() {
  return colorCalData.atime;
}

uint16_t colorGetAstep() {
  return colorCalData.astep;
}

uint8_t colorGetGain() {
  return colorCalData.gain;
}

// ============================================
// EXTERNAL ILLUMINATOR  (white LEDs)  — unchanged
// ============================================

void illuminatorInit() {
  pinMode(ILLUM_PIN,  OUTPUT);
  pinMode(ILLUM2_PIN, OUTPUT);
  analogWrite(ILLUM_PIN,  0);
  analogWrite(ILLUM2_PIN, 0);
}

void illuminatorOn() {
  analogWrite(ILLUM_PIN,  colorCalData.illumBrightness);
  analogWrite(ILLUM2_PIN, colorCalData.illumBrightness2);
}

void illuminatorOff() {
  analogWrite(ILLUM_PIN,  0);
  analogWrite(ILLUM2_PIN, 0);
}

void illuminatorSetBrightness(uint8_t brightness) {
  colorCalData.illumBrightness = brightness;
  analogWrite(ILLUM_PIN, brightness);
}

uint8_t illuminatorGetBrightness() {
  return colorCalData.illumBrightness;
}

void colorCalSaveIlluminator() {
  colorCalData.magic = COLOR_EEPROM_MAGIC;
  EEPROM.put(COLOR_EEPROM_ADDR, colorCalData);
  Serial.print("[Color] Illuminator 1 brightness saved: ");
  Serial.println(colorCalData.illumBrightness);
}

// ============================================
// SECONDARY ILLUMINATOR (D10)  — unchanged
// ============================================

void illuminator2SetBrightness(uint8_t brightness) {
  colorCalData.illumBrightness2 = brightness;
  analogWrite(ILLUM2_PIN, brightness);
}

uint8_t illuminator2GetBrightness() {
  return colorCalData.illumBrightness2;
}

void colorCalSaveIlluminator2() {
  colorCalData.magic = COLOR_EEPROM_MAGIC;
  EEPROM.put(COLOR_EEPROM_ADDR, colorCalData);
  Serial.print("[Color] Illuminator 2 brightness saved: ");
  Serial.println(colorCalData.illumBrightness2);
}

// ============================================
// AS7341 ON-BOARD LED  (the AS7341's own illuminant)
// ============================================
//
// Flashed ON only while the AS7341 integrates a reading or captures a
// calibration point — with the external D9/D10 LEDs switched OFF — so the
// spectral sensor sees a single known illuminant. enableLed(true) hands the
// LED pin to register control and sets the "on" bit; controlLed() sets the
// drive current (and re-asserts "on"). enableLed(false) clears both -> off.
//
// State-tracked: repeated colorOnboardLedOn()/Off() calls from the live
// calibration and diagnostics loops are no-ops once in the requested state,
// so they neither re-pay controlLed()'s 100 ms settle nor blink the LED.

void colorOnboardLedOn() {
  if (onboardLedOn) return;
  as7341.enableLed(true);
  as7341.controlLed(AS7341_ONBOARD_LED_CURRENT);
  onboardLedOn = true;
}

void colorOnboardLedOff() {
  if (!onboardLedOn) return;
  as7341.enableLed(false);
  onboardLedOn = false;
}
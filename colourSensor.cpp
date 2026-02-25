#include "colourSensor.h"

// ============================================
// GLOBALS
// ============================================

ColorCalStep     colorCalStep = COLOR_CAL_IDLE;
ColorCalibration colorCalData;

// ============================================
// LOW-LEVEL I2C HELPERS
// ============================================

static void tcsWrite8(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCS34725_I2C_ADDR);
  Wire.write(TCS34725_COMMAND_BIT | reg);
  Wire.write(value);
  Wire.endTransmission();
}

static uint8_t tcsRead8(uint8_t reg) {
  Wire.beginTransmission(TCS34725_I2C_ADDR);
  Wire.write(TCS34725_COMMAND_BIT | reg);
  Wire.endTransmission();
  Wire.requestFrom(TCS34725_I2C_ADDR, 1);
  return Wire.read();
}

static uint16_t tcsRead16(uint8_t reg) {
  Wire.beginTransmission(TCS34725_I2C_ADDR);
  Wire.write(TCS34725_COMMAND_BIT | reg);
  Wire.endTransmission();
  Wire.requestFrom(TCS34725_I2C_ADDR, 2);
  uint16_t lo = Wire.read();
  uint16_t hi = Wire.read();
  return (hi << 8) | lo;
}

// ============================================
// INITIALISATION
// ============================================

bool colorSensorInit() {
  Wire.begin();

  // Verify chip ID
  uint8_t id = tcsRead8(TCS34725_REG_ID);
  if (id != TCS34725_ID_TCS34725 && id != TCS34725_ID_TCS34727) {
    Serial.print("[Color] Sensor not found! ID=0x");
    Serial.println(id, HEX);
    return false;
  }
  Serial.print("[Color] TCS34725 detected. ID=0x");
  Serial.println(id, HEX);

  // Load or apply default calibration
  if (!colorCalLoad()) {
    Serial.println("[Color] No valid EEPROM calibration found — using defaults.");
    colorCalResetToDefaults();
  } else {
    Serial.println("[Color] Calibration loaded from EEPROM.");
    colorCalPrint();
  }

  // Power on and enable ADC
  tcsWrite8(TCS34725_REG_ENABLE, TCS34725_ENABLE_PON);
  delay(3);   // Oscillator warm-up (datasheet §3.5)
  tcsWrite8(TCS34725_REG_ENABLE, TCS34725_ENABLE_PON | TCS34725_ENABLE_AEN);

  // Apply stored integration time and gain
  colorSensorApplySettings();

  Serial.println("[Color] Sensor initialised and running.");
  return true;
}

void colorSensorApplySettings() {
  tcsWrite8(TCS34725_REG_ATIME,   colorCalData.atime);
  tcsWrite8(TCS34725_REG_CONTROL, colorCalData.gain);

  Serial.print("[Color] Integration time ATIME=0x");
  Serial.print(colorCalData.atime, HEX);
  Serial.print("  Gain=");
  switch (colorCalData.gain) {
    case TCS34725_GAIN_1X:  Serial.println("1x");  break;
    case TCS34725_GAIN_4X:  Serial.println("4x");  break;
    case TCS34725_GAIN_16X: Serial.println("16x"); break;
    case TCS34725_GAIN_60X: Serial.println("60x"); break;
    default:                Serial.println("?");   break;
  }
}

// ============================================
// READING
// ============================================

RawRGBC colorReadRaw() {
  // Wait for ADC-valid flag (with a generous timeout)
  unsigned long timeout = millis() + 1000UL;
  while (!(tcsRead8(TCS34725_REG_STATUS) & TCS34725_STATUS_AVALID)) {
    if (millis() > timeout) {
      Serial.println("[Color] Timeout waiting for ADC data!");
      break;
    }
    delay(5);
  }

  RawRGBC raw;
  raw.c = tcsRead16(TCS34725_REG_CDATAL);
  raw.r = tcsRead16(TCS34725_REG_RDATAL);
  raw.g = tcsRead16(TCS34725_REG_GDATAL);
  raw.b = tcsRead16(TCS34725_REG_BDATAL);
  return raw;
}

RawRGBC colorReadRawAveraged() {
  uint32_t sumR = 0, sumG = 0, sumB = 0, sumC = 0;

  for (int i = 0; i < COLOR_SAMPLE_COUNT; i++) {
    RawRGBC s = colorReadRaw();
    sumR += s.r;
    sumG += s.g;
    sumB += s.b;
    sumC += s.c;
    delay(COLOR_SAMPLE_DELAY);
  }

  RawRGBC avg;
  avg.r = (uint16_t)(sumR / COLOR_SAMPLE_COUNT);
  avg.g = (uint16_t)(sumG / COLOR_SAMPLE_COUNT);
  avg.b = (uint16_t)(sumB / COLOR_SAMPLE_COUNT);
  avg.c = (uint16_t)(sumC / COLOR_SAMPLE_COUNT);
  return avg;
}

// ============================================
// NORMALISATION  (white/dark correction → 8-bit)
// ============================================

NormalisedRGB colorNormalise(const RawRGBC& raw) {
  NormalisedRGB norm = {0, 0, 0};

  // Guard: avoid divide-by-zero if white == dark
  float wR = (float)colorCalData.white.r - (float)colorCalData.dark.r;
  float wG = (float)colorCalData.white.g - (float)colorCalData.dark.g;
  float wB = (float)colorCalData.white.b - (float)colorCalData.dark.b;

  if (fabsf(wR) < 1.0f) wR = 1.0f;
  if (fabsf(wG) < 1.0f) wG = 1.0f;
  if (fabsf(wB) < 1.0f) wB = 1.0f;

  float r = ((float)raw.r - (float)colorCalData.dark.r) / wR;
  float g = ((float)raw.g - (float)colorCalData.dark.g) / wG;
  float b = ((float)raw.b - (float)colorCalData.dark.b) / wB;

  // Clamp to [0, 1] then scale to [0, 255]
  norm.r = (uint8_t)(constrain(r, 0.0f, 1.0f) * 255.0f);
  norm.g = (uint8_t)(constrain(g, 0.0f, 1.0f) * 255.0f);
  norm.b = (uint8_t)(constrain(b, 0.0f, 1.0f) * 255.0f);
  return norm;
}

NormalisedRGB colorRead() {
  RawRGBC raw = colorReadRawAveraged();
  return colorNormalise(raw);
}

// ============================================
// DERIVED METRICS
// ============================================

/**
 * Correlated Colour Temperature (CCT) using the Hernandez-Andres formula.
 * Reference: Hernandez-Andres et al., "Calculating correlated color
 * temperatures across the entire gamut of daylight and skylight chromaticities"
 * Applied Optics, 1999.
 *
 * Step 1: Calculate chromaticity (x, y) from XYZ space.
 * The TCS34725 r/g/b channels are not CIE XYZ but we use manufacturer-
 * recommended conversion coefficients (from the ams app note).
 */
uint16_t colorCalcCCT(const RawRGBC& raw) {
  if (raw.c == 0) return 0;

  // Manufacturer XYZ conversion coefficients
  float X = (-0.14282f * raw.r) + (1.54924f * raw.g) + (-0.95641f * raw.b);
  float Y = (-0.32466f * raw.r) + (1.57837f * raw.g) + (-0.73191f * raw.b);
  float Z = (-0.68202f * raw.r) + (0.77073f * raw.g) + ( 0.56332f * raw.b);

  float denom = X + Y + Z;
  if (fabsf(denom) < 1e-6f) return 0;

  float xc = X / denom;
  float yc = Y / denom;

  // McCamy's approximation
  float n = (xc - 0.3320f) / (0.1858f - yc);
  uint16_t cct = (uint16_t)(449.0f * n * n * n
                           + 3525.0f * n * n
                           + 6823.3f * n
                           + 5520.33f);
  return cct;
}

/**
 * Illuminance (lux) using the ams simplified formula.
 * Coefficients from the TCS34725 datasheet / ams application note DN40.
 */
float colorCalcLux(const RawRGBC& raw) {
  if (raw.c == 0) return 0.0f;

  float lux = (-0.32466f * (float)raw.r)
             + (1.57837f * (float)raw.g)
             + (-0.73191f * (float)raw.b);
  if (lux < 0.0f) lux = 0.0f;
  return lux;
}

void colorPrintReport(const RawRGBC& raw, const NormalisedRGB& norm) {
  Serial.println("[Color] --- Measurement Report ---");
  Serial.print  ("  Raw   R="); Serial.print(raw.r);
  Serial.print  ("  G=");       Serial.print(raw.g);
  Serial.print  ("  B=");       Serial.print(raw.b);
  Serial.print  ("  C=");       Serial.println(raw.c);
  Serial.print  ("  Norm  R="); Serial.print(norm.r);
  Serial.print  ("  G=");       Serial.print(norm.g);
  Serial.print  ("  B=");       Serial.println(norm.b);
  Serial.print  ("  Hex   #");
  if (norm.r < 0x10) Serial.print("0"); Serial.print(norm.r, HEX);
  if (norm.g < 0x10) Serial.print("0"); Serial.print(norm.g, HEX);
  if (norm.b < 0x10) Serial.print("0"); Serial.println(norm.b, HEX);
  Serial.print  ("  Lux   "); Serial.println(colorCalcLux(raw), 1);
  Serial.print  ("  CCT   "); Serial.print(colorCalcCCT(raw)); Serial.println(" K");
  Serial.println("[Color] ---------------------------------");
}

// ============================================
// CALIBRATION — STATE MACHINE
// ============================================

void colorCalBegin() {
  colorCalStep = COLOR_CAL_DARK;
  Serial.println("[Color] Calibration started.");
  Serial.println("[Color] Step 1: Cover the sensor completely, then press CAPTURE.");
}

void colorCalCapture() {
  RawRGBC raw = colorReadRawAveraged();

  switch (colorCalStep) {
    case COLOR_CAL_DARK:
      colorCalData.dark.r = raw.r;
      colorCalData.dark.g = raw.g;
      colorCalData.dark.b = raw.b;
      colorCalData.dark.c = raw.c;
      Serial.println("[Color] Dark reference captured:");
      Serial.print  ("  R="); Serial.print(raw.r);
      Serial.print  ("  G="); Serial.print(raw.g);
      Serial.print  ("  B="); Serial.print(raw.b);
      Serial.print  ("  C="); Serial.println(raw.c);
      Serial.println("[Color] Step 2: Place sensor over white reference, then press CAPTURE.");
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

  // Dark: assume sensor reads near-zero when fully covered
  colorCalData.dark.r = 0;
  colorCalData.dark.g = 0;
  colorCalData.dark.b = 0;
  colorCalData.dark.c = 0;

  // White: assume saturation at ~80% of 16-bit full-scale (~52000) at 4x gain
  colorCalData.white.r = 52000;
  colorCalData.white.g = 52000;
  colorCalData.white.b = 52000;
  colorCalData.white.c = 52000;

  colorCalData.atime = TCS34725_DEFAULT_ATIME;
  colorCalData.gain  = TCS34725_DEFAULT_GAIN;

  EEPROM.put(COLOR_EEPROM_ADDR, colorCalData);
  Serial.println("[Color] Default calibration applied and saved to EEPROM.");
}

const char* colorCalStepLabel() {
  switch (colorCalStep) {
    case COLOR_CAL_IDLE:  return "Idle";
    case COLOR_CAL_DARK:  return "Cover sensor (dark ref)";
    case COLOR_CAL_WHITE: return "Place on white surface";
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
  Serial.print  ("  ATIME : 0x"); Serial.println(colorCalData.atime, HEX);
  Serial.print  ("  Gain  : ");
  switch (colorCalData.gain) {
    case TCS34725_GAIN_1X:  Serial.println("1x");  break;
    case TCS34725_GAIN_4X:  Serial.println("4x");  break;
    case TCS34725_GAIN_16X: Serial.println("16x"); break;
    case TCS34725_GAIN_60X: Serial.println("60x"); break;
    default:                Serial.println("?");   break;
  }
  Serial.println("[Color] ----------------------------");
}

// ============================================
// SETTINGS HELPERS
// ============================================

void colorSetIntegrationTime(uint8_t atime) {
  colorCalData.atime = atime;
  tcsWrite8(TCS34725_REG_ATIME, atime);
  Serial.print("[Color] Integration time set to ATIME=0x");
  Serial.println(atime, HEX);
}

void colorSetGain(uint8_t gain) {
  colorCalData.gain = gain;
  tcsWrite8(TCS34725_REG_CONTROL, gain);
  Serial.print("[Color] Gain set to ");
  switch (gain) {
    case TCS34725_GAIN_1X:  Serial.println("1x");  break;
    case TCS34725_GAIN_4X:  Serial.println("4x");  break;
    case TCS34725_GAIN_16X: Serial.println("16x"); break;
    case TCS34725_GAIN_60X: Serial.println("60x"); break;
    default:                Serial.println("?");   break;
  }
}

uint8_t colorGetIntegrationTime() {
  return colorCalData.atime;
}

uint8_t colorGetGain() {
  return colorCalData.gain;
}
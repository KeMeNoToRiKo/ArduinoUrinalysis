#include "cameraSensor.h"

// ============================================
// GLOBALS
// ============================================

CamCalStep camCalStep = CAM_CAL_IDLE;
uint16_t   camLastCalR = 0;
uint16_t   camLastCalG = 0;
uint16_t   camLastCalB = 0;
bool       camOnline   = false;

// ============================================
// LOW-LEVEL UART HELPERS
// ============================================

/**
 * Drain any unread bytes sitting in the Serial1 RX buffer.
 * Called before sending a command so we don't pick up stale chatter
 * (e.g. boot messages from the ESP32). A short settle pass also catches
 * bytes that were just-about-to-arrive when the first drain finished.
 */
static void drainRx() {
  while (CAM_SERIAL.available()) CAM_SERIAL.read();
  delay(2);
  while (CAM_SERIAL.available()) CAM_SERIAL.read();
}

/**
 * Send a single line (terminated with '\n') and read back one line.
 *
 * Reads up to (responseLen - 1) characters into `response`.
 * Returns true if a full line was received before the timeout, false otherwise.
 *
 * '\r' is silently stripped so the function works whether the peer sends
 * "OK\n" or "OK\r\n".
 */
static bool sendCommand(const char* cmd, char* response, size_t responseLen,
                        unsigned long timeoutMs) {
  drainRx();

  CAM_SERIAL.print(cmd);
  CAM_SERIAL.print('\n');
  CAM_SERIAL.flush();

  unsigned long deadline = millis() + timeoutMs;
  size_t pos = 0;

  while ((long)(deadline - millis()) > 0) {
    while (CAM_SERIAL.available()) {
      char c = (char)CAM_SERIAL.read();

      if (c == '\r') continue;
      if (c == '\n') {
        response[pos] = '\0';
        return true;
      }
      if (pos < responseLen - 1) {
        response[pos++] = c;
      }
      // If the line is longer than our buffer, keep consuming until '\n'
      // so the next call doesn't see leftover bytes.
    }
    delay(2);
  }

  response[pos] = '\0';
  return false;
}

// ============================================
// INITIALISATION
// ============================================
//
// IMPORTANT: this MUST be the first thing called in setup() (right after
// Serial.begin). The ESP32-CAM emits its "READY" line ~1 second after
// power-on. If Serial1 isn't open and being drained in real time before
// then, the small hardware RX buffer (~256 B on R4) overflows mid-boot
// and READY is dropped. That manifests as random "READY not received"
// failures even though the ESP32 is fine.
//
void cameraSensorInit() {
  CAM_SERIAL.begin(CAM_BAUD);

  // Default Stream timeout is 1000 ms. We never want to block that long
  // on a partial read. (We don't actually use readStringUntil below — we
  // parse byte-by-byte — but other callers might.)
  CAM_SERIAL.setTimeout(50);

  // ---- Phase 1: opportunistic READY listen ----
  //
  // Parse byte-by-byte: this lets us drain the boot stream as fast as it
  // arrives, never letting the hardware buffer fill up.
  //
  // We also echo any non-READY lines to the main Serial monitor — if the
  // ESP32 fails to bring up its camera (typical brownout symptom) it will
  // print "ERR,camera_init_failed" before READY, and that should be visible.
  unsigned long deadline = millis() + 8000UL;
  bool gotReady = false;
  String linebuf = "";

  while ((long)(deadline - millis()) > 0 && !gotReady) {
    while (CAM_SERIAL.available()) {
      char c = (char)CAM_SERIAL.read();
      if (c == '\r') continue;
      if (c == '\n') {
        linebuf.trim();
        if (linebuf == "READY") { gotReady = true; break; }
        if (linebuf.length() > 0) {
          Serial.print("[Cam boot] ");
          Serial.println(linebuf);
        }
        linebuf = "";
      } else if (linebuf.length() < 96) {
        linebuf += c;
      }
    }
    delay(5);
  }

  if (gotReady) {
    Serial.println("[Cam] ESP32-CAM sent READY.");
  } else {
    Serial.println("[Cam] READY not received in time — falling back to PING probe.");
  }

  // ---- Phase 2: drain residual bytes ----
  // Catch anything that landed after READY (or any leftover boot fragment)
  // so the first PING response line isn't polluted.
  drainRx();

  // ---- Phase 3: probe with retries ----
  //
  // A single PING failure right after boot is not a reliable signal that
  // the ESP32 is dead. The camera driver may still be settling, or the
  // board may have just recovered from a brief brownout. Retry several
  // times with backoff before giving up.
  bool ok = false;
  for (int attempt = 0; attempt < 4 && !ok; attempt++) {
    if (attempt > 0) {
      Serial.print("[Cam] Retrying PING (");
      Serial.print(attempt + 1);
      Serial.println("/4)...");
      delay(750);
    }
    ok = cameraIsReady();
  }

  if (ok) {
    Serial.println("[Cam] ESP32-CAM online (PING/PONG OK).");
  } else {
    Serial.println("[Cam] WARNING: ESP32-CAM did not respond to PING.");
    Serial.println("[Cam] If you see other sensor LEDs dim around now, that's a");
    Serial.println("[Cam] POWER symptom — the ESP32-CAM peaks at ~600 mA during");
    Serial.println("[Cam] boot/capture and will brown out the Arduino 5V rail.");
    Serial.println("[Cam] Use a separate 5V (>=1A) supply for the ESP32-CAM with");
    Serial.println("[Cam] a common ground. No software fix can compensate for it.");
  }
}

bool cameraIsReady() {
  char buf[32];
  bool ok = sendCommand("PING", buf, sizeof(buf), CAM_PING_TIMEOUT_MS)
            && strcmp(buf, "PONG") == 0;
  camOnline = ok;
  return ok;
}

// ============================================
// READING
// ============================================

CameraRGB cameraRead() {
  CameraRGB out = { 0, 0, 0, false };

  // If we previously marked the camera offline, opportunistically re-probe
  // before giving up — handles the case where the ESP32 has recovered
  // (e.g. user fixed power, or a transient brownout passed) mid-session.
  if (!camOnline && !cameraIsReady()) return out;

  char buf[64];
  if (!sendCommand("READ", buf, sizeof(buf), CAM_READ_TIMEOUT_MS)) {
    Serial.println("[Cam] READ timed out.");
    camOnline = false;
    return out;
  }

  unsigned int r, g, b;
  if (sscanf(buf, "RGB,%u,%u,%u", &r, &g, &b) == 3) {
    out.r = (uint8_t)(r > 255 ? 255 : r);
    out.g = (uint8_t)(g > 255 ? 255 : g);
    out.b = (uint8_t)(b > 255 ? 255 : b);
    out.valid = true;
  } else {
    Serial.print("[Cam] Unexpected READ response: ");
    Serial.println(buf);
  }
  return out;
}

// ============================================
// CALIBRATION
// ============================================

void camCalBegin() {
  camCalStep  = CAM_CAL_DARK;
  camLastCalR = camLastCalG = camLastCalB = 0;
  Serial.println("[Cam] Calibration started.");
  Serial.println("[Cam] Step 1: Cover the camera lens completely, then CAPTURE.");
}

/**
 * Common parser for CAL_DARK / CAL_WHITE responses of the form
 *    OK,r,g,b
 */
static bool parseOkRGB(const char* buf, uint16_t& r, uint16_t& g, uint16_t& b) {
  unsigned int rr, gg, bb;
  if (sscanf(buf, "OK,%u,%u,%u", &rr, &gg, &bb) != 3) return false;
  r = (uint16_t)rr;
  g = (uint16_t)gg;
  b = (uint16_t)bb;
  return true;
}

void camCalCapture() {
  if (!camOnline && !cameraIsReady()) {
    Serial.println("[Cam] Cannot capture — ESP32-CAM offline.");
    return;
  }

  char buf[64];
  uint16_t r, g, b;

  switch (camCalStep) {
    case CAM_CAL_DARK:
      if (sendCommand("CAL_DARK", buf, sizeof(buf), CAM_CAL_TIMEOUT_MS)
          && parseOkRGB(buf, r, g, b)) {
        camLastCalR = r; camLastCalG = g; camLastCalB = b;
        Serial.print("[Cam] Dark captured: R="); Serial.print(r);
        Serial.print(" G="); Serial.print(g);
        Serial.print(" B="); Serial.println(b);
        Serial.println("[Cam] Step 2: Show a white reference, then CAPTURE.");
        camCalStep = CAM_CAL_WHITE;
      } else {
        Serial.print("[Cam] CAL_DARK failed: ");
        Serial.println(buf);
      }
      break;

    case CAM_CAL_WHITE:
      if (sendCommand("CAL_WHITE", buf, sizeof(buf), CAM_CAL_TIMEOUT_MS)
          && parseOkRGB(buf, r, g, b)) {
        camLastCalR = r; camLastCalG = g; camLastCalB = b;
        Serial.print("[Cam] White captured: R="); Serial.print(r);
        Serial.print(" G="); Serial.print(g);
        Serial.print(" B="); Serial.println(b);
        Serial.println("[Cam] Both points captured. Press SELECT to save.");
        camCalStep = CAM_CAL_DONE;
      } else {
        Serial.print("[Cam] CAL_WHITE failed: ");
        Serial.println(buf);
      }
      break;

    case CAM_CAL_DONE:
      Serial.println("[Cam] Already done — call camCalSave() or camCalBegin().");
      break;

    default:
      Serial.println("[Cam] camCalCapture() called outside calibration sequence.");
      break;
  }
}

void camCalSave() {
  if (camCalStep != CAM_CAL_DONE) {
    Serial.println("[Cam] Cannot save — calibration not complete.");
    return;
  }

  char buf[32];
  if (sendCommand("CAL_SAVE", buf, sizeof(buf), CAM_CAL_TIMEOUT_MS)
      && strcmp(buf, "OK") == 0) {
    Serial.println("[Cam] Calibration saved on ESP32-CAM.");
  } else {
    Serial.print("[Cam] CAL_SAVE failed: ");
    Serial.println(buf);
  }
  camCalStep = CAM_CAL_IDLE;
}

void camCalCancel() {
  camCalStep = CAM_CAL_IDLE;
  Serial.println("[Cam] Calibration cancelled. ESP32 calibration unchanged.");
}

void camCalResetToDefaults() {
  char buf[32];
  if (sendCommand("CAL_RESET", buf, sizeof(buf), CAM_CAL_TIMEOUT_MS)
      && strcmp(buf, "OK") == 0) {
    Serial.println("[Cam] Calibration reset to defaults on ESP32-CAM.");
  } else {
    Serial.print("[Cam] CAL_RESET failed: ");
    Serial.println(buf);
  }
}

const char* camCalStepLabel() {
  switch (camCalStep) {
    case CAM_CAL_IDLE:  return "Idle";
    case CAM_CAL_DARK:  return "Cover lens (dark ref)";
    case CAM_CAL_WHITE: return "Show white reference";
    case CAM_CAL_DONE:  return "Press SELECT to save";
    default:            return "Unknown";
  }
}

void camCalPrint() {
  if (!camOnline && !cameraIsReady()) {
    Serial.println("[Cam] Offline — cannot fetch calibration.");
    return;
  }
  char buf[96];
  if (sendCommand("CAL_GET", buf, sizeof(buf), CAM_PING_TIMEOUT_MS)) {
    Serial.print("[Cam] ");
    Serial.println(buf);
  }
}


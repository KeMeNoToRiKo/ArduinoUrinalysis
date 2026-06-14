// ============================================
// KEYPAD — Direct GPIO driver (4×4 matrix)
// ============================================
// Drives a 4×4 matrix keypad using eight Arduino
// digital pins — no I2C expander required.
//
// Scan algorithm (standard row-scan):
//   For each row:
//     1. Drive row pin LOW (all others HIGH / INPUT).
//     2. Read all four column pins (INPUT_PULLUP).
//     3. A LOW column reading means that key is pressed.
//     4. Debounce with a 20 ms re-read; wait for release
//        before returning so each press is reported once.
// ============================================

#include "Keypad.h"

// --------------------------------------------------
// Constants
// --------------------------------------------------

static const int NUM_ROWS = 4;
static const int NUM_COLS = 4;

static const uint8_t ROW_PINS[NUM_ROWS] = {
  KEYPAD_ROW1_PIN,   // Row 1
  KEYPAD_ROW2_PIN,   // Row 2
  KEYPAD_ROW3_PIN,   // Row 3
  KEYPAD_ROW4_PIN,   // Row 4
};

static const uint8_t COL_PINS[NUM_COLS] = {
  KEYPAD_COL1_PIN,   // Col 1
  KEYPAD_COL2_PIN,   // Col 2
  KEYPAD_COL3_PIN,   // Col 3
  KEYPAD_COL4_PIN,   // Col 4
};

// --------------------------------------------------
// Internal helpers
// --------------------------------------------------

/**
 * Set all row pins to INPUT (high-impedance / pulled up via external
 * resistor or just floating high). This is the idle state between scans
 * so no row actively pulls the column lines down.
 */
static void allRowsIdle() {
  for (int r = 0; r < NUM_ROWS; r++) {
    digitalWrite(ROW_PINS[r], HIGH);
    pinMode(ROW_PINS[r], INPUT_PULLUP);
  }
}

/**
 * Drive one row LOW while all others remain idle (INPUT_PULLUP).
 * Column pins then read LOW only if a key in that row is pressed.
 */
static void driveRow(int r) {
  // First return all rows to idle so only one is ever driven LOW.
  allRowsIdle();
  pinMode(ROW_PINS[r], OUTPUT);
  digitalWrite(ROW_PINS[r], LOW);
}

/**
 * Read the column state while a row is being driven.
 * Returns true if the given column reads LOW (key pressed).
 */
static bool colPressed(int c) {
  return (digitalRead(COL_PINS[c]) == LOW);
}

// --------------------------------------------------
// Public API
// --------------------------------------------------

/**
 * Initialise all keypad pins.
 *   Row pins → INPUT_PULLUP (idle; driven LOW one at a time during scan)
 *   Col pins → INPUT_PULLUP (HIGH when open, pulled LOW by row when pressed)
 * Call once from setup().
 */
void keypadInit() {
  for (int r = 0; r < NUM_ROWS; r++) {
    pinMode(ROW_PINS[r], INPUT_PULLUP);
    digitalWrite(ROW_PINS[r], HIGH);
  }
  for (int c = 0; c < NUM_COLS; c++) {
    pinMode(COL_PINS[c], INPUT_PULLUP);
  }
}

/**
 * Scan the 4×4 matrix and return the pressed key (1–16),
 * or 0 if no key is down.
 *
 * Each key is debounced with a 20 ms re-sample. The function blocks
 * until the key is released so every press is reported exactly once.
 */
int scanKey() {
  for (int r = 0; r < NUM_ROWS; r++) {
    driveRow(r);
    delayMicroseconds(50);   // let the driven row settle before reading

    for (int c = 0; c < NUM_COLS; c++) {
      if (colPressed(c)) {
        // ---- Debounce: re-sample after 20 ms ----
        delay(20);
        driveRow(r);
        delayMicroseconds(50);

        if (colPressed(c)) {
          // Key confirmed — wait for release before returning.
          while (true) {
            allRowsIdle();
            delay(5);
            driveRow(r);
            delayMicroseconds(50);
            if (!colPressed(c)) break;   // gone HIGH → released
          }
          allRowsIdle();   // leave pins tidy on exit
          return r * NUM_COLS + c + 1;
        }
      }
    }
    allRowsIdle();   // restore before scanning next row
  }
  return 0;   // no key pressed
}
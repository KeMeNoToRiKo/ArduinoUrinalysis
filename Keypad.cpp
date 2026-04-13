// ============================================
// KEYPAD — PCF8574 I2C driver
// ============================================
// Replaces the direct-GPIO driver with one that
// communicates through a PCF8574 I/O expander.
//
// The public API (keypadInit / scanKey) is kept
// identical to the original so no call-site changes
// are needed in the rest of the project.
//
// PCF8574 I/O bit layout (one byte, 8 bits):
//
//   Bit:  7    6    5    4    3    2    1    0
//         C4   C3   C2   C1   R4   R3   R2   R1
//
//   Rows  → lower nibble (bits 3:0), active-LOW output
//   Cols  → upper nibble (bits 7:4), INPUT with pull-ups
//              (PCF8574 outputs written HIGH act as
//               weak pull-up inputs — no external
//               resistors needed on column lines)
// ============================================

#include "Keypad.h"
#include <Wire.h>

// --------------------------------------------------
// Constants
// --------------------------------------------------

// Idle state: all rows HIGH (not driving), all cols HIGH (pulled up)
static const uint8_t IDLE_BYTE = 0xFF;

// Row-drive bytes — one row pulled LOW at a time, cols remain HIGH
// Bit layout: [C4 C3 C2 C1 R4 R3 R2 R1]
static const uint8_t ROW_SELECT[4] = {
  0xFE,   // R1 LOW: 1111 1110
  0xFD,   // R2 LOW: 1111 1101
  0xFB,   // R3 LOW: 1111 1011
  0xF7,   // R4 LOW: 1111 0111
};

// Column bit masks in the upper nibble
static const uint8_t COL_MASK[4] = {
  0x10,   // C1 = bit 4
  0x20,   // C2 = bit 5
  0x40,   // C3 = bit 6
  0x80,   // C4 = bit 7
};

static const int NUM_ROWS = 4;
static const int NUM_COLS = 4;

// --------------------------------------------------
// Internal helpers
// --------------------------------------------------

/**
 * Write one byte to the PCF8574.
 * Returns true on success.
 */
static bool pcfWrite(uint8_t data) {
  Wire.beginTransmission(PCF8574_ADDR);
  Wire.write(data);
  return (Wire.endTransmission() == 0);
}

/**
 * Read one byte from the PCF8574.
 * On I2C error returns IDLE_BYTE (safe "no key" value).
 */
static uint8_t pcfRead() {
  if (Wire.requestFrom((uint8_t)PCF8574_ADDR, (uint8_t)1) != 1) {
    return IDLE_BYTE;
  }
  return Wire.read();
}

// --------------------------------------------------
// Public API
// --------------------------------------------------

/**
 * Initialise I2C and put the PCF8574 into its idle state.
 * Call once from setup().
 *
 * Note: Wire.begin() is safe to call even if the OLED or
 * another peripheral has already called it — the Arduino
 * Wire library ignores duplicate begin() calls.
 */
void keypadInit() {
  Wire.begin();
  pcfWrite(IDLE_BYTE);   // all pins HIGH → rows floating, cols pulled up
}

/**
 * Scan the 4×4 matrix and return the pressed key (1–16),
 * or 0 if no key is down.
 *
 * Key numbering:
 *   Row 1: 1  2  3  4
 *   Row 2: 5  6  7  8
 *   Row 3: 9 10 11 12
 *   Row 4: 13 14 15 16
 *
 * Identical numbering to the original GPIO driver, so all
 * call-sites in the project work without modification.
 */
int scanKey() {
  for (int r = 0; r < NUM_ROWS; r++) {

    // Drive this row LOW, leave everything else HIGH
    pcfWrite(ROW_SELECT[r]);
    delayMicroseconds(50);   // allow signal to settle before reading

    uint8_t state = pcfRead();

    // Restore idle before processing so the bus is clean
    pcfWrite(IDLE_BYTE);

    for (int c = 0; c < NUM_COLS; c++) {
      // A LOW column bit means that key is pressed
      if ((state & COL_MASK[c]) == 0) {

        // Debounce: re-drive and re-read after 20 ms
        delay(20);
        pcfWrite(ROW_SELECT[r]);
        delayMicroseconds(50);
        uint8_t confirm = pcfRead();
        pcfWrite(IDLE_BYTE);

        if ((confirm & COL_MASK[c]) == 0) {
          // Key confirmed — wait for release before returning
          while (true) {
            delay(5);
            pcfWrite(ROW_SELECT[r]);
            delayMicroseconds(50);
            uint8_t released = pcfRead();
            pcfWrite(IDLE_BYTE);
            if (released & COL_MASK[c]) break;  // bit gone HIGH → released
          }
          return r * NUM_COLS + c + 1;
        }
      }
    }
  }
  return 0;  // no key pressed
}
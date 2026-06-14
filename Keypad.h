// ============================================
// KEYPAD — Direct GPIO driver (4×4 matrix)
// ============================================
// 4×4 matrix keypad wired directly to Arduino
// digital pins. No I2C expander required.
//
// Pin mapping:
//   ROW pins (driven LOW one at a time):
//     Row 1 → KEYPAD_ROW1_PIN  (D3)
//     Row 2 → KEYPAD_ROW2_PIN  (D4)
//     Row 3 → KEYPAD_ROW3_PIN  (D5)
//     Row 4 → KEYPAD_ROW4_PIN  (D6)
//
//   COL pins (read; INPUT_PULLUP — LOW when pressed):
//     Col 1 → KEYPAD_COL1_PIN  (D7)
//     Col 2 → KEYPAD_COL2_PIN  (D8)
//     Col 3 → KEYPAD_COL3_PIN  (D9)
//     Col 4 → KEYPAD_COL4_PIN  (D10)
//
// Key numbering:
//   Row 1: keys  1  2  3  4
//   Row 2: keys  5  6  7  8
//   Row 3: keys  9 10 11 12
//   Row 4: keys 13 14 15 16
//
// Returns 0 when no key is pressed.
//
// NOTE: D2 is reserved for TDS_POWER_PIN.
//       D12/D13 are the illuminator PWM pins.
//       D11 is left free (SPI MOSI / spare).
// ============================================

#ifndef KEYPAD_H
#define KEYPAD_H

#include <Arduino.h>

// ---- Row output pins (one driven LOW per scan) ----
#define KEYPAD_ROW1_PIN   3
#define KEYPAD_ROW2_PIN   4
#define KEYPAD_ROW3_PIN   5
#define KEYPAD_ROW4_PIN   6

// ---- Column input pins (INPUT_PULLUP; LOW = pressed) ----
#define KEYPAD_COL1_PIN   7
#define KEYPAD_COL2_PIN   8
#define KEYPAD_COL3_PIN   9
#define KEYPAD_COL4_PIN  10

void keypadInit();
int  scanKey();

#endif // KEYPAD_H
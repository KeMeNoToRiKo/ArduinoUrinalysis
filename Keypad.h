// ============================================
// KEYPAD — PCF8574 I2C driver
// ============================================
// 4×4 matrix keypad driven through a PCF8574
// I/O expander over I2C.
//
// Pin mapping (PCF8574 P0–P7):
//   P0 → Row 1    P4 → Col 1
//   P1 → Row 2    P5 → Col 2
//   P2 → Row 3    P6 → Col 3
//   P3 → Row 4    P7 → Col 4
//
// Key numbering (matches original direct-GPIO driver):
//   Row 1: keys  1  2  3  4
//   Row 2: keys  5  6  7  8
//   Row 3: keys  9 10 11 12
//   Row 4: keys 13 14 15 16
//
// Returns 0 when no key is pressed.
// ============================================

#ifndef KEYPAD_H
#define KEYPAD_H

#include <Arduino.h>

// I2C address of the PCF8574.
// Default with A0=A1=A2=GND → 0x20
// PCF8574A variant with A0=A1=A2=GND → 0x38
#ifndef PCF8574_ADDR
  #define PCF8574_ADDR 0x20
#endif

void keypadInit();
int  scanKey();

#endif // KEYPAD_H
#include "Keypad.h"

//  INIT PINS
/**
*         ROWS
*      row 1 -> pin2
*      row 2 -> pin3
*      row 3 -> pin4
*      row 4 -> pin5
*
*         COLUMNS
*      column 1 -> pin6
*      column 2 -> pin7
*      column 3 -> pin8
*      column 4 -> pin9
*/
static const int rowPins[4] = {2, 3, 4, 5};
static const int colPins[4] = {6, 7, 8 ,9};
static const int rowSize = sizeof(rowPins) / sizeof(rowPins[0]);
static const int colSize = sizeof(colPins) / sizeof(colPins[0]);

/**
*   Initialise keypad
*   Rows as OUTPUT (HIGH IDLE)
*   Columns as INPUT_PULLUP
*/

void keypadInit() {

  for (auto i = 0; i < rowSize; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], HIGH);
  }
  for (auto i = 0; i < colSize; i++) {
      pinMode(colPins[i], INPUT_PULLUP);
  }
}

int scanKey() {
  for (auto r = 0; r < rowSize; r++) {
    digitalWrite(rowPins[r], LOW);
    for (auto c = 0; c < colSize; c++) {
      if (digitalRead(colPins[c]) == LOW) {
        delay(20);
        if (digitalRead(colPins[c]) == LOW) {
          int key = r * colSize + c + 1;
          while (digitalRead(colPins[c]) == LOW) {
            delay(5);
          }
          digitalWrite(rowPins[r], HIGH);
          return key;
        }
      }
    }
    digitalWrite(rowPins[r], HIGH);
  }
  return 0;
}
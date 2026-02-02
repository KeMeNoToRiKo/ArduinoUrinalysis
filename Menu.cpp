#include "menu.h"

int cursorPos = 1; // Initialize cursor position
const int menuY[NUM_OPTIONS] = {12, 28, 44};
const char* menuText[NUM_OPTIONS] = {
  false ? "Connection: YES" : "Connection: NO",
  "Start Test",
  "Arduino Settings"
};

void drawMenu(U8G2 &u8g2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Draw connection status (not selectable)
  u8g2.drawStr(2, menuY[0], menuText[0]);

  // Draw selectable items
  for (int i = 1; i < NUM_OPTIONS; i++) {
    if (i == cursorPos) {
      u8g2.drawBox(1, menuY[i] - 10, 127, 12);
      u8g2.setDrawColor(0);
      u8g2.drawStr(2, menuY[i], menuText[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(2, menuY[i], menuText[i]);
    }
  }

  u8g2.sendBuffer();
}
#include "Menu.h"

static Menu* currentMenu = nullptr;
int cursorPos = 0;

void setMenu(Menu* newMenu) {
  currentMenu = newMenu;
  cursorPos = 0;
}

void menuUp() {
  if (!currentMenu) return;
  cursorPos--;
  if (cursorPos < 0) cursorPos = currentMenu->itemCount - 1;
}

void menuDown() {
  if (!currentMenu) return;
  cursorPos++;
  if (cursorPos >= currentMenu->itemCount) cursorPos = 0;
}

void menuSelect() {
  if (!currentMenu) return;
  if (currentMenu->items[cursorPos].action)
    currentMenu->items[cursorPos].action();
}

void drawMenu(U8G2 &u8g2) {
  if (!currentMenu) return;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Title
  u8g2.drawStr(2, 10, currentMenu->title);
  u8g2.drawHLine(0, 12, 128);

  for (int i = 0; i < currentMenu->itemCount; i++) {
    int y = 26 + i * 12;

    if (i == cursorPos) {
      u8g2.drawBox(0, y - 10, 128, 12);
      u8g2.setDrawColor(0);
      u8g2.drawStr(4, y, currentMenu->items[i].text);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(4, y, currentMenu->items[i].text);
    }
  }

  u8g2.sendBuffer();
}

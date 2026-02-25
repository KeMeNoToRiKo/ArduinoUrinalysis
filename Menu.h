#ifndef MENU_H
#define MENU_H

#include <U8g2lib.h>

#define MAX_MENU_ITEMS 6

typedef void (*MenuAction)();   // Function pointer

struct MenuItem {
  const char* text;
  MenuAction action;
};

struct Menu {
  const char* title;
  MenuItem items[MAX_MENU_ITEMS];
  uint8_t itemCount;
};

extern int cursorPos;

void setMenu(Menu* newMenu);
void drawMenu(U8G2 &u8g2);
void menuUp();
void menuDown();
void menuSelect();

#endif

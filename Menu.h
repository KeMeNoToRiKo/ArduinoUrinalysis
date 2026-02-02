#ifndef MENU_H
#define MENU_H

#include <U8g2lib.h>
#include <Wire.h>

// CONFIG MENU
#define NUM_OPTIONS 3
extern int cursorPos; // We'll define this in main sketch
extern const int menuY[NUM_OPTIONS];
extern const char* menuText[NUM_OPTIONS];

// Function to draw the menu
void drawMenu(U8G2 &u8g2);

#endif
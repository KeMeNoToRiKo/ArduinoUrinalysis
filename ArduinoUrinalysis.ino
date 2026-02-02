// ============================================
// ARDUINO URINALYSIS DEVICE
// Device ID: URINE-TEST-001
// Version: 1.0
// ============================================

// INCLUDES
#include "Menu.h"
#include "Keypad.h"
#include "Bluetooth.h"
#include <U8g2lib.h>

// DEVICE CONFIGURATION
#define DEVICE_NAME "URINE-TEST-001"
#define DEVICE_VERSION "1.0"
#define DEVICE_TYPE "Urinalysis Analyzer"

// CUSTOM BLUETOOTH IDENTIFIER
#define BT_IDENTIFIER "URINE-TEST-001-HC05"

// INIT OLED
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

void setup() {
  Serial.begin(9600);
  u8g2.begin();
  keypadInit();
  bluetoothInit(); // Serial1 used for HC-05
  
  // System startup message
  Serial.println("========================================");
  Serial.print("Device: "); Serial.println(DEVICE_NAME);
  Serial.print("Type: "); Serial.println(DEVICE_TYPE);
  Serial.print("Version: "); Serial.println(DEVICE_VERSION);
  Serial.println("System initialized");
  Serial.println("========================================");
  
  // Send device info via Bluetooth
  delay(500);
  sendBluetoothMessage(BT_IDENTIFIER);  // Broadcast custom identifier
}

void loop() {
  // Draw menu on OLED
  drawMenu(u8g2);
  
  // Scan keypad for input
  int key = scanKey();
  
  // Handle Bluetooth communication
  handleBluetooth();

  // Process keypad input
  switch (key) {
      case 2: // UP
        cursorPos--;
        if (cursorPos < 1) cursorPos = NUM_OPTIONS - 1;
        break;
        
      case 10: // DOWN
        cursorPos++;
        if (cursorPos >= NUM_OPTIONS) cursorPos = 1;
        break;
        
      case 5: // LEFT
        // Implement left action if needed
        break;
        
      case 7: // RIGHT
        // Implement right action if needed
        break;
        
      case 15: { // SELECT
        Serial.print("Selected option: "); Serial.println(cursorPos);

        String message = "Option " + String(cursorPos) + " selected";
        sendBluetoothMessage(message.c_str());
        // Handle menu selection logic here
        break;
      }

      case 16: // BACK
        Serial.println("Back pressed");
        sendBluetoothMessage("Back pressed");
        break;
        
      default:
        break;
  }
  
  delay(120);
}

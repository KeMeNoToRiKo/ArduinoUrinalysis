#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <ArduinoBLE.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// ============================================
// BLE SERVICE & CHARACTERISTIC UUIDS
// ============================================

#define BLE_SERVICE_UUID       "180A"
#define DEVICE_INFO_CHAR_UUID  "2A29"   // Manufacturer Name
#define DEVICE_MODEL_CHAR_UUID "2A24"   // Model Number
#define DATA_TX_CHAR_UUID      "2A37"   // Data TX (notify)
#define DATA_RX_CHAR_UUID      "2A38"   // Data RX (write)

// ============================================
// DEFAULTS  (used when EEPROM has no valid data)
// ============================================

#define BLE_DEFAULT_NAME          "URINE-TEST-001"
#define BLE_DEFAULT_MANUFACTURER  "Arduino R4 WiFi"
#define BLE_DEFAULT_MODEL         "URINE-TEST-001"
#define BLE_DEFAULT_TX_POWER      0       // dBm: -40, -20, -16, -12, -8, -4, 0, 4
#define BLE_DEFAULT_ADVERTISING   true

#define JSON_BUFFER_SIZE  256

// ============================================
// EEPROM LAYOUT
// ============================================

// Sits right after the pH calibration block (pHSensor uses 0x00..~0x1F).
// Adjust BLE_EEPROM_ADDR if your pHSensor struct grows.
#define BLE_EEPROM_ADDR   0x40
#define BLE_EEPROM_MAGIC  0xB6

// Maximum length for the broadcast device name (including null terminator)
#define BLE_NAME_MAX_LEN  20

// ============================================
// SETTINGS STRUCT
// ============================================

/**
 * All user-configurable BLE parameters.
 * Persisted to EEPROM so they survive power cycles.
 */
struct BLESettings {
  uint8_t magic;                        // validity marker
  char    localName[BLE_NAME_MAX_LEN];  // broadcast device name
  char    manufacturer[BLE_NAME_MAX_LEN];
  char    modelNumber[BLE_NAME_MAX_LEN];
  int8_t  txPower;                      // TX power in dBm
  bool    advertisingEnabled;           // false = stop advertising
};

// Active settings (loaded from EEPROM or defaults on startup)
extern BLESettings bleSettings;

// ============================================
// STATE FLAGS
// ============================================

extern bool hasNewData;

// ============================================
// CORE FUNCTIONS
// ============================================

/**
 * Initialise BLE. Loads settings from EEPROM first; applies defaults
 * if no valid settings are found.
 */
void bluetoothInit();

/**
 * Poll BLE events. Call every loop iteration.
 */
void bluetoothUpdate();

/**
 * Send a JSON document to the connected central via notification.
 */
void sendJsonData(const JsonDocument& jsonDoc);

/**
 * Send a plain text message wrapped in a simple JSON envelope.
 */
void sendMessage(const String& message);

/**
 * Returns true if a central is currently connected.
 */
bool isBluetoothConnected();

/**
 * Returns the last received data as a JsonDocument and clears the flag.
 */
JsonDocument getReceivedJson();

// ============================================
// SETTINGS MANAGEMENT
// ============================================

/**
 * Load BLE settings from EEPROM into bleSettings.
 * @return true if valid data was found, false if defaults were applied.
 */
bool bleLoadSettings();

/**
 * Save the current bleSettings struct to EEPROM.
 */
void bleSaveSettings();

/**
 * Reset bleSettings to factory defaults and save to EEPROM.
 */
void bleResetToDefaults();

/**
 * Apply bleSettings immediately: stop advertising, reconfigure BLE
 * with the new name / TX power / advertising flag, then restart.
 * Call this after changing any field in bleSettings.
 */
void bleApplySettings();

/**
 * Print current BLE settings to Serial (debugging).
 */
void blePrintSettings();

#endif // BLUETOOTH_H
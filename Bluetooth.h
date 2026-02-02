#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <ArduinoBLE.h>
#include <ArduinoJson.h>

// ============================================
// BLE SERVICE & CHARACTERISTIC UUIDS
// ============================================

// Main BLE Service UUID
#define BLE_SERVICE_UUID "180A"

// Characteristic UUIDs
#define DEVICE_INFO_CHAR_UUID "2A29"       // Manufacturer Name String
#define DEVICE_MODEL_CHAR_UUID "2A24"      // Model Number String
#define DATA_TX_CHAR_UUID "2A37"           // Data TX (notifications)
#define DATA_RX_CHAR_UUID "2A38"           // Data RX (write)

// ============================================
// BLE COMMUNICATION SETTINGS
// ============================================
#define BLE_LOCAL_NAME "URINE-TEST-001"
#define BLE_ADVERTISED_SERVICE_UUID "180A"
#define JSON_BUFFER_SIZE 256



extern bool hasNewData;

// ============================================
// FUNCTION DECLARATIONS
// ============================================

/**
 * Initialize BLE communication
 * Sets up services, characteristics, and starts advertising
 */
void bluetoothInit();

/**
 * Handle BLE events and process incoming data
 * Call this regularly in the main loop
 */
void bluetoothUpdate();

/**
 * Send JSON data via BLE
 * @param jsonDoc The JSON document to send
 */
void sendJsonData(const JsonDocument& jsonDoc);

/**
 * Send a simple text message via BLE
 * @param message The message to send
 */
void sendMessage(const String& message);

/**
 * Check if BLE is connected
 * @return true if connected, false otherwise
 */
bool isBluetoothConnected();

/**
 * Get the last received data as a JSON document
 * @return JsonDocument containing the received data
 */
JsonDocument getReceivedJson();

#endif // BLUETOOTH_H

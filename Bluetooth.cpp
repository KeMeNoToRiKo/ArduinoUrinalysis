#include "Bluetooth.h"

// ============================================
// GLOBAL BLE VARIABLES
// ============================================

BLEService deviceInfoService(BLE_SERVICE_UUID);

BLEStringCharacteristic manufacturerNameCharacteristic(DEVICE_INFO_CHAR_UUID, BLERead, 20);
BLEStringCharacteristic modelNumberCharacteristic(DEVICE_MODEL_CHAR_UUID, BLERead, 20);

// Data transmission characteristic (device -> phone)
BLEStringCharacteristic dataTxCharacteristic(DATA_TX_CHAR_UUID, BLENotify, JSON_BUFFER_SIZE);

// Data reception characteristic (phone -> device)
BLEStringCharacteristic dataRxCharacteristic(DATA_RX_CHAR_UUID, BLEWrite, JSON_BUFFER_SIZE);

// Last received JSON data
StaticJsonDocument<JSON_BUFFER_SIZE> lastReceivedJson;
bool hasNewData = false;

// ============================================
// FORWARD DECLARATIONS
// ============================================
void onDataReceived(BLEDevice central, BLECharacteristic characteristic);

// ============================================
// FUNCTION IMPLEMENTATIONS
// ============================================

void bluetoothInit() {
  // Start BLE communication
  if (!BLE.begin()) {
    Serial.println("[BLE] Failed to start BLE");
    while (1);
  }

  // Set BLE local name and advertised service
  BLE.setLocalName(BLE_LOCAL_NAME);
  BLE.setAdvertisedService(deviceInfoService);

  // Add characteristics to service
  deviceInfoService.addCharacteristic(manufacturerNameCharacteristic);
  deviceInfoService.addCharacteristic(modelNumberCharacteristic);
  deviceInfoService.addCharacteristic(dataTxCharacteristic);
  deviceInfoService.addCharacteristic(dataRxCharacteristic);

  // Set characteristic values
  manufacturerNameCharacteristic.writeValue("Arduino R4 WiFi");
  modelNumberCharacteristic.writeValue("URINE-TEST-001");

  // Add service
  BLE.addService(deviceInfoService);

  // Set event handler for data reception
  dataRxCharacteristic.setEventHandler(BLEWritten, onDataReceived);

  // Start advertising
  BLE.advertise();

  Serial.println("[BLE] Bluetooth initialized and advertising");
  Serial.print("[BLE] Local Name: ");
  Serial.println(BLE_LOCAL_NAME);
}

void bluetoothUpdate() {
  // Poll for BLE events
  BLEDevice central = BLE.central();

  if (central) {
    if (!central.connected()) {
      Serial.println("[BLE] Central disconnected");
      return;
    }

    // Connection active - data is handled by event handlers
  }
}

void sendJsonData(const JsonDocument& jsonDoc) {
  if (!isBluetoothConnected()) {
    Serial.println("[BLE] Not connected, cannot send JSON data");
    return;
  }

  // Serialize JSON to string
  String jsonString;
  serializeJson(jsonDoc, jsonString);

  // Check size limit
  if (jsonString.length() > JSON_BUFFER_SIZE) {
    Serial.println("[BLE] JSON data too large to send");
    return;
  }

  // Send via characteristic notification
  dataTxCharacteristic.writeValue(jsonString);
  Serial.print("[BLE] Sent JSON: ");
  Serial.println(jsonString);
}

void sendMessage(const String& message) {
  if (!isBluetoothConnected()) {
    Serial.println("[BLE] Not connected, cannot send message");
    return;
  }

  // Create a simple JSON wrapper for text messages
  StaticJsonDocument<JSON_BUFFER_SIZE> doc;
  doc["type"] = "message";
  doc["data"] = message;

  sendJsonData(doc);
}

bool isBluetoothConnected() {
  BLEDevice central = BLE.central();
  return central.connected();
}

JsonDocument getReceivedJson() {
  hasNewData = false;
  return lastReceivedJson;
}

// ============================================
// EVENT HANDLERS
// ============================================

void onDataReceived(BLEDevice central, BLECharacteristic characteristic) {
  String receivedData = dataRxCharacteristic.value();

  Serial.print("[BLE] Data received: ");
  Serial.println(receivedData);

  // Try to parse as JSON
  DeserializationError error = deserializeJson(lastReceivedJson, receivedData);

  if (error) {
    Serial.print("[BLE] Failed to parse JSON: ");
    Serial.println(error.c_str());

    // Try to create a simple message structure if it fails
    lastReceivedJson.clear();
    lastReceivedJson["type"] = "raw_message";
    lastReceivedJson["data"] = receivedData;
  } else {
    Serial.println("[BLE] JSON parsed successfully");
  }

  hasNewData = true;
}

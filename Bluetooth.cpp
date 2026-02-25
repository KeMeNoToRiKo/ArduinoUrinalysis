#include "Bluetooth.h"

// ============================================
// GLOBALS
// ============================================

BLESettings bleSettings;

BLEService deviceInfoService(BLE_SERVICE_UUID);

BLEStringCharacteristic manufacturerNameCharacteristic(DEVICE_INFO_CHAR_UUID,  BLERead,   BLE_NAME_MAX_LEN);
BLEStringCharacteristic modelNumberCharacteristic     (DEVICE_MODEL_CHAR_UUID, BLERead,   BLE_NAME_MAX_LEN);
BLEStringCharacteristic dataTxCharacteristic          (DATA_TX_CHAR_UUID,      BLENotify, JSON_BUFFER_SIZE);
BLEStringCharacteristic dataRxCharacteristic          (DATA_RX_CHAR_UUID,      BLEWrite,  JSON_BUFFER_SIZE);

StaticJsonDocument<JSON_BUFFER_SIZE> lastReceivedJson;
bool hasNewData = false;

// ============================================
// FORWARD DECLARATIONS
// ============================================

void onDataReceived(BLEDevice central, BLECharacteristic characteristic);

// ============================================
// SETTINGS MANAGEMENT
// ============================================

bool bleLoadSettings() {
  EEPROM.get(BLE_EEPROM_ADDR, bleSettings);
  if (bleSettings.magic != BLE_EEPROM_MAGIC) {
    bleResetToDefaults();
    return false;
  }
  return true;
}

void bleSaveSettings() {
  bleSettings.magic = BLE_EEPROM_MAGIC;
  EEPROM.put(BLE_EEPROM_ADDR, bleSettings);
  Serial.println("[BLE] Settings saved to EEPROM.");
  blePrintSettings();
}

void bleResetToDefaults() {
  bleSettings.magic = BLE_EEPROM_MAGIC;
  strncpy(bleSettings.localName,    BLE_DEFAULT_NAME,         BLE_NAME_MAX_LEN - 1);
  strncpy(bleSettings.manufacturer, BLE_DEFAULT_MANUFACTURER, BLE_NAME_MAX_LEN - 1);
  strncpy(bleSettings.modelNumber,  BLE_DEFAULT_MODEL,        BLE_NAME_MAX_LEN - 1);
  bleSettings.localName   [BLE_NAME_MAX_LEN - 1] = '\0';
  bleSettings.manufacturer[BLE_NAME_MAX_LEN - 1] = '\0';
  bleSettings.modelNumber [BLE_NAME_MAX_LEN - 1] = '\0';
  bleSettings.txPower           = BLE_DEFAULT_TX_POWER;
  bleSettings.advertisingEnabled = BLE_DEFAULT_ADVERTISING;
  EEPROM.put(BLE_EEPROM_ADDR, bleSettings);
  Serial.println("[BLE] Settings reset to defaults.");
}

void blePrintSettings() {
  Serial.println("[BLE] --- Current Settings ---");
  Serial.print  ("  Local name   : "); Serial.println(bleSettings.localName);
  Serial.print  ("  Manufacturer : "); Serial.println(bleSettings.manufacturer);
  Serial.print  ("  Model number : "); Serial.println(bleSettings.modelNumber);
  Serial.print  ("  TX power     : "); Serial.print(bleSettings.txPower); Serial.println(" dBm");
  Serial.print  ("  Advertising  : "); Serial.println(bleSettings.advertisingEnabled ? "ON" : "OFF");
  Serial.println("[BLE] ----------------------------");
}

// ============================================
// APPLY SETTINGS  (restarts BLE stack)
// ============================================

void bleApplySettings() {
  // Disconnect any active central gracefully
  BLEDevice central = BLE.central();
  if (central && central.connected()) {
    central.disconnect();
    delay(200);
  }

  BLE.stopAdvertise();
  BLE.end();
  delay(100);

  if (!BLE.begin()) {
    Serial.println("[BLE] Failed to restart BLE after settings change!");
    return;
  }

  BLE.setLocalName(bleSettings.localName);
  BLE.setAdvertisedService(deviceInfoService);

  // Rewrite characteristic values from new settings
  manufacturerNameCharacteristic.writeValue(bleSettings.manufacturer);
  modelNumberCharacteristic.writeValue(bleSettings.modelNumber);

  // TX power — ArduinoBLE exposes this via BLE.setAdvertisingInterval as a
  // workaround; direct power setting uses the underlying HCI command.
  // On Arduino R4 WiFi / ESP32-based boards this maps to BLE.setTxPower().
  // Uncomment the line below if your core supports it:
  // BLE.setTxPower(bleSettings.txPower);

  if (bleSettings.advertisingEnabled) {
    BLE.advertise();
    Serial.println("[BLE] Advertising restarted with new settings.");
  } else {
    Serial.println("[BLE] Advertising disabled by user setting.");
  }

  blePrintSettings();
}

// ============================================
// INIT
// ============================================

void bluetoothInit() {
  // Load persisted settings (falls back to defaults if EEPROM is blank)
  if (!bleLoadSettings()) {
    Serial.println("[BLE] No saved settings — defaults applied.");
  } else {
    Serial.println("[BLE] Settings loaded from EEPROM.");
  }
  blePrintSettings();

  if (!BLE.begin()) {
    Serial.println("[BLE] Failed to start BLE!");
    while (1);
  }

  BLE.setLocalName(bleSettings.localName);
  BLE.setAdvertisedService(deviceInfoService);

  deviceInfoService.addCharacteristic(manufacturerNameCharacteristic);
  deviceInfoService.addCharacteristic(modelNumberCharacteristic);
  deviceInfoService.addCharacteristic(dataTxCharacteristic);
  deviceInfoService.addCharacteristic(dataRxCharacteristic);

  manufacturerNameCharacteristic.writeValue(bleSettings.manufacturer);
  modelNumberCharacteristic.writeValue(bleSettings.modelNumber);

  BLE.addService(deviceInfoService);
  dataRxCharacteristic.setEventHandler(BLEWritten, onDataReceived);

  if (bleSettings.advertisingEnabled) {
    BLE.advertise();
    Serial.println("[BLE] Advertising started.");
  }
}

// ============================================
// UPDATE
// ============================================

void bluetoothUpdate() {
  BLEDevice central = BLE.central();
  if (central && !central.connected()) {
    Serial.println("[BLE] Central disconnected.");
  }
}

// ============================================
// SEND
// ============================================

void sendJsonData(const JsonDocument& jsonDoc) {
  if (!isBluetoothConnected()) {
    Serial.println("[BLE] Not connected — cannot send.");
    return;
  }
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  if (jsonString.length() > JSON_BUFFER_SIZE) {
    Serial.println("[BLE] Payload too large.");
    return;
  }
  dataTxCharacteristic.writeValue(jsonString);
  Serial.print("[BLE] Sent: "); Serial.println(jsonString);
}

void sendMessage(const String& message) {
  if (!isBluetoothConnected()) return;
  StaticJsonDocument<JSON_BUFFER_SIZE> doc;
  doc["type"] = "message";
  doc["data"] = message;
  sendJsonData(doc);
}

// ============================================
// STATUS
// ============================================

bool isBluetoothConnected() {
  BLEDevice central = BLE.central();
  return central && central.connected();
}

JsonDocument getReceivedJson() {
  hasNewData = false;
  return lastReceivedJson;
}

// ============================================
// EVENT HANDLER
// ============================================

void onDataReceived(BLEDevice central, BLECharacteristic characteristic) {
  String receivedData = dataRxCharacteristic.value();
  Serial.print("[BLE] Received: "); Serial.println(receivedData);

  DeserializationError error = deserializeJson(lastReceivedJson, receivedData);
  if (error) {
    lastReceivedJson.clear();
    lastReceivedJson["type"] = "raw_message";
    lastReceivedJson["data"] = receivedData;
  }
  hasNewData = true;
}
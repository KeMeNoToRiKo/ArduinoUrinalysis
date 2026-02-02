#include "Bluetooth.h"

bool bluetoothConnected = false;
unsigned long lastConnectionCheck = 0;
const unsigned long CONNECTION_CHECK_INTERVAL = 5000; // Check every 5s

void bluetoothInit() {
  Serial1.begin(9600); // HC-05 default baud
  Serial.println("Bluetooth initialized via Serial1");
}

bool isBluetoothConnected() {
  return bluetoothConnected;
}

void sendBluetoothData(String data) {
  if (bluetoothConnected) {
    Serial1.print(data);
  }
}

void sendBluetoothMessage(const char* message) {
  Serial1.println(message);
  Serial.print("BT Send: ");
  Serial.println(message);
}

String readBluetoothData() {
  String receivedData = "";
  while (Serial1.available()) {
    char c = Serial1.read();
    receivedData += c;
  }
  return receivedData;
}

void handleBluetooth() {
  if (Serial1.available()) {
    String incomingData = readBluetoothData();
    if (incomingData.length() > 0) {
      bluetoothConnected = true;
      lastConnectionCheck = millis();
      
      Serial.print("BT Received: ");
      Serial.println(incomingData);
      
      // Handle commands
      if (incomingData.indexOf("START") != -1) {
        sendBluetoothMessage("Test started");
      }
      else if (incomingData.indexOf("STOP") != -1) {
        sendBluetoothMessage("Test stopped");
      }
      else if (incomingData.indexOf("STATUS") != -1) {
        sendBluetoothMessage("Device ready");
      }
    }
  }

  // Check if connection lost
  if (millis() - lastConnectionCheck > CONNECTION_CHECK_INTERVAL) {
    if (bluetoothConnected) {
      bluetoothConnected = false;
      Serial.println("Bluetooth disconnected");
    }
    lastConnectionCheck = millis();
  }
}

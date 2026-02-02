#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <Arduino.h>

// Track connection status
extern bool bluetoothConnected;

// Initialize Bluetooth module
void bluetoothInit();

// Check if Bluetooth is connected
bool isBluetoothConnected();

// Send data via Bluetooth
void sendBluetoothData(String data);

// Send data with newline
void sendBluetoothMessage(const char* message);

// Read incoming Bluetooth data
String readBluetoothData();

// Handle Bluetooth communication
void handleBluetooth();

#endif

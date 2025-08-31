# ESP32 Presence Logger

**ESP32 Presence Logger** is a project for ESP32 that logs the presence of devices via BLE and Wi-Fi. It supports full string transmission over BLE with MTU negotiation, logging to SPIFFS, and basic device commands.

## Key Features

- Logs BLE devices (with hashed MAC addresses) and RSSI.
- Logs Wi-Fi networks (with hashed BSSID) and RSSI.
- Sends logs over BLE, split into multiple packets if necessary.
- BLE commands:
  - `DUMP` — sends the full log
  - `STATUS` — device status
  - `CLEAR` — clears the log
  - `TIME` — current time
  - `SET_TIME:YYYY-MM-DD HH:MM:SS` — set time
  - `SYNC_TIME` — sync time (currently no NTP)
- Automatic BLE scanning and periodic memory check.

## Libraries Used

- `WiFi.h`
- `SPIFFS.h`
- `BLEDevice.h`, `BLEServer.h`, `BLEUtils.h`, `BLE2902.h`, `BLEScan.h`
- `mbedtls/sha256.h` (for hashing MAC/BSSID)

## Setup and Upload

1. Install Arduino IDE or PlatformIO.
2. Add ESP32 support to Arduino IDE.
3. Install all required libraries (BLE, SPIFFS; mbedtls is included in ESP32 SDK).
4. Connect ESP32 and select the correct board and port.
5. Upload the `main.ino` sketch.
6. Use Serial Monitor for debugging and a BLE client for commands.


## Disclaimer / Legal Notice

This project is intended **for educational purposes only**. 

- It collects BLE and Wi-Fi device information **only for testing and logging in environments where you have permission**.  
- All MAC addresses and BSSIDs are **hashed to anonymize the data**.  
- The author is **not responsible** for any misuse of this code by third parties.  
- Do **not use this project to access, hack, or interfere with networks or devices without consent**, as it may be illegal in your country.  

Use responsibly and always respect privacy and local laws.


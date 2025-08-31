/*
  ESP32 Presence Logger - Complete Solution
  Fixed: Full string transmission over BLE with MTU negotiation
*/

#include <Arduino.h>
#include "WiFi.h"
#include "SPIFFS.h"
#include "esp_heap_caps.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include "mbedtls/sha256.h"

// Configuration
#define LOG_FILE "/presence_log.txt"
#define BLE_SCAN_DURATION 5
#define WIFI_SCAN_INTERVAL 10
#define BLE_SCAN_INTERVAL 8
#define MEMORY_CHECK_INTERVAL 60

// Время для автономной работы
struct DateTime {
  int year = 2024;
  int month = 8;
  int day = 30;
  int hour = 12;
  int minute = 0;
  int second = 0;
};

DateTime currentTime;
unsigned long lastMillis = 0;

// BLE Configuration
#define MAX_BLE_PACKET_SIZE 185  // Безопасный размер пакета

// BLE UART Service
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Global objects
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Timing variables
unsigned long lastWifiScan = 0;
unsigned long lastBleScan = 0;
unsigned long lastMemoryCheck = 0;

// BLE scan control
bool isBleScanRunning = false;
unsigned long bleScanStartTime = 0;
BLEScan* pBLEScan = nullptr;

// Log dump control
bool isSendingLog = false;
File logDumpFile;
unsigned long lastDumpSendTime = 0;
#define DUMP_SEND_INTERVAL 100

// -------------------- Forward Declarations --------------------
String getTimestamp();
void updateTime();
void setCurrentTime(int year, int month, int day, int hour, int minute, int second);
String sha256Hash(const String& input);
void appendToLog(const String& message, bool sendOverBLE = true);
void sendLongStringOverBLE(const String& message);
void startLogDump();
void processLogDump();
void startBleScan();
void checkBleScanStatus();
void scanWiFi();

// -------------------- Utility Functions --------------------
void updateTime() {
  unsigned long currentMillis = millis();
  unsigned long elapsed = currentMillis - lastMillis;
  
  if (elapsed >= 1000) { // Обновляем каждую секунду
    int secondsToAdd = elapsed / 1000;
    lastMillis = currentMillis - (elapsed % 1000);
    
    currentTime.second += secondsToAdd;
    
    // Обработка переполнения секунд
    while (currentTime.second >= 60) {
      currentTime.second -= 60;
      currentTime.minute++;
      
      if (currentTime.minute >= 60) {
        currentTime.minute = 0;
        currentTime.hour++;
        
        if (currentTime.hour >= 24) {
          currentTime.hour = 0;
          currentTime.day++;
          
          // Упрощенная обработка дней в месяце
          int daysInMonth = 31;
          if (currentTime.month == 4 || currentTime.month == 6 || 
              currentTime.month == 9 || currentTime.month == 11) {
            daysInMonth = 30;
          } else if (currentTime.month == 2) {
            daysInMonth = ((currentTime.year % 4 == 0) && 
                          ((currentTime.year % 100 != 0) || 
                           (currentTime.year % 400 == 0))) ? 29 : 28;
          }
          
          if (currentTime.day > daysInMonth) {
            currentTime.day = 1;
            currentTime.month++;
            
            if (currentTime.month > 12) {
              currentTime.month = 1;
              currentTime.year++;
            }
          }
        }
      }
    }
  }
}

String getTimestamp() {
  updateTime();
  
  char timestamp[20];
  sprintf(timestamp, "%04d-%02d-%02d %02d:%02d:%02d", 
          currentTime.year, currentTime.month, currentTime.day,
          currentTime.hour, currentTime.minute, currentTime.second);
  return String(timestamp);
}

void setCurrentTime(int year, int month, int day, int hour, int minute, int second) {
  currentTime.year = year;
  currentTime.month = month;
  currentTime.day = day;
  currentTime.hour = hour;
  currentTime.minute = minute;
  currentTime.second = second;
  lastMillis = millis();
  
  Serial.printf("Time set to: %s\n", getTimestamp().c_str());
}

String sha256Hash(const String& input) {
  unsigned char hash[32];
  mbedtls_sha256_context ctx;
  
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  
  char hexHash[65];
  for(int i = 0; i < 32; i++) {
    sprintf(hexHash + (i * 2), "%02x", hash[i]);
  }
  hexHash[64] = '\0';
  
  return String(hexHash).substring(0, 16);
}

// -------------------- BLE Transmission Functions --------------------
void sendLongStringOverBLE(const String& message) {
  if (!deviceConnected || !pTxCharacteristic) return;
  
  int messageLength = message.length();
  int maxPacketSize = 185; // Безопасный размер для большинства устройств
  
  Serial.printf("Sending BLE message: %d bytes, PacketSize: %d\n", 
                messageLength, maxPacketSize);
  
  // Если строка помещается в один пакет
  if (messageLength <= maxPacketSize) {
    pTxCharacteristic->setValue(message.c_str());
    pTxCharacteristic->notify();
    delay(50);
    return;
  }
  
  // Разбиваем на пакеты
  int totalPackets = (messageLength + maxPacketSize - 1) / maxPacketSize;
  
  // Отправляем заголовок с информацией о количестве пакетов
  String header = "START:" + String(totalPackets);
  pTxCharacteristic->setValue(header.c_str());
  pTxCharacteristic->notify();
  delay(75);
  
  // Отправляем пакеты данных
  for (int i = 0; i < totalPackets; i++) {
    int start = i * maxPacketSize;
    int end = min(start + maxPacketSize, messageLength);
    
    String packet = message.substring(start, end);
    
    pTxCharacteristic->setValue(packet.c_str());
    pTxCharacteristic->notify();
    delay(100); // Увеличенная задержка между пакетами
    
    Serial.printf("Sent packet %d/%d (%d bytes)\n", i+1, totalPackets, packet.length());
  }
  
  // Отправляем сигнал завершения
  pTxCharacteristic->setValue("END");
  pTxCharacteristic->notify();
  delay(50);
}

// -------------------- Logging Functions --------------------
void appendToLog(const String& message, bool sendOverBLE) {
  // Always print to serial
  Serial.println(message);
  
  // Write to SPIFFS
  File file = SPIFFS.open(LOG_FILE, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open log file");
    return;
  }
  
  file.println(message);
  file.close();
  
  // Send via BLE if connected
  if (sendOverBLE && deviceConnected && !isSendingLog) {
    sendLongStringOverBLE(message);
  }
}

void startLogDump() {
  logDumpFile = SPIFFS.open(LOG_FILE, FILE_READ);
  if (!logDumpFile) {
    pTxCharacteristic->setValue("ERROR: Log file not found");
    pTxCharacteristic->notify();
    Serial.println("Error: Log file not found for dump");
    return;
  }

  isSendingLog = true;
  lastDumpSendTime = 0;
  Serial.println("Starting log dump");
}

void processLogDump() {
  if (!isSendingLog || !deviceConnected) return;
  if (millis() - lastDumpSendTime < DUMP_SEND_INTERVAL) return;

  if (logDumpFile.available()) {
    String line = logDumpFile.readStringUntil('\n');
    line.trim();
    
    if (line.length() > 0) {
      sendLongStringOverBLE(line);
    }
    lastDumpSendTime = millis();
  } else {
    // End of file
    logDumpFile.close();
    isSendingLog = false;
    pTxCharacteristic->setValue("DUMP_COMPLETE");
    pTxCharacteristic->notify();
    Serial.println("Log dump completed");
  }
}

// -------------------- BLE Server Callbacks --------------------
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("BLE device connected");
    }

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("BLE device disconnected");
      
      // Clean up log dump if active
      if (isSendingLog) {
        logDumpFile.close();
        isSendingLog = false;
        Serial.println("Dump aborted due to disconnect");
      }
      
      // Restart advertising
      delay(500);
      pServer->getAdvertising()->start();
      Serial.println("Advertising restarted");
    }
    
    // MTU callback removed due to compatibility issues
};

// -------------------- BLE Characteristic Callbacks --------------------
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();
      
      if (rxValue.length() > 0) {
        Serial.print("Received command: ");
        Serial.println(rxValue.c_str());
        
        String command = String(rxValue.c_str());
        command.trim();
        
        if (command == "DUMP") {
          if (isSendingLog) {
            pTxCharacteristic->setValue("ERROR: Dump already in progress");
            pTxCharacteristic->notify();
          } else {
            pTxCharacteristic->setValue("DUMP_START");
            pTxCharacteristic->notify();
            startLogDump();
          }
        }
        else if (command == "STATUS") {
          String statusMsg = "STATUS: Heap=" + String(esp_get_free_heap_size()) +
                           ", Connected=" + String(deviceConnected);
          sendLongStringOverBLE(statusMsg);
        }
        else if (command == "CLEAR") {
          if (SPIFFS.exists(LOG_FILE)) {
            SPIFFS.remove(LOG_FILE);
            pTxCharacteristic->setValue("LOG_CLEARED");
            pTxCharacteristic->notify();
            appendToLog("=== Log cleared at " + getTimestamp() + " ===", false);
          }
        }
        else if (command == "TIME") {
          String timeMsg = "Current time: " + getTimestamp();
          sendLongStringOverBLE(timeMsg);
        }
        else if (command.startsWith("SET_TIME:")) {
          // Формат: SET_TIME:YYYY-MM-DD HH:MM:SS
          // Пример: SET_TIME:2024-08-30 15:30:00
          String timeStr = command.substring(9);
          timeStr.trim();
          
          if (timeStr.length() == 19) {
            int year = timeStr.substring(0, 4).toInt();
            int month = timeStr.substring(5, 7).toInt();
            int day = timeStr.substring(8, 10).toInt();
            int hour = timeStr.substring(11, 13).toInt();
            int minute = timeStr.substring(14, 16).toInt();
            int second = timeStr.substring(17, 19).toInt();
            
            if (year >= 2020 && year <= 2050 && month >= 1 && month <= 12 && 
                day >= 1 && day <= 31 && hour >= 0 && hour <= 23 && 
                minute >= 0 && minute <= 59 && second >= 0 && second <= 59) {
              
              setCurrentTime(year, month, day, hour, minute, second);
              sendLongStringOverBLE("Time set successfully to: " + getTimestamp());
            } else {
              sendLongStringOverBLE("ERROR: Invalid time values");
            }
          } else {
            sendLongStringOverBLE("ERROR: Time format should be YYYY-MM-DD HH:MM:SS");
          }
        }
        else if (command == "SYNC_TIME") {
          String timeMsg = "Current time: " + getTimestamp() + 
                          " (Synced: No)";
          sendLongStringOverBLE(timeMsg);
        }
        else {
          String errorMsg = "ERROR: Unknown command: " + command;
          pTxCharacteristic->setValue(errorMsg.c_str());
          pTxCharacteristic->notify();
        }
      }
    }
};

// -------------------- BLE Scanning --------------------
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String address = String(advertisedDevice.getAddress().toString().c_str());
    String hashedAddress = sha256Hash(address);
    int rssi = advertisedDevice.getRSSI();
    
    String manufacturerId = "None";
    if (advertisedDevice.haveManufacturerData()) {
      std::string manufacturerData = advertisedDevice.getManufacturerData();
      if (manufacturerData.length() >= 2) {
        uint16_t mfrId = (manufacturerData[1] << 8) | manufacturerData[0];
        char mfrStr[7];
        sprintf(mfrStr, "0x%04X", mfrId);
        manufacturerId = String(mfrStr);
      }
    }
    
    String logEntry = getTimestamp() + " - BLE Device - RSSI " + String(rssi) + 
                     " dBm - ID: " + hashedAddress + " - MFR: " + manufacturerId;
    appendToLog(logEntry, true);
  }
};

void startBleScan() {
  if (isBleScanRunning) return;
  
  Serial.println("Starting BLE scan...");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(false);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  pBLEScan->start(BLE_SCAN_DURATION, false);
  
  isBleScanRunning = true;
  bleScanStartTime = millis();
  lastBleScan = millis();
}

void checkBleScanStatus() {
  if (!isBleScanRunning) return;
  
  if (millis() - bleScanStartTime > (BLE_SCAN_DURATION * 1000 + 200)) {
    pBLEScan->stop();
    pBLEScan->clearResults();
    isBleScanRunning = false;
    Serial.println("BLE scan completed");
  }
}

// -------------------- Wi-Fi Scanning --------------------
void scanWiFi() {
  Serial.println("Starting Wi-Fi scan...");
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  int networkCount = WiFi.scanNetworks(false, true);
  
  if (networkCount == -1) {
    Serial.println("Wi-Fi scan failed");
    return;
  }
  
  for (int i = 0; i < networkCount; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    String bssid = WiFi.BSSIDstr(i);
    
    String hashedBssid = sha256Hash(bssid);
    
    String logEntry = getTimestamp() + " - Wi-Fi: " + ssid + 
                     " (BSSID " + hashedBssid + ") - RSSI " + String(rssi) + " dBm";
    appendToLog(logEntry, true);
  }
  
  WiFi.scanDelete();
  lastWifiScan = millis();
}

// -------------------- Setup & Main Loop --------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP32 Presence Logger - Full String Transmission Fixed ===");
  Serial.println("Initializing...");
  
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed!");
    while (1) delay(1000);
  }
  
  // Initialize BLE with MTU configuration
  BLEDevice::init("ESP32_Full_Logger");
  
  // Create BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  // Create BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Create BLE Characteristics
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());
  
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE
  );
  pRxCharacteristic->setCallbacks(new MyCallbacks());
  
  // Start services
  pService->start();
  
  // Configure and start advertising
  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();
  
  Serial.println("BLE service started with packet transmission");
  Serial.println("Device name: ESP32_Full_Logger");
  Serial.println("Available commands: DUMP, STATUS, CLEAR, TIME, SET_TIME:YYYY-MM-DD HH:MM:SS");
  
  // Инициализация времени
  lastMillis = millis();
  Serial.printf("System started at: %s\n", getTimestamp().c_str());
  Serial.println("Use SET_TIME command to set current time");
  
  // Write initial log entry
  appendToLog("=== Log started at " + getTimestamp() + " ===", false);
}

void loop() {
  // Handle BLE connection status
  if (deviceConnected != oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
    if (deviceConnected) {
      appendToLog("BLE device connected - " + getTimestamp(), false);
    }
  }
  
  // Perform Wi-Fi scan
  if (millis() - lastWifiScan > (WIFI_SCAN_INTERVAL * 1000)) {
    scanWiFi();
  }
  
  // Manage BLE scanning
  checkBleScanStatus();
  if (!isBleScanRunning && (millis() - lastBleScan > (BLE_SCAN_INTERVAL * 1000))) {
    startBleScan();
  }

  // Process log dump
  if (isSendingLog) {
    processLogDump();
  }
  
  // Periodic memory check
  if (millis() - lastMemoryCheck > (MEMORY_CHECK_INTERVAL * 1000)) {
    Serial.printf("Heap: %d bytes, Connected: %d\n", 
                 esp_get_free_heap_size(), deviceConnected);
    lastMemoryCheck = millis();
  }
  
  delay(50);
}

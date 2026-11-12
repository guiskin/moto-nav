#pragma once
#include "Arduino.h"
struct BLECharacteristic; struct BLEServer;
struct BLECharacteristicCallbacks { virtual void onWrite(BLECharacteristic*){} virtual ~BLECharacteristicCallbacks(){} };
struct BLECharacteristic { static const uint32_t PROPERTY_WRITE=1, PROPERTY_WRITE_NR=2, PROPERTY_READ=4, PROPERTY_NOTIFY=8; void setValue(const uint8_t*, size_t){} void notify(){} void setCallbacks(BLECharacteristicCallbacks*){} uint8_t* getData(){return nullptr;} size_t getLength(){return 0;} };
struct BLEService { BLECharacteristic* createCharacteristic(const char*, uint32_t){ return new BLECharacteristic(); } void start(){} };
struct ble_gap_conn_desc { uint16_t conn_handle; struct { bool encrypted; } sec_state; };
struct BLEServerCallbacks { virtual void onConnect(BLEServer*){} virtual void onDisconnect(BLEServer*){} virtual void onConnect(BLEServer*, ble_gap_conn_desc*){} virtual void onDisconnect(BLEServer*, ble_gap_conn_desc*){} virtual ~BLEServerCallbacks(){} };
struct BLEServer { void setCallbacks(BLEServerCallbacks*){} BLEService* createService(const char*){ return new BLEService(); } };
struct BLEAdvertising { void addServiceUUID(const char*){} void setScanResponse(bool){} };
struct BLEDevice { static void init(const char*){} static void setMTU(int){} static BLEServer* createServer(){ return new BLEServer(); } static BLEAdvertising* getAdvertising(){ static BLEAdvertising a; return &a; } static void startAdvertising(){} };

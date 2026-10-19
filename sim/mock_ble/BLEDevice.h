#pragma once
// Host stand-in for the ESP32 BLE library: records what the rod sends,
// lets the test act as the phone.
#include "Arduino.h"
#include <vector>
#include <string>
#include <map>

// Behave like NimBLE (ESP32-S3): writes say which connection wrote them,
// answers can go to one connection only.
#define CONFIG_NIMBLE_ENABLED 1
#define BLE_MOCK 1
struct ble_gap_conn_desc { uint16_t conn_handle; };

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))

class BLEServer;
class BLECharacteristic;

struct BLEServerCallbacks
{
  virtual ~BLEServerCallbacks() {}
  virtual void onConnect(BLEServer*) {}
  virtual void onDisconnect(BLEServer*) {}
};

struct BLECharacteristicCallbacks
{
  virtual ~BLECharacteristicCallbacks() {}
  virtual void onWrite(BLECharacteristic*) {}
  virtual void onWrite(BLECharacteristic* c, ble_gap_conn_desc*) { onWrite(c); }
};

struct BLEDescriptor { virtual ~BLEDescriptor() {} };
struct BLE2902 : BLEDescriptor {};

class BLECharacteristic
{
public:
  static const uint32_t PROPERTY_WRITE = 1, PROPERTY_WRITE_NR = 2, PROPERTY_NOTIFY = 4;
  BLECharacteristicCallbacks* cb = nullptr;
  std::string value;
  std::vector<std::string> notified;          // every notification the rod sent
  void setCallbacks(BLECharacteristicCallbacks* c) { cb = c; }
  void addDescriptor(BLEDescriptor*) {}
  void setValue(uint8_t* d, size_t n) { value.assign((const char*)d, n); }
  void notify() { notified.push_back(value); }
  String getValue() { return String(value); }
};

class BLEService
{
public:
  std::vector<BLECharacteristic*> chars;
  BLECharacteristic* createCharacteristic(const char*, uint32_t) { chars.push_back(new BLECharacteristic()); return chars.back(); }
  void start() {}
};

class BLEServer
{
public:
  BLEServerCallbacks* cb = nullptr;
  BLEService* service = nullptr;
  void setCallbacks(BLEServerCallbacks* c) { cb = c; }
  BLEService* createService(const char*) { service = new BLEService(); return service; }
};

struct BLEAdvertising
{
  int started = 0;
  void addServiceUUID(const char*) {}
  void setScanResponse(bool) {}
};

struct BLEDevice
{
  static BLEServer*& server() { static BLEServer* s = nullptr; return s; }
  static BLEAdvertising& adv() { static BLEAdvertising a; return a; }
  static void init(const char*) {}
  static void setMTU(int) {}
  static BLEServer* createServer() { server() = new BLEServer(); return server(); }
  static BLEAdvertising* getAdvertising() { return &adv(); }
  static void startAdvertising() { adv().started++; }
};

// ---- one connection at a time (what NimBLE gives the rod) ----
struct MockConns
{
  std::map<uint16_t, std::vector<std::string>> notified;   // per connection
  std::map<uint16_t, size_t> mtu;                          // ATT MTU per connection
  std::map<uint16_t, bool> down;                           // disconnected
  int refuseNext = 0;                                      // the stack is "full" this many times
};

inline MockConns& mockConns() { static MockConns m; return m; }

inline bool bleNotifyTo(BLECharacteristic* c, uint16_t conn, const uint8_t* data, size_t length)
{
  if(mockConns().refuseNext > 0) { mockConns().refuseNext--; return false; }
  std::string piece((const char*)data, length);
  mockConns().notified[conn].push_back(piece);
  c->notified.push_back(piece);                            // also the all-in-one log the test reads
  return true;
}

inline size_t bleMtu(uint16_t conn)
{
  return mockConns().mtu.count(conn) ? mockConns().mtu[conn] : 23;
}

inline bool bleConnected(uint16_t conn)
{
  return !mockConns().down[conn];
}

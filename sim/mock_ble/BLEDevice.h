#pragma once
// Host stand-in for the ESP32 BLE library: records what the rod sends,
// lets the test act as the phone.
#include "Arduino.h"
#include <vector>
#include <string>

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
};

struct BLEDescriptor { virtual ~BLEDescriptor() {} };
struct BLE2902 : BLEDescriptor {};

class BLECharacteristic
{
public:
  static const uint32_t PROPERTY_WRITE = 1, PROPERTY_WRITE_NR = 2, PROPERTY_NOTIFY = 4;
  static const uint32_t PROPERTY_WRITE_ENC = 0, PROPERTY_WRITE_AUTHEN = 0;   // (NimBLE only)
  BLECharacteristicCallbacks* cb = nullptr;
  std::string value;
  std::vector<std::string> notified;          // every notification the rod sent
  void setCallbacks(BLECharacteristicCallbacks* c) { cb = c; }
  void addDescriptor(BLEDescriptor*) {}
  void setValue(uint8_t* d, size_t n) { value.assign((const char*)d, n); }
  // A watch on its calm (power-saving) link takes its pieces only every
  // 110 ms, about 4 at a time. The Bluetooth chip holds only ~12 pieces
  // for it; when that is full, notify() drops the piece without a word
  // (like the real stack). slowLink = 0: no slow watch connected.
  static int& slowLink() { static int v = 0; return v; }
  static int& drops() { static int v = 0; return v; }
  int queued = 0;
  unsigned long drained = 0;
  void notify()
  {
    extern unsigned long simMillis;
    if(slowLink())
    {
      while(simMillis - drained >= 110) { drained += 110; queued = queued > 4 ? queued - 4 : 0; }
      if(queued >= 12) { drops()++; return; }
      queued++;
    }
    notified.push_back(value);
  }
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

struct BLEUUID { std::string u; BLEUUID(const char* s) : u(s) {} };

// what the rod puts in its radio signal
struct BLEAdvertisementData
{
  std::string name, service;
  void setFlags(uint8_t) {}
  void setName(String n) { name = n.c_str(); }
  void setCompleteServices(BLEUUID u) { service = u.u; }
};

struct BLEAdvertising
{
  int started = 0;
  BLEAdvertisementData adv, scan;
  void addServiceUUID(const char*) {}
  void setScanResponse(bool) {}
  void setAdvertisementData(BLEAdvertisementData& a) { adv = a; }
  void setScanResponseData(BLEAdvertisementData& a) { scan = a; }
};

// pairing with a PIN (the real stack does the encryption)
#define ESP_IO_CAP_OUT 0
struct BLESecurity
{
  static uint32_t& pin() { static uint32_t p = 0; return p; }
  static uint32_t setPassKey(bool, uint32_t p) { pin() = p; return p; }
  static void setCapability(uint8_t) {}
  static void setAuthenticationMode(bool, bool, bool) {}
};

struct BLEDevice
{
  static BLEServer*& server() { static BLEServer* s = nullptr; return s; }
  static BLEAdvertising& adv() { static BLEAdvertising a; return a; }
  static String& name() { static String n; return n; }
  static void init(String n) { name() = n; }
  static void setMTU(int) {}
  static BLEServer* createServer() { server() = new BLEServer(); return server(); }
  static BLEAdvertising* getAdvertising() { return &adv(); }
  static void startAdvertising() { adv().started++; }
};

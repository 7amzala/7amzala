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

// ---- Bluedroid's per-connection view (what the rod's "lines" use) ----
#define BLE_MOCK_LINES 1
typedef int esp_err_t;
#define ESP_OK 0
typedef uint8_t esp_gatt_if_t;
#define ESP_GATT_OK 0
#define ESP_GATT_CONGESTED 0x8f
#define ESP_GATT_INTERNAL_ERROR 0x81
enum esp_gatts_cb_event_t { ESP_GATTS_CONNECT_EVT, ESP_GATTS_DISCONNECT_EVT, ESP_GATTS_MTU_EVT, ESP_GATTS_CONGEST_EVT, ESP_GATTS_CONF_EVT };
union esp_ble_gatts_cb_param_t
{
  struct { uint16_t conn_id; } connect;
  struct { uint16_t conn_id; } disconnect;
  struct { uint16_t conn_id; } write;
  struct { uint16_t conn_id; uint16_t mtu; } mtu;
  struct { uint16_t conn_id; bool congested; } congest;
  struct { int status; uint16_t conn_id; uint16_t handle; } conf;
};
typedef void (*gatts_event_handler)(esp_gatts_cb_event_t, esp_gatt_if_t, esp_ble_gatts_cb_param_t*);

struct BLEServerCallbacks
{
  virtual ~BLEServerCallbacks() {}
  virtual void onConnect(BLEServer*) {}
  virtual void onDisconnect(BLEServer*) {}
  virtual void onConnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
  virtual void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
};

struct BLECharacteristicCallbacks
{
  virtual ~BLECharacteristicCallbacks() {}
  virtual void onWrite(BLECharacteristic*) {}
  virtual void onWrite(BLECharacteristic* c, esp_ble_gatts_cb_param_t*) { onWrite(c); }
};

// One phone / watch connection as the Bluetooth chip sees it: a small
// buffer (12 pieces) that the link empties at its own speed. A fast phone
// link takes ~6 pieces every 7.5 ms; a watch on a calm link 4 every 110 ms.
// Buffer nearly full: "congested"; full: the piece is thrown away and the
// stack reports it (ESP_GATTS_CONF_EVT with an error), like Bluedroid.
struct MockLink
{
  bool up = false;
  int mtu = 23;
  int perDrain = 6;
  int drainMs = 8;
  int queued = 0;
  unsigned long drainedAt = 0;
  bool congested = false;
  int drops = 0;
  size_t biggest = 0;
  std::vector<std::string> pieces;     // what reached the phone / watch
};
inline MockLink* mockLinks() { static MockLink l[4]; return l; }
inline gatts_event_handler& mockGattsHandler() { static gatts_event_handler h = nullptr; return h; }
inline void mockGattsFire(esp_gatts_cb_event_t e, esp_ble_gatts_cb_param_t* p) { if(mockGattsHandler()) mockGattsHandler()(e, 3, p); }

inline void mockDrain(uint16_t id)
{
  extern unsigned long simMillis;
  MockLink& L = mockLinks()[id];
  while(simMillis - L.drainedAt >= (unsigned long)L.drainMs) { L.drainedAt += L.drainMs; L.queued = L.queued > L.perDrain ? L.queued - L.perDrain : 0; }
  if(L.congested && L.queued <= 3)
  {
    L.congested = false;
    esp_ble_gatts_cb_param_t p; p.congest.conn_id = id; p.congest.congested = false;
    mockGattsFire(ESP_GATTS_CONGEST_EVT, &p);
  }
}

inline bool mockDeliver(uint16_t id, const uint8_t* d, size_t n)
{
  MockLink& L = mockLinks()[id];
  if(!L.up) return false;
  mockDrain(id);
  esp_ble_gatts_cb_param_t p; p.conf.conn_id = id; p.conf.handle = 42;
  if(L.queued >= 12)
  {
    L.drops++;
    p.conf.status = ESP_GATT_INTERNAL_ERROR;
    mockGattsFire(ESP_GATTS_CONF_EVT, &p);
    return false;
  }
  L.queued++;
  L.biggest = std::max(L.biggest, n);
  L.pieces.push_back(std::string((const char*)d, n));
  p.conf.status = L.queued >= 10 ? ESP_GATT_CONGESTED : ESP_GATT_OK;
  if(L.queued >= 10 && !L.congested)
  {
    L.congested = true;
    esp_ble_gatts_cb_param_t c; c.congest.conn_id = id; c.congest.congested = true;
    mockGattsFire(ESP_GATTS_CONGEST_EVT, &c);
  }
  mockGattsFire(ESP_GATTS_CONF_EVT, &p);
  return true;
}

inline bool& mockSendRefused() { static bool v = false; return v; }   // test: the direct send never works
inline bool& mockAlwaysFull() { static bool v = false; return v; }    // test: the stack always says "full"

inline esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t, uint16_t conn_id, uint16_t, uint16_t len, uint8_t* data, bool)
{
  if(mockSendRefused()) return 0x101;
  if(conn_id >= 4 || !mockLinks()[conn_id].up) return 0x103;   // ESP_ERR_INVALID_STATE
  if(len > mockLinks()[conn_id].mtu - 3) return 0x102;          // too big for this link
  mockDeliver(conn_id, data, len);
  return ESP_OK;
}

inline uint16_t esp_ble_get_cur_sendable_packets_num(uint16_t conn_id)
{
  if(conn_id >= 4 || !mockLinks()[conn_id].up || mockAlwaysFull()) return 0;
  mockDrain(conn_id);
  return 12 - mockLinks()[conn_id].queued;
}

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
  uint16_t getHandle() { return 42; }
  void notify()
  {
    for(int i = 0; i < 4; i++)             // (the real one: every subscriber)
      if(mockLinks()[i].up) mockDeliver(i, (const uint8_t*)value.data(), std::min(value.size(), (size_t)mockLinks()[i].mtu - 3));
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
  uint16_t getPeerMTU(uint16_t id) { return id < 4 && mockLinks()[id].up ? mockLinks()[id].mtu : 0; }
  void disconnect(uint16_t id) { mockDisconnect(id); }

  // the test: a phone / watch connects (like the library: both callbacks)
  void mockConnect(uint16_t id, int mtu, bool slow)
  {
    extern unsigned long simMillis;
    MockLink& L = mockLinks()[id];
    L = MockLink();
    L.up = true;
    L.mtu = 23;
    L.perDrain = slow ? 4 : 6;
    L.drainMs = slow ? 110 : 8;
    L.drainedAt = simMillis;
    esp_ble_gatts_cb_param_t p; p.connect.conn_id = id;
    mockGattsFire(ESP_GATTS_CONNECT_EVT, &p);
    cb->onConnect(this);
    cb->onConnect(this, &p);
    if(mtu > 23)                         // the MTU exchange follows
    {
      L.mtu = mtu;
      esp_ble_gatts_cb_param_t m; m.mtu.conn_id = id; m.mtu.mtu = mtu;
      mockGattsFire(ESP_GATTS_MTU_EVT, &m);
    }
  }
  void mockDisconnect(uint16_t id)
  {
    if(!mockLinks()[id].up) return;
    mockLinks()[id].up = false;
    esp_ble_gatts_cb_param_t p; p.disconnect.conn_id = id;
    mockGattsFire(ESP_GATTS_DISCONNECT_EVT, &p);
    cb->onDisconnect(this);
    cb->onDisconnect(this, &p);
  }
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
  static void setCustomGattsHandler(gatts_event_handler h) { mockGattsHandler() = h; }
};

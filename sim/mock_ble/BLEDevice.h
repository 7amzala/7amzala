#pragma once
// Host stand-in for the ESP32 BLE library: records what the rod sends,
// lets the test act as the phone.
#include "Arduino.h"
#include <vector>
#include <map>
#include <string>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))

class BLEServer;
class BLECharacteristic;

// Build with -DMOCK_NIMBLE to play the NimBLE library (newer ESP32 cores
// on the S3), else Bluedroid. The rod's "lines" have code for both.
#ifdef MOCK_NIMBLE
#define CONFIG_NIMBLE_ENABLED 1
#define BLE_MOCK_NIMBLE 1
struct ble_gap_conn_desc { uint16_t conn_handle; uint16_t conn_itvl; };
struct os_mbuf { std::string d; };
#define BLE_HS_ENOMEM 6
#define BLE_HS_EBUSY 15
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
inline int& mockBonds() { static int n = 0; return n; }
inline int ble_store_util_count(int, int* n) { *n = mockBonds(); return 0; }
inline int ble_store_clear() { mockBonds() = 0; return 0; }
#else
// ---- Bluedroid's per-connection view ----
#define BLE_MOCK_LINES 1
#endif
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
#ifdef MOCK_NIMBLE
  virtual void onConnect(BLEServer*, ble_gap_conn_desc*) {}
  virtual void onDisconnect(BLEServer*, ble_gap_conn_desc*) {}
#else
  virtual void onConnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
  virtual void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
#endif
};

struct BLECharacteristicCallbacks
{
  virtual ~BLECharacteristicCallbacks() {}
  virtual void onWrite(BLECharacteristic*) {}
#ifdef MOCK_NIMBLE
  virtual void onWrite(BLECharacteristic* c, ble_gap_conn_desc*) { onWrite(c); }
#else
  virtual void onWrite(BLECharacteristic* c, esp_ble_gatts_cb_param_t*) { onWrite(c); }
#endif
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
  int rssi = -58;                      // how strong the rod hears it
};
inline MockLink* mockLinks() { static MockLink l[8]; return l; }
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
  if(conn_id >= 8 || !mockLinks()[conn_id].up) return 0x103;   // ESP_ERR_INVALID_STATE
  if(len > mockLinks()[conn_id].mtu - 3) return 0x102;          // too big for this link
  mockDeliver(conn_id, data, len);
  return ESP_OK;
}

#ifdef MOCK_NIMBLE
// NimBLE: one pool of 20 buffers for all links; a piece waiting for its link
// holds one. Pool (or that link's queue) full: ENOMEM, nothing sent.
inline int mockPoolUsed()
{
  int n = 0;
  for(int i = 0; i < 8; i++) if(mockLinks()[i].up) { mockDrain(i); n += mockLinks()[i].queued; }
  return n;
}
inline os_mbuf* ble_hs_mbuf_from_flat(const void* d, uint16_t n)
{
  if(mockPoolUsed() >= 20) return nullptr;
  os_mbuf* m = new os_mbuf(); m->d.assign((const char*)d, n); return m;
}
inline int ble_gatts_notify_custom(uint16_t conn, uint16_t, os_mbuf* om)
{
  std::string d = om->d; delete om;
  if(mockSendRefused()) return 7;                                  // BLE_HS_ENOTCONN
  if(conn >= 8 || !mockLinks()[conn].up) return 7;
  if((int)d.size() > mockLinks()[conn].mtu - 3) return 3;         // BLE_HS_EINVAL
  mockDrain(conn);
  if(mockPoolUsed() >= 20 || mockLinks()[conn].queued >= 12) return BLE_HS_ENOMEM;
  mockDeliver(conn, (const uint8_t*)d.data(), d.size());
  return 0;
}
inline int ble_gap_conn_rssi(uint16_t conn, int8_t* r)
{
  if(conn >= 8 || !mockLinks()[conn].up) return 2;
  *r = (int8_t)mockLinks()[conn].rssi;
  return 0;
}
inline int ble_gap_conn_find(uint16_t conn, ble_gap_conn_desc* d)
{
  if(conn >= 8 || !mockLinks()[conn].up) return 2;
  d->conn_handle = conn;
  d->conn_itvl = (uint16_t)(mockLinks()[conn].drainMs * 4 / 5);   // x 1.25 ms
  return 0;
}
#endif

inline uint16_t esp_ble_get_cur_sendable_packets_num(uint16_t conn_id)
{
  if(conn_id >= 8 || !mockLinks()[conn_id].up || mockAlwaysFull()) return 0;
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
    for(int i = 0; i < 8; i++)             // (the real one: every subscriber)
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
  uint16_t getPeerMTU(uint16_t id) { return id < 8 && mockLinks()[id].up ? mockLinks()[id].mtu : 0; }
  uint32_t getConnectedCount() { uint32_t n = 0; for(int i = 0; i < 8; i++) if(mockLinks()[i].up) n++; return n; }
  std::map<uint16_t, int> getPeerDevices(bool) { std::map<uint16_t, int> m; for(int i = 0; i < 8; i++) if(mockLinks()[i].up) m[i] = 1; return m; }
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
#ifdef MOCK_NIMBLE
    ble_gap_conn_desc dsc = { id, 0 };
    cb->onConnect(this, &dsc);
#else
    cb->onConnect(this, &p);
#endif
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
#ifdef MOCK_NIMBLE
    ble_gap_conn_desc dsc = { id, 0 };
    cb->onDisconnect(this, &dsc);
#else
    cb->onDisconnect(this, &p);
#endif
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

// the test writes a request piece as phone / watch number "conn"
inline void mockWrite(BLECharacteristic* rx, BLECharacteristicCallbacks* cb, uint16_t conn)
{
#ifdef MOCK_NIMBLE
  ble_gap_conn_desc d = { conn, 0 };
  cb->onWrite(rx, &d);
#else
  esp_ble_gatts_cb_param_t p;
  p.write.conn_id = conn;
  cb->onWrite(rx, &p);
#endif
}

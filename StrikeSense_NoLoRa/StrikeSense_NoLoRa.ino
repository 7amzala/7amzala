#if defined(ESP32)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <esp_mac.h>
#if !defined(CONFIG_NIMBLE_ENABLED)
#include <BLE2902.h>
#endif
#else
#include <WebServer.h>        // host simulator
#endif
#include <Wire.h>
#include <Preferences.h>
#if defined(ESP32) && __has_include("esp_app_desc.h")
#include "esp_app_desc.h"
#endif
#include <LittleFS.h>
#include <esp_task_wdt.h>

// (defined further down, before the battery code)
void startWatchdog();
const char* resetReason();
void addWatchChanMode(String& json);
#include <SparkFunLIS3DH.h>

// Sensor: ADXL355 (EVAL-ADXL355-PMDZ, I2C) or LIS3DH, found automatically.
// ADXL355 wiring: pin 1 SCL -> D5, pin 2 SDA -> D4, pin 5 GND, pin 6 3V3,
// on the sensor link pin 3 -> 12 and pin 4 -> 11.
LIS3DH myIMU(I2C_MODE, 0x19);
// ---------- Private Bluetooth: rod ID + PIN, pairing window ----------

String rodId = "ROD-0000";
uint32_t rodPin = 0;
const unsigned long PAIR_WINDOW_MS = 180000;     // new phone / watch: first 3 min after switching on

// (defined further down, with the other Bluetooth pages)
int bondedDevices();
bool pairWindowOpen();
void privacyBegin();
void forgetPairedDevices();

#if defined(CONFIG_BLUEDROID_ENABLED)
// A new phone or watch asks to pair: only while pairing is open, with the PIN
class PairCallbacks : public BLESecurityCallbacks
{
  uint32_t onPassKeyRequest() override { return rodPin; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return pairWindowOpen(); }
  bool onConfirmPIN(uint32_t) override { return pairWindowOpen(); }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t d) override
  {
    Serial.printf("Bluetooth: pairing %s\n", d.success ? "OK (encrypted)" : "refused or wrong PIN");
  }
};
#endif

// =====================================================
// BLUETOOTH LINK TO THE PHONE APP
//
// Same requests as the old web page (/data, /set?k=..&v=.. ...), so the
// handlers below did not change. Uses the ESP32's own BLE library.
// =====================================================

#if defined(ESP32)

#define BLE_SERVICE_UUID "6e8a0001-5f2d-4c8e-9a51-2b7c1d3e4f60"
#define BLE_RX_UUID      "6e8a0002-5f2d-4c8e-9a51-2b7c1d3e4f60"   // phone -> rod
#define BLE_TX_UUID      "6e8a0003-5f2d-4c8e-9a51-2b7c1d3e4f60"   // rod -> phone

static portMUX_TYPE bleLock = portMUX_INITIALIZER_UNLOCKED;   // phone writes vs loop()

// One line per phone / watch: each gets only its own answers, in pieces of
// its own size, at the speed its own link takes them. Before, every answer
// went to both, in pieces as small as the smaller MTU, as slow as the slower
// link: the phone's live graph lagged and the watch waited for answers
// behind the phone's. Works with both Bluetooth libraries of the ESP32 core:
// Bluedroid (BLE_BD) and NimBLE (newer cores on the ESP32-S3).
#if defined(CONFIG_BLUEDROID_ENABLED) || defined(BLE_MOCK_LINES)
#define BLE_LINES 1
#define BLE_BD 1
#include <esp_gatt_common_api.h>        // esp_ble_get_cur_sendable_packets_num
#elif defined(CONFIG_NIMBLE_ENABLED)
#define BLE_LINES 1
#define BLE_BD 0
#include "host/ble_hs.h"                // ble_gatts_notify_custom, ble_gap_conn_find
#include "host/ble_store.h"             // paired devices (bondedDevices, forgetPairedDevices)
#else
#define BLE_LINES 0
#define BLE_BD 0
#endif

#if defined(ESP32) && !defined(BLE_MOCK_LINES) && !defined(BLE_MOCK_NIMBLE)
#if BLE_BD
#pragma message("StrikeSense Bluetooth: Bluedroid - own line for each device")
#elif BLE_LINES
#pragma message("StrikeSense Bluetooth: NimBLE - own line for each device")
#else
#warning "StrikeSense Bluetooth: unknown library - one shared line for phone + watch"
#endif
#endif

class BleServer
{
public:
  typedef void (*Handler)();

  typedef String (*LiveBuilder)();

  // The live message: built and sent to everyone every LIVE_MS
  void onLive(LiveBuilder b)
  {
    liveBuilder = b;
  }

  // Something happened (strike, knock): send the live message now, not at
  // the next 100 ms tick, so the phone and the watch hear it at once
  void pushLive()
  {
    lastLive = millis() - LIVE_MS;
  }

  void on(const char* path, Handler h)
  {
    if(routeCount < MAX_ROUTES)
    {
      routes[routeCount].path = path;
      routes[routeCount].handler = h;
      routeCount++;
    }
    else
    {
      Serial.printf("Bluetooth: TOO MANY REQUESTS - %s not added (make MAX_ROUTES bigger)\n", path);
    }
  }

  void begin()
  {
    privacyBegin();

    BLEDevice::init(rodId);
    BLEDevice::setMTU(517);

#if defined(CONFIG_BLUEDROID_ENABLED)
    BLEDevice::setSecurityCallbacks(new PairCallbacks());
#endif
    BLESecurity::setPassKey(true, rodPin);
    BLESecurity::setCapability(ESP_IO_CAP_OUT);              // the PIN is on the box: the phone / watch types it
    BLESecurity::setAuthenticationMode(true, true, true);    // bond, PIN protected, Secure Connections

    srv = BLEDevice::createServer();
    srv->setCallbacks(new Callbacks(this));

#if BLE_BD
    me = this;
    BLEDevice::setCustomGattsHandler(gattsEvent);            // congestion + which pieces got lost
#endif

    BLEService* service = srv->createService(BLE_SERVICE_UUID);

    BLECharacteristic* rx = service->createCharacteristic(
      BLE_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR |
      BLECharacteristic::PROPERTY_WRITE_ENC | BLECharacteristic::PROPERTY_WRITE_AUTHEN);   // (NimBLE)
    rx->setCallbacks(new RxCallbacks(this));

#if defined(CONFIG_BLUEDROID_ENABLED)
    rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);  // only a paired device can ask anything
#endif

    tx = service->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);

#if !defined(CONFIG_NIMBLE_ENABLED)
    BLE2902* cccd = new BLE2902();       // older ESP32 boards (Bluedroid); NimBLE adds it by itself
#if defined(CONFIG_BLUEDROID_ENABLED)
    cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);   // only a paired device gets the data
#endif
    tx->addDescriptor(cccd);
#endif

    service->start();

    advertiseFor(pairWindowOpen());
    BLEDevice::startAdvertising();

    Serial.println(broadcast ? "Bluetooth: one shared line for phone + watch"
                             : "Bluetooth: own line for the phone and for the watch");
  }

  // Pairing open: "ROD-4F2A" + the StrikeSense service, so the app and the
  // watch can list it. Locked: no name, no service (paired devices find
  // the rod by its address).
  void advertiseFor(bool pairing)
  {
    advPairing = pairing;

    BLEAdvertisementData adv, scan;
    adv.setFlags(0x06);

    if(pairing)
    {
      adv.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));
      scan.setName(rodId);
    }

    BLEAdvertising* a = BLEDevice::getAdvertising();
    a->setAdvertisementData(adv);
    a->setScanResponseData(scan);
    a->setScanResponse(true);

    Serial.printf("Bluetooth: %s\n", pairing ? "pairing OPEN - the rod shows its ID" : "pairing locked - the rod is hidden");
  }

  // A device that connects but never pairs (writes nothing) within 30 s is sent away
  void kickStrangers()
  {
#if defined(CONFIG_BLUEDROID_ENABLED)
    for(int i = 0; i < MAX_LINES; i++)
    {
      Line& L = lines[i];

      if(L.used && !L.talked && millis() - L.since > 30000)
      {
        Serial.printf("Bluetooth: device %d never paired - sent away\n", L.id);
        L.used = false;
        srv->disconnect(L.id);
      }
    }
#endif
  }

  bool connected() const
  {
    return clients > 0;
  }

  // Every 2 s: the lines against the connections the Bluetooth library really
  // has. A line left over from a device that went away without a word is
  // freed (else after a few comings and goings the phone got no line: it was
  // connected but stuck on LEARNING until the rod restarted), the count of
  // devices is put right, and a line that cannot send is started again.
  void checkLines(unsigned long nowMs)
  {
#if BLE_LINES
    // sending to all after failures: a minute later, the direct way again
    // (at the first moment no message to all is on its way: it must end
    // whole - see handleClient; checking only here, every 2 s, could land
    // in the middle of a message every time)
    if(broadcast && nowMs - broadcastSince > 60000)
      directDue = true;

    int real = (int)srv->getConnectedCount();

    if(real != clients)
    {
      Serial.printf("Bluetooth: %d devices really connected (counted %d) - put right\n", real, (int)clients);
      clients = real;
      advertiseAgain = true;
    }

    for(int i = 0; i < MAX_LINES; i++)
    {
      Line& L = lines[i];

      if(!L.used)
        continue;

      if(!connAlive(L.id))
      {
        Serial.printf("Bluetooth: device %d is gone - its line freed\n", L.id);
        portENTER_CRITICAL(&bleLock);
        L.used = false;
        portEXIT_CRITICAL(&bleLock);
        continue;
      }

      // something to send, but nothing went out for 3 s: start its pace again
      if(L.outPos < L.out.length() && nowMs - L.sentAt > 3000)
      {
        Serial.printf("Bluetooth: device %d's line stuck - started again\n", L.id);
        L.gapUs = PIECE_GAP_US;
        L.congested = false;
        L.blockedSince = 0;
        L.ignoreFullUntil = nowMs + 10000;
        L.sentAt = nowMs;
      }
    }

    if(lineHealed)
    {
      lineHealed = false;
      Serial.println("Bluetooth: a connected device had no line - it has one now");
    }
#endif
  }

  bool sharedIdle()
  {
    for(int i = 0; i < MAX_LINES; i++)
      if(lines[i].used && lines[i].outPos > 0 && lines[i].outPos < lines[i].out.length())
        return false;

    return true;
  }

  bool connAlive(uint16_t id)
  {
#if BLE_LINES && !BLE_BD
    ble_gap_conn_desc d;
    return ble_gap_conn_find(id, &d) == 0;
#elif BLE_LINES
    return srv->getPeerDevices(false).count(id) > 0;
#else
    return true;
#endif
  }

  // Called from loop(): each line gets its waiting question answered, then
  // the live message, and its answer sent in pieces at its own pace
  void handleClient()
  {
    // Keep advertising while there is room for another phone/watch
    if(advertiseAgain || millis() - lastAdvertiseCheck >= 5000)
    {
      kickStrangers();

      if(pairWindowOpen() != advPairing)
        advertiseFor(pairWindowOpen());

      if(!advertiseAgain)
        Serial.printf("Bluetooth: %s, %d connected%s\n",
                      clients < MAX_CLIENTS ? "advertising" : "full (not advertising)", (int)clients,
                      dropsSaid != drops ? " (some pieces lost: sending slower)" : "");

      dropsSaid = drops;
      advertiseAgain = false;
      lastAdvertiseCheck = millis();

      if(clients < MAX_CLIENTS)
        BLEDevice::startAdvertising();
    }

    unsigned long nowMs = millis();

    if(nowMs - checkedAt >= 2000)
    {
      checkedAt = nowMs;
      checkLines(nowMs);
    }

#if BLE_LINES && !BLE_BD
    if(dropConn >= 0 && (long)(millis() - dropAt) >= 0)
    {
      ble_gap_terminate((uint16_t)dropConn, BLE_ERR_REM_USER_CONN_TERM);
      ble_store_util_delete_peer(&dropAddr);
      Serial.println("Bluetooth: a device this rod does not accept was sent away (pairing deleted)");
      dropConn = -1;
    }
#endif

    if(directDue && sharedIdle())
    {
      directDue = false;

      if(broadcast)
      {
        broadcast = false;
        Serial.println("Bluetooth: own line for each device again");
      }
    }

    // a line just opened, or closed: start it clean (here, not in the Bluetooth task)
    for(int i = 0; i < MAX_LINES; i++)
    {
      Line& L = lines[i];

      if(L.fresh || (!L.used && L.out.length() > 0))
      {
        L.fresh = false;
        L.out = "";
        L.outPos = 0;
      }
    }

    // the live message, 10 times a second: every line gets the latest one
    if(clients > 0 && liveBuilder != nullptr && nowMs - lastLive >= LIVE_MS)
    {
      lastLive = nowMs;
      String body = liveBuilder();
      live = "#live 200 " + String((unsigned long)body.length()) + "\n" + body;

      for(int i = 0; i < MAX_LINES; i++)
        lines[i].liveDue = lines[i].used;

      if(broadcast)                     // one copy for everyone
        for(int i = 0; i < MAX_LINES; i++)
          lines[i].liveDue = lines[i].used && i == firstUsed();
    }

    // one question answered per call (the handlers take the time), lines take turns
    bool answered = false;

    for(int k = 0; k < MAX_LINES; k++)
    {
      int i = (turn + k) % MAX_LINES;
      Line& L = lines[i];

      if(!L.used || L.outPos < L.out.length())
        continue;

      L.out = "";
      L.outPos = 0;

      char line[REQ_MAX];

      if(!answered && popLine(i, line))
      {
        answered = true;
        turn = (i + 1) % MAX_LINES;
        replyLine = i;
        dispatch(line);
        replyLine = -1;
      }
      else if(L.liveDue)
      {
        L.liveDue = false;
        L.out = live;
      }
    }

    // pieces: each line at its own pace (taking turns: sending to all, only
    // one piece goes at a time and the first line must not always win)
    for(int k = 0; k < MAX_LINES; k++)
    {
      int i = (sendTurn + k) % MAX_LINES;

      if(sendPiece(lines[i], nowMs))
        sendTurn = (i + 1) % MAX_LINES;
    }
  }

  bool hasArg(const char* name)
  {
    for(int i = 0; i < argCount; i++)
      if(argNames[i] == name)
        return true;

    return false;
  }

  // the connection of the device whose question is being answered (-1 = none)
  int askerConn()
  {
    return replyLine >= 0 ? (int)lines[replyLine].id : -1;
  }

  // The asking device's own address (NimBLE: the same after every
  // reconnect, even with a changing radio address). "" = not known.
  String askerAddr()
  {
#if BLE_LINES && !BLE_BD
    ble_gap_conn_desc d;
    int id = askerConn();

    if(id >= 0 && ble_gap_conn_find((uint16_t)id, &d) == 0)
    {
      char a[18];
      const uint8_t* v = d.peer_id_addr.val;
      snprintf(a, sizeof(a), "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
      return String(a);
    }
#endif
    return "";
  }

  // Send the asking device away once its answer is out: its pairing is
  // deleted and its link closed (a second phone, refused)
  void dropAsker()
  {
#if BLE_LINES && !BLE_BD
    ble_gap_conn_desc d;
    int id = askerConn();

    if(id >= 0 && ble_gap_conn_find((uint16_t)id, &d) == 0)
    {
      dropAddr = d.peer_id_addr;
      dropConn = id;
      dropAt = millis() + 1500;
    }
#endif
  }

  // that Bluetooth connection is still up (-1: no)
  bool connUp(int id)
  {
#if BLE_LINES
    return id >= 0 && connAlive((uint16_t)id);
#else
    return false;
#endif
  }

  // how strong the rod hears a connection, dBm (0 = unknown). The watch
  // says how it hears the rod; when it cannot (0), this is used instead.
  int connRssi(int id)
  {
#if BLE_LINES && !BLE_BD
    int8_t r = 0;

    if(id >= 0 && ble_gap_conn_rssi((uint16_t)id, &r) == 0)
      return r;
#else
    (void)id;
#endif
    return 0;
  }

  String arg(const char* name)
  {
    for(int i = 0; i < argCount; i++)
    {
      if(argNames[i] == name)
        return argValues[i];
    }

    return "";
  }

  void sendHeader(const char*, const char*)
  {
  }

  void send(int code, const char* type, const String& body)
  {
    reply(code, body.c_str(), body.length());
  }

  void send(int code, const char* type, const char* body)
  {
    reply(code, body, strlen(body));
  }

  void send_P(int code, const char* type, const char* body)
  {
    reply(code, body, strlen(body));
  }

  void send_P(int code, const char* type, const char* body, size_t length)
  {
    reply(code, body, length);
  }

private:
  static const int MAX_ROUTES = 64;     // (40 was full: the last requests were silently not known)
  static const int MAX_ARGS = 6;
  static const int REQ_QUEUE = 6;           // per line
  static const int REQ_MAX = 200;
  static constexpr unsigned long PIECE_GAP_US = 4000;      // fastest: a piece every 4 ms per line
  static constexpr unsigned long PIECE_GAP_MAX_US = 40000; // slowest, after lost pieces
  static const unsigned long LIVE_MS = 100;            // live message 10 times a second

  struct Route
  {
    const char* path;
    Handler handler;
  };

  Route routes[MAX_ROUTES];
  int routeCount = 0;

  static const int MAX_CLIENTS = 2;         // phone + watch
  bool advPairing = true;

  // one line per connected phone / watch
  static const int MAX_LINES = 4;

  struct Line
  {
    bool used;
    bool fresh;                 // just opened: loop() clears what was left
    bool talked;                // wrote something = paired (only a paired device can write)
    uint16_t id;                // the Bluetooth connection
    unsigned long since;

    // questions from this device, in order
    char queue[REQ_QUEUE][REQ_MAX];
    int qHead, qTail;
    char partial[REQ_MAX];
    int partialLen;

    // what is being sent to it
    String out;
    size_t outPos;
    bool liveDue;
    size_t mtuSaid;             // its /mtu message (0 = not yet)

    // its pace
    unsigned long lastPiece;    // micros()
    unsigned long gapUs;        // grows when pieces get lost, shrinks again when all is well
    unsigned long calmSince;    // millis() of the last lost piece
    volatile bool congested;    // the Bluetooth stack says: full, wait
    unsigned long blockedSince; // millis(), 0 = not waiting
    unsigned long ignoreFullUntil; // the stack's "full" got stuck: pace by time only until then (millis)
    unsigned long itvlUs;       // NimBLE: the time its link needs per piece (0 = not known)
    unsigned long itvlAt;       // millis() when that was looked up
    unsigned long sentAt;       // millis() of its last piece (or when it opened)
  };

  Line lines[MAX_LINES] = {};
  int turn = 0;
  int sendTurn = 0;
  int replyLine = -1;           // the line the answer being built is for

  BLEServer* srv = nullptr;
  BLECharacteristic* tx = nullptr;
  volatile int clients = 0;
  volatile bool advertiseAgain = false;
  String live;
  unsigned long lastLive = 0;
  LiveBuilder liveBuilder = nullptr;
  unsigned long lastAdvertiseCheck = 0;
  String requestId = "0";

  // sending to one device failed again and again: send to all (the old way)
  bool broadcast = !BLE_LINES;
  int sendFails = 0;
  volatile unsigned long drops = 0;
  unsigned long dropsSaid = 0;
  unsigned long lastShared = 0;   // micros() of the last piece to anyone (sending to all)
  int sharedOwner = -1;           // sending to all: the line whose message is on its way
  volatile bool lineHealed = false; // a device wrote without a line: it got one
  unsigned long broadcastSince = 0; // when sending to all started (after failures)
  bool directDue = false;               // a minute is over: own lines again when nothing is half sent
  int dropConn = -1;                    // a device to send away (after its answer)
  unsigned long dropAt = 0;
#if BLE_LINES && !BLE_BD
  ble_addr_t dropAddr = {};
#endif
  unsigned long checkedAt = 0;      // the last look at which connections really exist
  uint16_t gattsIf = 0xFFFF;

  String argNames[MAX_ARGS];
  String argValues[MAX_ARGS];
  int argCount = 0;

  int firstUsed()
  {
    for(int i = 0; i < MAX_LINES; i++)
      if(lines[i].used)
        return i;

    return -1;
  }

  Line* lineFor(uint16_t id)
  {
    for(int i = 0; i < MAX_LINES; i++)
      if(lines[i].used && lines[i].id == id)
        return &lines[i];

    return nullptr;
  }

  // (Bluetooth task) a phone / watch connected or left
  void lineOpened(uint16_t id)
  {
    portENTER_CRITICAL(&bleLock);
    openLine(id);
    portEXIT_CRITICAL(&bleLock);
  }

  // (the lock held) a line for this connection; false = no room
  Line* openLine(uint16_t id)
  {
    Line* had = lineFor(id);

    if(had != nullptr)
      return had;                       // (already there)

    for(int i = 0; i < MAX_LINES; i++)
    {
      Line& L = lines[i];

      if(!L.used)
      {
        L.used = true;
        L.fresh = true;
        L.talked = false;
        L.id = id;
        L.since = millis();
        L.qHead = L.qTail = 0;
        L.partialLen = 0;
        L.liveDue = false;
        L.mtuSaid = 0;
        L.lastPiece = 0;
        L.gapUs = PIECE_GAP_US;
        L.calmSince = millis();
        L.congested = false;
        L.blockedSince = 0;
        L.ignoreFullUntil = 0;
        L.itvlUs = 0;
        L.itvlAt = 0;
        L.sentAt = millis();
        return &L;
      }
    }

    return nullptr;
  }

  void lineClosed(uint16_t id)
  {
    portENTER_CRITICAL(&bleLock);

    for(int i = 0; i < MAX_LINES; i++)
      if(lines[i].used && lines[i].id == id)
        lines[i].used = false;

    portEXIT_CRITICAL(&bleLock);
  }

  // Biggest piece for this line: its own MTU (the stack knows it), else
  // what it said in /mtu; everyone's smallest when sending to all
  size_t pieceSize(Line& L)
  {
    if(!broadcast)
      return ownPiece(L);

    size_t m = 512;

    for(int i = 0; i < MAX_LINES; i++)
      if(lines[i].used && ownPiece(lines[i]) < m)
        m = ownPiece(lines[i]);

    return m;
  }

  size_t ownPiece(Line& L)
  {
    size_t m = 0;

#if BLE_LINES
    uint16_t mtu = srv->getPeerMTU(L.id);

    if(mtu > 23)
      m = mtu - 3;
#endif

    if(m == 0)
      m = L.mtuSaid > 0 ? L.mtuSaid : 20;

    return m > 512 ? 512 : m;
  }

  // true = a piece went out
  bool sendPiece(Line& L, unsigned long nowMs)
  {
    if(!L.used || L.outPos >= L.out.length())
    {
      L.blockedSince = 0;
      return false;
    }

#if BLE_LINES && !BLE_BD
    // NimBLE has no "this link is full" signal, and one pool of buffers for
    // all links: no faster than the link takes them, 3 pieces per connection
    // event (the phone's 15 ms: one every 5 ms; a watch on its calm 110 ms
    // link: one every 37 ms), so a slow link never fills the pool for the others
    if(nowMs - L.itvlAt >= 1000)
    {
      ble_gap_conn_desc d;

      if(ble_gap_conn_find(L.id, &d) == 0)
        L.itvlUs = (unsigned long)d.conn_itvl * 1250UL / 3;

      L.itvlAt = nowMs;
    }
#endif

    // sending to all: one pace for everyone, the slowest line's
    unsigned long gap = L.gapUs > L.itvlUs ? L.gapUs : L.itvlUs;

    if(broadcast)
      for(int i = 0; i < MAX_LINES; i++)
        if(lines[i].used)
        {
          if(lines[i].gapUs > gap) gap = lines[i].gapUs;
          if(lines[i].itvlUs > gap) gap = lines[i].itvlUs;
        }

    if(micros() - (broadcast ? lastShared : L.lastPiece) < gap)
      return false;

    // sending to all: everyone gets every piece, so one message at a time
    // (pieces of two messages mixed would scramble both)
    int me_i = (int)(&L - lines);

    if(broadcast && sharedOwner >= 0 && sharedOwner != me_i && lines[sharedOwner].used &&
       lines[sharedOwner].outPos > 0 && lines[sharedOwner].outPos < lines[sharedOwner].out.length())
      return false;

    // all well for 2 s: a bit faster again
    if(L.gapUs > PIECE_GAP_US && nowMs - L.calmSince > 2000)
    {
      L.gapUs = L.gapUs / 2 > PIECE_GAP_US ? L.gapUs / 2 : PIECE_GAP_US;
      L.calmSince = nowMs;
    }

#if BLE_BD
    // its link is full: wait (else the stack throws the piece away). If the
    // stack seems stuck saying "full" (500 ms), stop believing it for this line.
    // Sending to all: every piece goes to every device, so wait for the fullest.
    Line* stuck = nullptr;

    for(int i = 0; i < MAX_LINES; i++)
    {
      Line& o = lines[i];

      if(!o.used || (long)(nowMs - o.ignoreFullUntil) < 0 || (!broadcast && &o != &L))
        continue;

      if(o.congested || esp_ble_get_cur_sendable_packets_num(o.id) == 0)
        stuck = &o;
    }

    if(stuck != nullptr)
    {
      if(L.blockedSince == 0)
        L.blockedSince = nowMs;

      if(nowMs - L.blockedSince < 500)
        return false;

      stuck->ignoreFullUntil = nowMs + 10000;    // try believing it again in 10 s
      Serial.printf("Bluetooth: device %d stuck 'full' - sending by time only for 10 s\n", stuck->id);
    }

    L.blockedSince = 0;
#endif

    size_t n = L.out.length() - L.outPos;
    size_t most = pieceSize(L);

    if(n > most)
      n = most;

    const uint8_t* data = (const uint8_t*)L.out.c_str() + L.outPos;

    if(broadcast)
    {
      tx->setValue((uint8_t*)data, n);
      tx->notify();
    }
#if BLE_BD
    else
    {
      esp_err_t rc = esp_ble_gatts_send_indicate(gattsIf, L.id, tx->getHandle(), n, (uint8_t*)data, false);

      if(rc != ESP_OK)
      {
        sendFailed(L, (int)rc);
        return false;                  // the same piece again next time
      }

      sendFails = 0;
    }
#elif BLE_LINES
    else
    {
      os_mbuf* om = ble_hs_mbuf_from_flat(data, n);
      int rc = om != nullptr ? ble_gatts_notify_custom(L.id, tx->getHandle(), om) : BLE_HS_ENOMEM;

      if(rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY)
      {
        // the stack's memory is full (a slow link holds it): the same piece
        // again soon, this line a bit slower
        L.gapUs = L.gapUs * 2 < PIECE_GAP_MAX_US ? L.gapUs * 2 : PIECE_GAP_MAX_US;
        L.calmSince = nowMs;
        L.lastPiece = micros();
        return false;
      }

      if(rc != 0)
      {
        sendFailed(L, rc);
        return false;                  // the same piece again next time
      }

      sendFails = 0;
    }
#endif

    L.outPos += n;
    L.lastPiece = micros();
    L.sentAt = nowMs;
    lastShared = L.lastPiece;
    sharedOwner = me_i;

    if(L.outPos >= L.out.length())
    {
      L.out = "";
      L.outPos = 0;
    }

    return true;
  }

  // sending to one device failed. The device is gone: free its line (that
  // is not the direct sending failing). Else after 3 in a row: send to all
  // (the old way) for a minute, then try the direct way again.
  void sendFailed(Line& L, int rc)
  {
    L.lastPiece = micros();

    if(!connAlive(L.id))
    {
      Serial.printf("Bluetooth: device %d is gone - its line freed\n", L.id);
      portENTER_CRITICAL(&bleLock);
      L.used = false;
      portEXIT_CRITICAL(&bleLock);
      return;
    }

    Serial.printf("Bluetooth: sending to device %d failed (error %d)\n", L.id, rc);

    if(++sendFails >= 3)
    {
      broadcast = true;
      broadcastSince = millis();
      directDue = false;
      sendFails = 0;
      Serial.println("Bluetooth: sending to all devices for a minute");
    }
  }

#if BLE_BD
  static BleServer* me;

  // (Bluetooth task) the stack's own news about each connection
  static void gattsEvent(esp_gatts_cb_event_t e, esp_gatt_if_t gif, esp_ble_gatts_cb_param_t* p)
  {
    BleServer* s = me;

    if(s == nullptr)
      return;

    s->gattsIf = gif;

    if(e == ESP_GATTS_CONGEST_EVT)
    {
      Line* L = s->lineFor(p->congest.conn_id);

      if(L != nullptr)
        L->congested = p->congest.congested;
    }
    else if(e == ESP_GATTS_CONF_EVT && p->conf.status != ESP_GATT_OK && p->conf.status != ESP_GATT_CONGESTED)
    {
      // a piece was thrown away: that answer arrives broken (the phone / watch
      // asks again); this line sends slower from now on
      Line* L = s->lineFor(p->conf.conn_id);

      if(L != nullptr)
      {
        L->gapUs = L->gapUs * 2 < PIECE_GAP_MAX_US ? L->gapUs * 2 : PIECE_GAP_MAX_US;
        L->calmSince = millis();
      }

      s->drops = s->drops + 1;
    }
  }
#endif

  class Callbacks : public BLEServerCallbacks
  {
  public:
    Callbacks(BleServer* s) : owner(s) {}

    void onConnect(BLEServer*) override
    {
      owner->clients = owner->clients + 1;
      owner->advertiseAgain = true;
      Serial.printf("Bluetooth: connected (%d)\n", (int)owner->clients);

#if !BLE_LINES
      if(owner->clients == 1)
        owner->lineOpened(0);          // (no connection numbers: one shared line for all)
#endif
    }

    void onDisconnect(BLEServer*) override
    {
      if(owner->clients > 0)
        owner->clients = owner->clients - 1;

      owner->advertiseAgain = true;
      Serial.printf("Bluetooth: disconnected (%d left)\n", (int)owner->clients);

#if !BLE_LINES
      if(owner->clients == 0)
        owner->lineClosed(0);
#endif
    }

#if BLE_BD
    // (the library calls both onConnect versions)
    void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override
    {
      owner->lineOpened(p->connect.conn_id);
    }

    void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override
    {
      owner->lineClosed(p->disconnect.conn_id);
    }
#elif BLE_LINES
    void onConnect(BLEServer*, ble_gap_conn_desc* d) override
    {
      owner->lineOpened(d->conn_handle);
    }

    void onDisconnect(BLEServer*, ble_gap_conn_desc* d) override
    {
      owner->lineClosed(d->conn_handle);
    }
#endif

  private:
    BleServer* owner;
  };

  class RxCallbacks : public BLECharacteristicCallbacks
  {
  public:
    RxCallbacks(BleServer* s) : owner(s) {}

    void onWrite(BLECharacteristic* c) override
    {
      write(c, 0);
    }

#if BLE_BD
    // only a paired (encrypted) device can write: this one is ours
    void onWrite(BLECharacteristic* c, esp_ble_gatts_cb_param_t* p) override
    {
      write(c, p->write.conn_id);
    }
#elif BLE_LINES
    void onWrite(BLECharacteristic* c, ble_gap_conn_desc* d) override
    {
      write(c, d != nullptr ? d->conn_handle : 0);
    }
#endif

  private:
    BleServer* owner;

    void write(BLECharacteristic* c, uint16_t id)
    {
      auto v = c->getValue();

      for(size_t i = 0; i < v.length(); i++)
        owner->received(v[i], id);
    }
  };

  void received(char ch, uint16_t id)
  {
    portENTER_CRITICAL(&bleLock);

    Line* L = lineFor(id);

#if BLE_LINES
    if(L == nullptr)
    {
      L = openLine(id);                 // it wrote, so it is connected: give it a line
      lineHealed = true;
    }
#endif

    if(L != nullptr)
    {
      L->talked = true;

      if(ch == '\n')
      {
        int next = (L->qHead + 1) % REQ_QUEUE;

        if(next != L->qTail)
        {
          memcpy(L->queue[L->qHead], L->partial, L->partialLen);
          L->queue[L->qHead][L->partialLen] = 0;
          L->qHead = next;
        }

        L->partialLen = 0;
      }
      else if(ch != '\r' && L->partialLen < REQ_MAX - 1)
      {
        L->partial[L->partialLen++] = ch;
      }
    }

    portEXIT_CRITICAL(&bleLock);
  }

  bool popLine(int i, char* line)
  {
    bool got = false;
    Line& L = lines[i];

    portENTER_CRITICAL(&bleLock);

    if(L.used && L.qTail != L.qHead)
    {
      memcpy(line, L.queue[L.qTail], REQ_MAX);
      L.qTail = (L.qTail + 1) % REQ_QUEUE;
      got = true;
    }

    portEXIT_CRITICAL(&bleLock);

    return got;
  }

  static String decode(const String& t)
  {
    String r;

    for(size_t i = 0; i < t.length(); i++)
    {
      char c = t[i];

      if(c == '+')
      {
        r += ' ';
      }
      else if(c == '%' && i + 2 < t.length())
      {
        char hex[3] = { t[i + 1], t[i + 2], 0 };
        r += (char)strtol(hex, nullptr, 16);
        i += 2;
      }
      else
      {
        r += c;
      }
    }

    return r;
  }

  // "<id>|/path?a=1&b=2"
  void dispatch(const char* line)
  {
    String l(line);
    int bar = l.indexOf('|');

    if(bar < 0)
      return;

    requestId = l.substring(0, bar);

    String url = l.substring(bar + 1);
    int q = url.indexOf('?');
    String path = q >= 0 ? url.substring(0, q) : url;
    String query = q >= 0 ? url.substring(q + 1) : "";

    argCount = 0;

    while(query.length() > 0 && argCount < MAX_ARGS)
    {
      int amp = query.indexOf('&');
      String pair = amp >= 0 ? query.substring(0, amp) : query;
      query = amp >= 0 ? query.substring(amp + 1) : "";

      int eq = pair.indexOf('=');
      argNames[argCount] = decode(eq >= 0 ? pair.substring(0, eq) : pair);
      argValues[argCount] = decode(eq >= 0 ? pair.substring(eq + 1) : "");
      argCount++;
    }

    // A phone/watch says how big one Bluetooth piece may be for it
    if(path == "/mtu")
    {
      int v = arg("v").toInt();
      v = v < 20 ? 20 : (v > 512 ? 512 : v);

      if(replyLine >= 0)
        lines[replyLine].mtuSaid = v;

      send(200, "text/plain", "OK");
      return;
    }

    for(int i = 0; i < routeCount; i++)
    {
      if(path == routes[i].path)
      {
        routes[i].handler();
        return;
      }
    }

    Serial.printf("Bluetooth: unknown request %s\n", path.c_str());
    send(404, "text/plain", "NOT FOUND");
  }

  // the answer goes to the line that asked
  void reply(int code, const char* body, size_t length)
  {
    if(replyLine < 0 || !lines[replyLine].used)
      return;

    String& out = lines[replyLine].out;
    out = "#" + requestId + " " + String(code) + " " + String((unsigned long)length) + "\n";
    out.concat(body, length);
    lines[replyLine].outPos = 0;
  }
};

#if BLE_BD
BleServer* BleServer::me = nullptr;
#endif

BleServer server;

#else

WebServer server(80);     // host simulator

#endif
Preferences prefs;

// Battery: BAT+ -> 100k -> D1 -> 100k -> GND (the XIAO has no battery
// sensing of its own). Nothing wired = "no battery" on the dashboard.
const int BAT_PIN = D1;
const unsigned long BAT_READ_MS = 2000;

float batVolts = 0;                     // smoothed, 0 = no reading
int batPercent = -1;                    // -1 = no battery seen
unsigned long lastBatRead = 0;
bool batLowSaid = false;


// =====================================================
// TUNING
//
// All the numbers you may want to change are here.
// See TUNING.md for which number fixes which problem.
// =====================================================

// ---------- Sampling ----------

const unsigned long SAMPLE_MS = 10;      // 100 processed readings per second
const float DT = 0.010;                  // seconds per reading
const int SENSOR_RATE = 400;             // Hz, sensor output rate
// Sensor cable up to ~3 m: 100 kHz (400 kHz only works with short wires).
// With a 3 m cable also fit 2.2k pull-ups: SDA -> 3V3 and SCL -> 3V3 at the box.
const uint32_t SENSOR_I2C_HZ = 100000;

const unsigned long OVERSAMPLE_US = 2500; // read every 2.5 ms, average ~4 per step (less noise)

// ---------- Signal filters ----------

const float GRAVITY_TAU = 0.30;          // s, rod angle smoothing
const float FAST_TILT_TAU = 0.10;        // s, quicker angle for the watch ring (display only)
const float VIBRATION_TAU = 0.04;        // s, only shaking faster than ~4 Hz counts as vibration
const float ENERGY_TAU = 0.08;           // s, knock energy smoothing (smoother noise, knocks last 200+ ms)

// ---------- Environment ----------

const unsigned long LEARN_TIME = 15000;  // quick start learning (covers 1-2 waves)
const float NOISE_TAU = 30.0;            // s, background vibration tracking
const float GUST_NOISE_TAU = 3.0;        // s, fast vibration level (wind gusts)
const float GUST_NOISE_MUL = 1.0;        // limit rises this much with a gust (1 = same ratio as the gust)
const float WAVE_TAU = 20.0;             // s, wave size tracking
const float WIND_TAU = 20.0;             // s, sideways sway tracking
const float BASELINE_TAU = 60.0;         // s, rest position follows tide
const unsigned long SEA_STATE_HOLD = 30000; // auto sea state must hold this long to change

// ---------- Knock detector ----------

const float KNOCK_FLOOR_LIS3DH = 0.010;  // g, threshold never goes below this (LIS3DH)
const float KNOCK_FLOOR_ADXL355 = 0.006; // g, same for the ~10x quieter ADXL355
float KNOCK_FLOOR = KNOCK_FLOOR_LIS3DH;  // set in setup() for the sensor found
const float KNOCK_CEIL = 0.250;          // g, threshold never goes above this
const float KNOCK_RELEASE = 0.50;        // knock ends halfway between normal noise and the limit
const int KNOCK_CONFIRM = 3;             // readings in a row above threshold (30 ms)
const unsigned long KNOCK_MAX_MS = 800;  // longer than this = wind / handling
const unsigned long KNOCK_GAP_MS = 150;  // dead time after each knock
// Repeated-bite count/window, sharp level, pull angle/time, drop-back
// and knock beep are LIVE SETTINGS (dashboard -> SETTINGS). The
// defaults are in DEFAULT_SETTINGS below.
const unsigned long SHARP_PULL_WINDOW_MS = 600;
const float SHARP_PULL_MIN_DEG = 1.0;    // plus: faster than a wave can move the tip

// ---------- Slow pull / drop-back bucket ----------

const float BUCKET_MIN = 0.80;           // deg*s, smallest bucket size
const float DEFAULT_WAVE_PERIOD = 6.0;   // s, used until the real period is measured
const float WAVE_FILL_TAU = 300.0;       // s, how long a big wave is remembered
const float SUSTAIN_PERIOD_PART = 0.60;  // bucket must stay filled this part of a wave period (a wave can't)
const float GUST_SIDE_MIN = 0.50;        // deg, sideways move that counts as a gust
const float GUST_FACTOR = 0.40;          // bucket fills slower during a gust

// ---------- Settling ----------

const float SETTLE_TOL_MIN = 0.60;       // deg, allowed wobble while settling
const unsigned long SETTLE_TIME_MIN = 1500;
const unsigned long SETTLE_TIME_MAX = 10000;
const float SETTLE_PULL_MIN = 1.50;      // deg moved in 1 s while settling
const unsigned long SETTLE_PULL_TIME = 700;
const unsigned long READY_GRACE_MS = 5000; // ignore knocks after READY (handling)
const unsigned long SETTLE_MAX_WAIT = 8000; // never settle longer than this (after grace)
const float RECENT_REST_TAU = 2.0;       // s, average position used if settling times out
const unsigned long FAST_FOLLOW_MS = 15000; // after a timed-out settle, follow the rod faster
const float FAST_FOLLOW_TAU = 6.0;       // s

// ---------- Pull test ----------

const float PULLTEST_MIN_DEG = 1.0;      // plus 1.5 x the usual wave bend, so a wave can't pass the test
const unsigned long PULLTEST_CONFIRM_MS = 150;
const unsigned long PULLTEST_SETTLE_MS = 1500; // finish this long after the biggest bend

// ---------- Condition labels ----------

const float WAVE_SMALL = 0.30;           // deg swing
const float WAVE_MEDIUM = 1.00;
const float WAVE_BIG = 2.50;

const float WIND_NORMAL = 0.30;          // deg sideways swing
const float WIND_STRONG = 1.00;
const float WIND_VSTRONG = 2.50;

const float WINDVIB_NORMAL = 0.005;      // g vibration ABOVE the sensor's own noise
const float WINDVIB_STRONG = 0.010;
const float WINDVIB_VSTRONG = 0.020;
const float SENSOR_FLOOR_LIS3DH = 0.015; // g, sensor's own noise until measured
const float SENSOR_FLOOR_ADXL355 = 0.002;
float SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_LIS3DH; // set in setup() for the sensor found
const bool FORGET_SAVED_NOISE = false;   // set true once after changing the sensor board

const float TIDE_SLOW = 0.30;            // deg per minute
const float TIDE_FAST = 1.50;

// ---------- Sea state profiles ----------

struct SeaProfile
{
  float knockDev;        // knock threshold = noise + knockDev * spread
  float allowMul;        // pull allowance = allowMul * wave swing
  float bucketMul;       // bucket = bucketMul x the most the waves filled it
  float holdMul;         // pull time setting x this (rougher sea = a bit longer)
  float noDirAllowMul;   // allowance when the pull direction is not known
  float sharpAlone;      // knock this strong = sharp strike on its own
  float sharpWithPull;   // knock this strong + fast tip pull = sharp strike
};

const SeaProfile SEA_PROFILES[3] =
{
  // CALM
  { 6.0, 0.50, 2.0, 1.0, 1.0, 6.0, 2.5 },

  // MODERATE
  { 7.0, 0.60, 2.2, 1.2, 1.2, 6.5, 2.5 },

  // ROUGH
  { 8.0, 0.70, 2.5, 1.5, 1.5, 7.0, 3.0 }
};

const char* SEA_NAMES[3] = { "calm", "mod", "rough" };

// ---------- Live settings (changed from the dashboard) ----------

struct UserSettings
{
  float knockMul;        // knock limit x this (below 1 = more sensitive)
  float pullDeg;         // deg, tip must lean at least this far to count as a pull
  float pullTime;        // s, a pull must last at least this long
  float repeatCount;     // knocks for a REPEATED BITE
  float repeatWindow;    // s, within this time
  float sharpG;          // knock energy (g) that is always a SHARP STRIKE (~3x = hit size)
  float dropOn;          // 1 = drop-back alarm on
  float knockBeep;       // 1 = single knock beep on
  float pullMul;         // slow pull: allowed lean and bucket x this (above 1 = less sensitive)
};

const UserSettings DEFAULT_SETTINGS =
{
  1.0,    // knockMul
  1.0,    // pullDeg
  2.0,    // pullTime
  3,      // repeatCount
  4.0,    // repeatWindow
  0.08,   // sharpG
  1,      // dropOn
  1,      // knockBeep
  1.0     // pullMul
};

struct SettingInfo
{
  const char* key;
  float UserSettings::* field;
  float low;
  float high;
};

const SettingInfo SETTING_INFO[] =
{
  { "kmul",  &UserSettings::knockMul,     0.5,  4.0 },
  { "pdeg",  &UserSettings::pullDeg,      0.4,  5.0 },
  { "ptime", &UserSettings::pullTime,     0.5,  8.0 },
  { "rcnt",  &UserSettings::repeatCount,  2,    5 },
  { "rwin",  &UserSettings::repeatWindow, 2.0,  8.0 },
  { "sharp", &UserSettings::sharpG,       0.03, 0.30 },
  { "drop",  &UserSettings::dropOn,       0,    1 },
  { "kbeep", &UserSettings::knockBeep,    0,    1 },
  { "pmul",  &UserSettings::pullMul,      0.8,  3.0 }
};

const int SETTING_COUNT = sizeof(SETTING_INFO) / sizeof(SETTING_INFO[0]);

UserSettings settings = DEFAULT_SETTINGS;
UserSettings savedSettings = DEFAULT_SETTINGS;

// =====================================================
// VECTOR HELPERS
// =====================================================

const float RAD_TO_DEGREES = 57.29578;

struct Vec3
{
  float x;
  float y;
  float z;
};

Vec3 vec(float x, float y, float z)
{
  Vec3 v = { x, y, z };
  return v;
}

Vec3 vAdd(Vec3 a, Vec3 b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 vSub(Vec3 a, Vec3 b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 vScale(Vec3 a, float s) { return vec(a.x * s, a.y * s, a.z * s); }
float vDot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float vLen(Vec3 a) { return sqrt(vDot(a, a)); }

Vec3 vCross(Vec3 a, Vec3 b)
{
  return vec(
    a.y * b.z - a.z * b.y,
    a.z * b.x - a.x * b.z,
    a.x * b.y - a.y * b.x
  );
}

Vec3 vNorm(Vec3 a)
{
  float n = vLen(a);

  if(n < 0.000001)
    return vec(0, 0, 1);

  return vScale(a, 1.0 / n);
}

// Part of a that is perpendicular to unit vector n
Vec3 vPerp(Vec3 a, Vec3 n)
{
  return vSub(a, vScale(n, vDot(a, n)));
}

float vAngleDeg(Vec3 a, Vec3 b)
{
  float c = vDot(a, b);

  if(c > 1) c = 1;
  if(c < -1) c = -1;

  return acos(c) * RAD_TO_DEGREES;
}

float clampFloat(float value, float low, float high)
{
  if(value < low) return low;
  if(value > high) return high;

  return value;
}

// Exponential smoothing factor for a time constant
float alphaFor(float tau)
{
  return DT / (tau + DT);
}

// =====================================================
// STATES
// =====================================================

enum RodState
{
  ST_LEARNING,
  ST_PULLTEST,
  ST_MONITORING,
  ST_SETTLING,
  ST_PAUSED
};

RodState rodState = ST_LEARNING;

// Running time: since power on, since armed, and all-time total
unsigned long armedSince = 0;            // 0 = not armed
unsigned long totalSecondsSaved = 0;     // all-time seconds saved in flash
unsigned long totalSavedAtMs = 0;        // millis() when it was loaded/saved
const unsigned long TOTAL_SAVE_MS = 600000; // save every 10 min (flash wears out)

bool strikeActive = false;

unsigned long lastSampleTime = 0;

// =====================================================
// SIGNALS
// =====================================================

Vec3 rawAccel = { 0, 0, 1 };
Vec3 gravity = { 0, 0, 1 };      // smoothed accel = where the tip points
Vec3 slowPart = { 0, 0, 1 };     // everything slower than ~4 Hz
Vec3 gravityUnit = { 0, 0, 1 };

float energySq = 0;
float knockEnergy = 0;           // fast vibration strength (g)

float tiltDeg = 0;               // signed: + toward water, - away
Vec3 fastGravity = { 0, 0, 1 };
float fastTiltDeg = 0;           // same, less smoothing: the watch ring follows the tip
float sideDeg = 0;               // sideways (wind)
float totalDeg = 0;              // total angle from rest

float energyPeakSincePoll = 0;
float energyPeakSinceWatch = 0;          // same, for the watch (its own poll)

// =====================================================
// REST POSITION + PULL DIRECTION
// =====================================================

Vec3 restDir = { 0, 0, 1 };
Vec3 pullDir = { 1, 0, 0 };      // unit, perpendicular to restDir
Vec3 sideDir = { 0, 1, 0 };

bool haveDir = false;

Vec3 storedRest = { 0, 0, 0 };   // rest position at that pull test (sensor frame)
bool haveStoredRest = false;
Vec3 storedDir = { 0, 0, 0 };    // last pull test result, sensor frame
bool haveStoredDir = false;

unsigned long dirLearnedAt = 0;

// =====================================================
// ENVIRONMENT
// =====================================================

float knockMean = 0.005;
float knockSpread = 0.002;

// The sensor's own noise (quietest vibration level ever seen),
// saved so that a still sensor always reads "Calm"
float sensorFloor = SENSOR_FLOOR_DEFAULT;
bool haveSensorFloor = false;
float savedSensorFloor = 0;
unsigned long sensorFloorSavedAt = 0;
float windVib = 0;               // vibration above the sensor's own noise
float knockThreshold = KNOCK_FLOOR;

float gustNoise = 0.005;         // vibration level over the last few seconds

float waveAmp = 0;               // average |tilt|
float waveSwing = 0;             // typical peak swing (deg)
float wavePeriod = 0;            // seconds, 0 = unknown

float sideAmp = 0;
float windSwing = 0;

float tideRate = 0;              // deg per minute
Vec3 tidePrevRest = { 0, 0, 1 };
unsigned long tideCheckTime = 0;

// wave period from upward zero crossings
bool waveBelow = false;
unsigned long lastUpCross = 0;
unsigned long lastWaveCrossSeen = 0;

int waveLevel = 0;
int windLevel = 0;
int tideLevel = 0;

int seaState = 1;                // 0 calm, 1 moderate, 2 rough
bool seaAuto = true;

int seaCandidate = 1;
unsigned long seaCandidateSince = 0;

// =====================================================
// ENVIRONMENT LEARNING
// =====================================================

unsigned long learningStart = 0;

const int LEARN_MAX = LEARN_TIME / SAMPLE_MS + 20;

float learnEnergy[LEARN_MAX];
Vec3 learnDir[LEARN_MAX];
int learnCount = 0;

Vec3 learnSum = { 0, 0, 0 };

// =====================================================
// KNOCK DETECTOR
// =====================================================

bool knockActive = false;
int knockAbove = 0;

unsigned long knockStart = 0;
unsigned long lastKnockEnd = 0;
unsigned long lastValidKnock = 0;

float knockPeak = 0;
float knockTiltAtStart = 0;

float lastKnockStrength = 0;

unsigned long knockTimes[8];
int knockTimeIndex = 0;

unsigned long knockAlertID = 0;

bool sharpPending = false;

bool gustNoted = false;                 // this long shake was already reported as wind
unsigned long gustIgnoredAt = 0;        // last strong gust ignored (dashboard)
unsigned long sharpPendingStart = 0;
float sharpPendingTilt = 0;

// =====================================================
// SLOW PULL / DROP-BACK
// =====================================================

float pullAllow = 1.0;
float bucketSize = BUCKET_MIN;

float pullBucket = 0;
float dropBucket = 0;

// Drop-back watch: a drop-back alone is normal (waves loosen the braid).
// Back and then forward past the normal position is the fish pattern.
const unsigned long DROP_WATCH_MS = 120000;   // watch this long for a pull after a drop-back
const unsigned long DROP_PULL_HOLD_MS = 500;  // the pull forward must hold this long

bool dropWatch = false;
void noteDropBack(bool watch);
unsigned long dropSeenAt = 0;                 // last drop-back seen (0 = never)
unsigned long dropSeenID = 0;                 // counts drop-backs (dashboard)
unsigned long dropPullSince = 0;

// How full the waves alone get the bucket. Learned from each
// wave that fills and empties it without causing an alarm.
float sustainNeed = 1.0;         // s the bucket must stay filled before an alarm

float waveFill = 0;
float pullEpisodePeak = 0;
float dropEpisodePeak = 0;
unsigned long pullEpisodeStart = 0;
unsigned long dropEpisodeStart = 0;

// =====================================================
// SETTLING
// =====================================================

bool pullTestAfterSettle = false;
bool teachDirAsked = false;      // "new pull direction" asked: pull test after learning
unsigned long settleGraceUntil = 0;
unsigned long settleStart = 0;

Vec3 recentRest = { 0, 0, 1 };   // where the tip pointed over the last ~2 s
unsigned long fastFollowUntil = 0;

bool calmTimerRunning = false;
unsigned long calmStart = 0;

Vec3 settleAnchor = { 0, 0, 1 };
Vec3 settleSum = { 0, 0, 0 };
unsigned long settleSamples = 0;
float settleMaxDev = 0;

int settlingConfidence = 0;

// Local motion while settling (does not use old rest position)
const int SETTLE_RING = 20;      // one entry every 50 ms = 1 s

Vec3 settleRing[SETTLE_RING];
int settleRingIndex = 0;
int settleRingCount = 0;
int settleRingTick = 0;

bool settlePullCandidate = false;
unsigned long settlePullStart = 0;
bool settlePullAway = false;     // moving away from the water = drop-back

// =====================================================
// PULL TEST
// =====================================================

bool pullTestActive = false;
float pullTestWaveDeg = 0;       // usual total bend from waves alone
unsigned long pullTestAboveSince = 0;
unsigned long pullTestMaxTime = 0;
float pullTestMax = 0;
Vec3 pullTestVec = { 0, 0, 0 };

// =====================================================
// STRIKE
// =====================================================

String strikeType = "";
String lastStrikeType = "NONE";

float strikePeakEnergy = 0;
float strikePeakTilt = 0;

unsigned long strikeStart = 0;
unsigned long strikeEventID = 0;

// =====================================================
// STRIKE REPLAY
//
// 4 s before + 4 s after the strike, 50 points per second.
// =====================================================

const int REC_PRE = 200;
const int REC_POST = 200;
const int REC_TOTAL = REC_PRE + REC_POST;

float preEnergy[REC_PRE];
float preTilt[REC_PRE];
int preIndex = 0;
int preCount = 0;

float savedEnergy[REC_TOTAL];
float savedTilt[REC_TOTAL];
int savedCount = 0;
int savedPre = 0;

float savedThreshold = 0;
float savedAllow = 0;
String savedType = "";

bool recordingStrike = false;
bool lastStrikeReady = false;

// Last strike for the watch: 30 s at 5 points a second (20 s before, 10 s after)
const int WREC_N = 150;
const int WREC_POST = 50;
int16_t wrecK[WREC_N];                 // ring: knock % of the limit
int16_t wrecT[WREC_N];                 // ring: tilt x 100
int wrecHead = 0;
int wrecCount = 0;
float wrecEnergyMax = 0;
int wrecTick = 0;

bool wsRecording = false;              // a strike is being recorded (10 s after)
int wsPost = 0;
String wsNextType = "";
unsigned long wsNextAt = 0;
float wsNextTiltPeak = 0;
int wsNextPull = 0;
uint32_t wsNextId = 0;                 // the strike's number

// The last 5 finished strikes with their graphs, oldest first. The phone app
// fetches each by its number (/laststrike?n=), so a quick second strike no
// longer loses the first one's graph.
const int ZOOM_EACH = 100;             // 2 s at 50 a second, before and after
const int WS_KEEP = 5;

struct WsRec
{
  uint32_t id;
  int count;                           // points, 200 ms each
  int pre;                             // points before the strike
  int16_t k[WREC_N];
  int16_t t[WREC_N];
  int zpre;                            // zoom: points before the strike
  int zn;
  int16_t zk[ZOOM_EACH * 2];           // zoom: 50 a second around the strike
  int16_t zt[ZOOM_EACH * 2];
  char type[24];
  unsigned long at;
  float tiltPeak;
  int pull;
};

WsRec wsKeep[WS_KEEP];
int wsKeepN = 0;

// Rod battery: time left from how fast the % drops
int batStartPct = -1;
unsigned long batStartMs = 0;
long batLeftMin = -1;
int postRecorded = 0;

bool recordTick = false;
float recordEnergyMax = 0;

// =====================================================
// SENSOR
// =====================================================

// Several sensor readings are averaged into each 10 ms step.
// Random noise shrinks, knocks (5-15 Hz) are not affected.
Vec3 oversampleSum = { 0, 0, 0 };
int oversampleCount = 0;
unsigned long lastOversample = 0;

bool sensorHighRes = false;
uint8_t sensorReg1 = 0;
uint8_t sensorReg4 = 0;

void enableHighResolution();

// ---------- Which sensor ----------

enum SensorType
{
  SENSOR_NONE,
  SENSOR_LIS3DH,
  SENSOR_ADXL355
};

SensorType sensorType = SENSOR_NONE;

const char* sensorName()
{
  if(sensorType == SENSOR_ADXL355) return "ADXL355";
  if(sensorType == SENSOR_LIS3DH) return "LIS3DH";

  return "none";
}

// ---------- ADXL355 over I2C ----------

const uint8_t ADXL_DEVID_AD = 0x00;      // AD 1D ED
const uint8_t ADXL_XDATA3 = 0x08;        // X, Y, Z: 3 bytes each, 20 bits
const uint8_t ADXL_FILTER = 0x28;
const uint8_t ADXL_RANGE = 0x2C;
const uint8_t ADXL_POWER_CTL = 0x2D;
const uint8_t ADXL_RESET = 0x2F;
const float ADXL_LSB_PER_G = 256000.0;   // +-2 g range

uint8_t adxlAddr = 0;
unsigned long sensorReadErrors = 0;

bool i2cPresent(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool adxlWrite(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(adxlAddr);
  Wire.write(reg);
  Wire.write(value);

  return Wire.endTransmission() == 0;
}

bool adxlRead(uint8_t reg, uint8_t* buf, uint8_t n)
{
  Wire.beginTransmission(adxlAddr);
  Wire.write(reg);

  if(Wire.endTransmission(false) != 0)
    return false;

  if(Wire.requestFrom(adxlAddr, n) != n)
    return false;

  for(uint8_t i = 0; i < n; i++)
    buf[i] = Wire.read();

  return true;
}

float adxlAxis(const uint8_t* b)
{
  long v = ((long)b[0] << 12) | ((long)b[1] << 4) | (b[2] >> 4);

  if(v & 0x80000)
    v -= 0x100000;

  return v / ADXL_LSB_PER_G;
}

bool startADXL355()
{
  if(i2cPresent(0x53)) adxlAddr = 0x53;
  else if(i2cPresent(0x1D)) adxlAddr = 0x1D;
  else return false;

  uint8_t id[3] = { 0, 0, 0 };
  adxlRead(ADXL_DEVID_AD, id, 3);

  if(id[0] != 0xAD || id[1] != 0x1D || id[2] != 0xED)
  {
    Serial.printf("Device at 0x%02X is not an ADXL355 (ID %02X %02X %02X)\n", adxlAddr, id[0], id[1], id[2]);
    return false;
  }

  adxlWrite(ADXL_RESET, 0x52);
  delay(50);

  adxlWrite(ADXL_RANGE, 0x01);       // +-2 g, the most sensitive
  adxlWrite(ADXL_FILTER, 0x03);      // 500 readings per second (filter 125 Hz)
  adxlWrite(ADXL_POWER_CTL, 0x00);   // start measuring

  delay(50);

  sensorType = SENSOR_ADXL355;
  sensorHighRes = true;              // always 20 bit

  Serial.printf("ADXL355 CONNECTED at 0x%02X\n", adxlAddr);

  return true;
}

bool startLIS3DH()
{
  myIMU.settings.accelSampleRate = SENSOR_RATE;
  myIMU.settings.accelRange = 2;          // most sensitive range

  if(myIMU.begin() != 0)
    return false;

  // After begin(), which may reset the bus speed
  Wire.setClock(SENSOR_I2C_HZ);

  sensorType = SENSOR_LIS3DH;

  Serial.println("LIS3DH CONNECTED");

  enableHighResolution();

  return true;
}

bool startSensor()
{
  if(startADXL355())
  {
    KNOCK_FLOOR = KNOCK_FLOOR_ADXL355;
    SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_ADXL355;
    return true;
  }

  if(startLIS3DH())
  {
    KNOCK_FLOOR = KNOCK_FLOOR_LIS3DH;
    SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_LIS3DH;
    return true;
  }

  return false;
}

// One reading in g. Returns false if the sensor did not answer.
bool readOnce(Vec3& out)
{
  if(sensorType == SENSOR_ADXL355)
  {
    uint8_t b[9];

    if(!adxlRead(ADXL_XDATA3, b, 9))
    {
      sensorReadErrors++;
      return false;
    }

    out.x = adxlAxis(b);
    out.y = adxlAxis(b + 3);
    out.z = adxlAxis(b + 6);

    return true;
  }

  out.x = myIMU.readFloatAccelX();
  out.y = myIMU.readFloatAccelY();
  out.z = myIMU.readFloatAccelZ();

  return true;
}

void oversample()
{
  if(micros() - lastOversample < OVERSAMPLE_US)
    return;

  lastOversample = micros();

  Vec3 a;

  if(!readOnce(a))
    return;

  oversampleSum = vAdd(oversampleSum, a);
  oversampleCount++;
}

void readSensor()
{
  if(oversampleCount == 0)
  {
    readOnce(rawAccel);
    return;
  }

  rawAccel = vScale(oversampleSum, 1.0 / oversampleCount);

  oversampleSum = vec(0, 0, 0);
  oversampleCount = 0;
}

// High resolution (12 bit) mode: HR bit on, low-power bit off.
// Read back so we can see that it really worked.
void enableHighResolution()
{
  uint8_t reg = 0;

  myIMU.readRegister(&reg, LIS3DH_CTRL_REG1);
  reg &= ~0x08;  // LPen off
  myIMU.writeRegister(LIS3DH_CTRL_REG1, reg);

  myIMU.readRegister(&reg, LIS3DH_CTRL_REG4);
  reg |= 0x08;   // HR on
  myIMU.writeRegister(LIS3DH_CTRL_REG4, reg);

  myIMU.readRegister(&sensorReg1, LIS3DH_CTRL_REG1);
  myIMU.readRegister(&sensorReg4, LIS3DH_CTRL_REG4);

  sensorHighRes =
    (sensorReg4 & 0x08) &&
    !(sensorReg1 & 0x08);

  Serial.print("CTRL_REG1=0x");
  Serial.print(sensorReg1, HEX);
  Serial.print(" CTRL_REG4=0x");
  Serial.print(sensorReg4, HEX);
  Serial.println(sensorHighRes ? "  HIGH RESOLUTION ON" : "  HIGH RESOLUTION FAILED");
}

// =====================================================
// SIGNAL PROCESSING
// =====================================================

// ---------- 3D noise view ----------
//
// The last vibration vectors (100 per second), sent to the dashboard
// with every /data answer. Units: 0.1 mg (0.0001 g).

const int VIB_RING = 64;

int16_t vibRing[VIB_RING][3];
unsigned long vibWritten = 0;
unsigned long vibSent = 0;

int16_t toTenthMg(float g)
{
  float v = g * 10000.0;

  if(v > 32000) v = 32000;
  if(v < -32000) v = -32000;

  return (int16_t)lround(v);
}

void storeVibration(Vec3 v)
{
  int i = vibWritten % VIB_RING;

  vibRing[i][0] = toTenthMg(v.x);
  vibRing[i][1] = toTenthMg(v.y);
  vibRing[i][2] = toTenthMg(v.z);

  vibWritten++;
}

void addVibrationSamples(String& json)
{
  unsigned long from = vibSent;

  if(vibWritten - from > VIB_RING)
    from = vibWritten - VIB_RING;

  json += ",\"vib\":[";

  for(unsigned long n = from; n < vibWritten; n++)
  {
    int i = n % VIB_RING;

    if(n != from) json += ",";

    json += vibRing[i][0];
    json += ",";
    json += vibRing[i][1];
    json += ",";
    json += vibRing[i][2];
  }

  json += "],\"g\":[";
  json += String(gravityUnit.x, 3);
  json += ",";
  json += String(gravityUnit.y, 3);
  json += ",";
  json += String(gravityUnit.z, 3);
  json += "]";

  vibSent = vibWritten;
}

void updateSignals()
{
  gravity =
    vAdd(
      gravity,
      vScale(
        vSub(rawAccel, gravity),
        alphaFor(GRAVITY_TAU)
      )
    );

  // Bending of the tip (even a fast one) is not vibration
  slowPart =
    vAdd(
      slowPart,
      vScale(
        vSub(rawAccel, slowPart),
        alphaFor(VIBRATION_TAU)
      )
    );

  Vec3 vibration =
    vSub(rawAccel, slowPart);

  storeVibration(vibration);

  energySq +=
    (vDot(vibration, vibration) - energySq) *
    alphaFor(ENERGY_TAU);

  knockEnergy =
    sqrt(energySq);

  if(knockEnergy > energyPeakSincePoll)
    energyPeakSincePoll = knockEnergy;

  if(knockEnergy > energyPeakSinceWatch)
    energyPeakSinceWatch = knockEnergy;

  // Ignore free fall / bad readings for the angle
  if(vLen(gravity) > 0.5)
    gravityUnit = vNorm(gravity);

  float c = vDot(gravityUnit, restDir);

  totalDeg = vAngleDeg(gravityUnit, restDir);

  if(haveDir)
  {
    tiltDeg =
      atan2(vDot(gravityUnit, pullDir), c) *
      RAD_TO_DEGREES;

    sideDeg =
      atan2(vDot(gravityUnit, sideDir), c) *
      RAD_TO_DEGREES;
  }
  else
  {
    // No direction: fall back to "how much", like the old code
    tiltDeg = totalDeg;
    sideDeg = 0;
  }

  fastGravity =
    vAdd(
      fastGravity,
      vScale(
        vSub(rawAccel, fastGravity),
        alphaFor(FAST_TILT_TAU)
      )
    );

  if(vLen(fastGravity) > 0.5)
  {
    Vec3 fu = vNorm(fastGravity);

    if(haveDir)
      fastTiltDeg = atan2(vDot(fu, pullDir), vDot(fu, restDir)) * RAD_TO_DEGREES;
    else
      fastTiltDeg = vAngleDeg(fu, restDir);
  }

  recentRest =
    vNorm(
      vAdd(
        recentRest,
        vScale(
          vSub(gravityUnit, recentRest),
          alphaFor(RECENT_REST_TAU)
        )
      )
    );
}

// =====================================================
// REST POSITION + DIRECTION
// =====================================================

// v turned by the turn that takes unit vector a to unit vector b
Vec3 vTurn(Vec3 v, Vec3 a, Vec3 b)
{
  Vec3 axis = vCross(a, b);
  float s = vLen(axis);
  float c = vDot(a, b);

  if(s < 1e-6)
    return v;

  Vec3 k = vScale(axis, 1.0 / s);

  return vAdd(
    vAdd(vScale(v, c), vScale(vCross(k, v), s)),
    vScale(k, vDot(k, v) * (1 - c))
  );
}

void refreshDirection()
{
  if(!haveStoredDir)
  {
    haveDir = false;
    return;
  }

  // turn the saved direction with the rod (from the rest at the pull test)
  Vec3 aimed =
    haveStoredRest ? vTurn(storedDir, storedRest, restDir) : storedDir;

  Vec3 d =
    vPerp(aimed, restDir);

  if(vLen(d) < 0.3)
  {
    // Rod is now pointing along the old pull direction
    haveDir = false;
    return;
  }

  pullDir = vNorm(d);
  sideDir = vCross(restDir, pullDir);
  haveDir = true;
}

void setRest(Vec3 r)
{
  restDir = vNorm(r);

  tidePrevRest = restDir;
  tideCheckTime = millis();

  refreshDirection();
}

void loadStoredDirection()
{
  prefs.begin("strikesense", true);

  haveStoredDir = prefs.getBool("hasdir", false);

  storedDir.x = prefs.getFloat("dx", 0);
  storedDir.y = prefs.getFloat("dy", 0);
  storedDir.z = prefs.getFloat("dz", 0);

  haveStoredRest = prefs.getBool("hasrest", false);
  storedRest.x = prefs.getFloat("rx", 0);
  storedRest.y = prefs.getFloat("ry", 0);
  storedRest.z = prefs.getFloat("rz", 0);

  if(vLen(storedRest) < 0.5)
    haveStoredRest = false;

  prefs.end();

  if(vLen(storedDir) < 0.5)
    haveStoredDir = false;
}

void loadSensorFloor()
{
  prefs.begin("strikesense", true);

  bool adxl = sensorType == SENSOR_ADXL355;

  haveSensorFloor = !FORGET_SAVED_NOISE && prefs.getBool(adxl ? "hasfloorA" : "hasfloor", false);
  savedSensorFloor = prefs.getFloat(adxl ? "floorA" : "floor", SENSOR_FLOOR_DEFAULT);

  prefs.end();

  if(haveSensorFloor)
    sensorFloor = savedSensorFloor;
}

// Flash wears out with many writes: only save a clearly lower
// value, and not more than once a minute
void maybeSaveSensorFloor()
{
  if(haveSensorFloor && sensorFloor > savedSensorFloor * 0.9)
    return;

  if(sensorFloorSavedAt != 0 && millis() - sensorFloorSavedAt < 60000)
    return;

  prefs.begin("strikesense", false);
  bool adxl = sensorType == SENSOR_ADXL355;

  prefs.putBool(adxl ? "hasfloorA" : "hasfloor", true);
  prefs.putFloat(adxl ? "floorA" : "floor", sensorFloor);
  prefs.end();

  haveSensorFloor = true;
  savedSensorFloor = sensorFloor;
  sensorFloorSavedAt = millis();

  Serial.print("Sensor noise saved (g): ");
  Serial.println(sensorFloor, 4);
}

// Lower the sensor noise estimate whenever it is quieter than ever
void updateSensorFloor(float level)
{
  if(!haveSensorFloor || level < sensorFloor)
  {
    sensorFloor = level;
    maybeSaveSensorFloor();
  }
}

// ---------- Live settings in flash ----------

void loadSettings()
{
  prefs.begin("strikesense", true);

  for(int i = 0; i < SETTING_COUNT; i++)
  {
    const SettingInfo& info = SETTING_INFO[i];

    settings.*(info.field) =
      clampFloat(
        prefs.getFloat(info.key, DEFAULT_SETTINGS.*(info.field)),
        info.low,
        info.high
      );
  }

  prefs.end();

  savedSettings = settings;
}

void saveSettings()
{
  prefs.begin("strikesense", false);

  for(int i = 0; i < SETTING_COUNT; i++)
    prefs.putFloat(SETTING_INFO[i].key, settings.*(SETTING_INFO[i].field));

  prefs.end();

  savedSettings = settings;

  Serial.println("SETTINGS SAVED");
}

bool settingsSaved()
{
  for(int i = 0; i < SETTING_COUNT; i++)
  {
    if(settings.*(SETTING_INFO[i].field) != savedSettings.*(SETTING_INFO[i].field))
      return false;
  }

  return true;
}

// ---------- All-time running hours in flash ----------

void loadTotalTime()
{
  prefs.begin("strikesense", true);
  totalSecondsSaved = prefs.getULong("total", 0);
  prefs.end();

  totalSavedAtMs = millis();
}

unsigned long totalSeconds()
{
  return totalSecondsSaved + (millis() - totalSavedAtMs) / 1000;
}

void maybeSaveTotalTime()
{
  if(millis() - totalSavedAtMs < TOTAL_SAVE_MS)
    return;

  totalSecondsSaved = totalSeconds();
  totalSavedAtMs = millis();

  prefs.begin("strikesense", false);
  prefs.putULong("total", totalSecondsSaved);
  prefs.end();
}

void saveStoredDirection()
{
  prefs.begin("strikesense", false);

  prefs.putBool("hasdir", true);
  prefs.putFloat("dx", storedDir.x);
  prefs.putFloat("dy", storedDir.y);
  prefs.putFloat("dz", storedDir.z);

  prefs.putBool("hasrest", haveStoredRest);
  prefs.putFloat("rx", storedRest.x);
  prefs.putFloat("ry", storedRest.y);
  prefs.putFloat("rz", storedRest.z);

  prefs.end();
}

// =====================================================
// ENVIRONMENT TRACKING
// =====================================================

const SeaProfile& profile()
{
  return SEA_PROFILES[seaState];
}

void updateKnockThreshold()
{
  float thr =
    knockMean + profile().knockDev * knockSpread * settings.knockMul;

  // A wind gust raises the vibration for a few seconds above
  // normal: raise the limit by the same ratio, so knocks must
  // stand out from the gust too
  if(gustNoise > knockMean && knockMean > 0)
    thr *= 1.0 + GUST_NOISE_MUL * (gustNoise / knockMean - 1.0);

  knockThreshold =
    // The slider also moves the smallest limit, so it works
    // even when the sensor is so quiet that the minimum applies
    clampFloat(thr, KNOCK_FLOOR * settings.knockMul, KNOCK_CEIL);
}

// Background vibration. Learns from quiet readings and from
// long shakes (wind), never from real knocks.
void updateKnockNoise()
{
  // Fast level follows everything except knocks and their ring-down
  bool nearKnock =
    (knockActive && millis() - knockStart <= KNOCK_MAX_MS) ||
    millis() - lastValidKnock < 300;

  if(!nearKnock)
  {
    gustNoise +=
      (knockEnergy - gustNoise) * alphaFor(GUST_NOISE_TAU);

    updateKnockThreshold();
  }

  bool quietSample =
    !knockActive &&
    knockEnergy < knockThreshold;

  bool windShake =
    knockActive &&
    millis() - knockStart > KNOCK_MAX_MS;

  if(!quietSample && !windShake)
    return;

  if(millis() - lastValidKnock < 1000)
    return;

  float a = alphaFor(NOISE_TAU);

  knockMean += (knockEnergy - knockMean) * a;

  knockSpread +=
    (fabs(knockEnergy - knockMean) - knockSpread) * a;

  updateKnockThreshold();

  if(rodState == ST_MONITORING)
    updateSensorFloor(knockMean);
}

void updateWaveWindTide()
{
  // ---------- Waves ----------

  waveAmp +=
    (fabs(tiltDeg) - waveAmp) *
    alphaFor(WAVE_TAU);

  // average |sine| = 0.64 * peak
  waveSwing = waveAmp * 1.57;

  // Period from upward zero crossings (signed tilt only)
  if(haveDir)
  {
    float h = waveAmp * 0.5;

    if(h < 0.05) h = 0.05;

    if(tiltDeg < -h)
    {
      waveBelow = true;
    }
    else if(tiltDeg > h && waveBelow)
    {
      waveBelow = false;

      unsigned long now = millis();

      if(lastUpCross != 0)
      {
        float period =
          (now - lastUpCross) / 1000.0;

        if(period >= 1.5 && period <= 20.0)
        {
          if(wavePeriod <= 0)
            wavePeriod = period;
          else
            wavePeriod += (period - wavePeriod) * 0.2;

          lastWaveCrossSeen = now;
        }
      }

      lastUpCross = now;
    }

    if(
      wavePeriod > 0 &&
      millis() - lastWaveCrossSeen > 30000
    )
    {
      wavePeriod = 0;
    }
  }

  // ---------- Wind (sideways) ----------

  if(haveDir)
  {
    sideAmp +=
      (fabs(sideDeg) - sideAmp) *
      alphaFor(WIND_TAU);

    windSwing = sideAmp * 1.57;
  }

  // ---------- Rest position follows tide ----------

  float restTau =
    millis() < fastFollowUntil ? FAST_FOLLOW_TAU : BASELINE_TAU;

  restDir =
    vNorm(
      vAdd(
        restDir,
        vScale(
          vSub(gravityUnit, restDir),
          alphaFor(restTau)
        )
      )
    );

  if(haveDir)
  {
    pullDir = vNorm(vPerp(pullDir, restDir));
    sideDir = vCross(restDir, pullDir);
  }

  // ---------- Tide drift rate ----------

  if(millis() - tideCheckTime >= 10000)
  {
    float moved =
      vAngleDeg(restDir, tidePrevRest);

    float rate = moved * 6.0;    // per 10 s -> per minute

    tideRate += (rate - tideRate) * 0.3;

    tidePrevRest = restDir;
    tideCheckTime = millis();
  }
}

int levelFor(float value, float a, float b, float c)
{
  if(value < a) return 0;
  if(value < b) return 1;
  if(value < c) return 2;

  return 3;
}

int autoSeaState()
{
  if(waveLevel >= 3 || windLevel >= 3)
    return 2;

  if(waveLevel <= 1 && windLevel <= 1)
    return 0;

  return 1;
}

void updateConditions(bool immediate)
{
  waveLevel =
    levelFor(waveSwing, WAVE_SMALL, WAVE_MEDIUM, WAVE_BIG);

  // Noise adds up like the sides of a right triangle, so remove
  // the sensor's own noise that way
  float extra = knockMean * knockMean - sensorFloor * sensorFloor;

  windVib = extra > 0 ? sqrt(extra) : 0;

  int vibLevel =
    levelFor(windVib, WINDVIB_NORMAL, WINDVIB_STRONG, WINDVIB_VSTRONG);

  windLevel = vibLevel;

  if(haveDir)
  {
    int swayLevel =
      levelFor(windSwing, WIND_NORMAL, WIND_STRONG, WIND_VSTRONG);

    if(swayLevel > windLevel)
      windLevel = swayLevel;
  }

  if(tideRate < TIDE_SLOW) tideLevel = 0;
  else if(tideRate < TIDE_FAST) tideLevel = 1;
  else tideLevel = 2;

  if(!seaAuto)
    return;

  int wanted = autoSeaState();

  if(immediate)
  {
    seaState = wanted;
    seaCandidate = wanted;
    updateKnockThreshold();

    return;
  }

  if(wanted == seaState)
  {
    seaCandidate = seaState;
    return;
  }

  if(wanted != seaCandidate)
  {
    seaCandidate = wanted;
    seaCandidateSince = millis();
  }
  else if(millis() - seaCandidateSince >= SEA_STATE_HOLD)
  {
    seaState = wanted;
    updateKnockThreshold();

    Serial.print("SEA STATE: ");
    Serial.println(SEA_NAMES[seaState]);
  }
}

// =====================================================
// STRIKE REPLAY RECORDING
// =====================================================

void fightOnStrike();
void landOnReelIn();
void landTick();
void landNews();
void addLand(String& json);
void updateFight();
void finishWatchRecording();
bool loraRadioOk();
void loadLinkMode();
const char* linkModeName();
void setLinkMode(int v);
bool watchConnected();
int loraWatchBars();

void startStrikeRecording()
{
  savedCount = 0;

  int start =
    preCount < REC_PRE ? 0 : preIndex;

  for(int i = 0; i < preCount; i++)
  {
    int index = (start + i) % REC_PRE;

    savedEnergy[savedCount] = preEnergy[index];
    savedTilt[savedCount] = preTilt[index];
    savedCount++;
  }

  savedPre = savedCount;
  savedThreshold = knockThreshold;
  savedAllow = pullAllow;
  savedType = strikeType;

  recordingStrike = true;
  postRecorded = 0;
  lastStrikeReady = false;
}

// Watch's last strike: called every reading, keeps 5 points a second
void updateWatchRecording()
{
  if(knockEnergy > wrecEnergyMax)
    wrecEnergyMax = knockEnergy;

  if(++wrecTick < 20)                  // 20 readings x 10 ms = 200 ms
    return;

  wrecTick = 0;

  float thr = knockThreshold > 0 ? knockThreshold : 1;
  float k = wrecEnergyMax / thr * 100;
  float t = tiltDeg * 100;
  wrecEnergyMax = 0;

  wrecK[wrecHead] = (int16_t)(k > 300 ? 300 : k);
  wrecT[wrecHead] = (int16_t)(t > 300 ? 300 : (t < -300 ? -300 : t));
  wrecHead = (wrecHead + 1) % WREC_N;

  if(wrecCount < WREC_N)
    wrecCount++;

  if(!wsRecording)
    return;

  if(fabs(tiltDeg) > wsNextTiltPeak)
    wsNextTiltPeak = fabs(tiltDeg);

  if(++wsPost < WREC_POST)
    return;

  finishWatchRecording();              // 10 s after the strike
}

// Keep the strike being recorded with its graph (the last 30 s) and zoom.
// 10 s after it, or sooner when the next strike comes.
void finishWatchRecording()
{
  if(!wsRecording)
    return;

  if(wsKeepN == WS_KEEP)
  {
    for(int i = 1; i < WS_KEEP; i++)
      wsKeep[i - 1] = wsKeep[i];

    wsKeepN--;
  }

  WsRec& r = wsKeep[wsKeepN++];
  int start = (wrecHead - wrecCount + WREC_N) % WREC_N;

  r.id = wsNextId;
  r.count = wrecCount;
  r.pre = wrecCount > wsPost ? wrecCount - wsPost : 0;

  for(int i = 0; i < r.count; i++)
  {
    r.k[i] = wrecK[(start + i) % WREC_N];
    r.t[i] = wrecT[(start + i) % WREC_N];
  }

  // zoom: the dashboard's recording of the same strike (50 a second)
  int zs = savedPre - ZOOM_EACH;
  int ze = savedPre + ZOOM_EACH;

  if(zs < 0) zs = 0;
  if(ze > savedCount) ze = savedCount;
  if(ze < zs) ze = zs;

  float thr = savedThreshold > 0 ? savedThreshold : 1;
  r.zpre = savedPre - zs;
  r.zn = ze - zs;

  for(int i = 0; i < r.zn; i++)
  {
    float k = savedEnergy[zs + i] / thr * 100;
    float t = savedTilt[zs + i] * 100;
    r.zk[i] = (int16_t)(k > 300 ? 300 : k);
    r.zt[i] = (int16_t)(t > 300 ? 300 : (t < -300 ? -300 : t));
  }

  strncpy(r.type, wsNextType.c_str(), sizeof(r.type) - 1);
  r.type[sizeof(r.type) - 1] = 0;
  r.at = wsNextAt;
  r.tiltPeak = wsNextTiltPeak;
  r.pull = wsNextPull;

  wsRecording = false;
}

void startWatchRecording()
{
  wsRecording = true;
  wsPost = 0;
  wsNextId = strikeEventID;
  wsNextType = strikeType;
  wsNextAt = millis();
  wsNextTiltPeak = fabs(tiltDeg);
  wsNextPull = (int)(pullBucket / bucketSize * 100.0);
}

// Called every reading, stores every second reading (50 per second)
void updateRecording()
{
  if(knockEnergy > recordEnergyMax)
    recordEnergyMax = knockEnergy;

  recordTick = !recordTick;

  if(!recordTick)
    return;

  float e = recordEnergyMax;
  recordEnergyMax = 0;

  if(recordingStrike)
  {
    if(savedCount < REC_TOTAL)
    {
      savedEnergy[savedCount] = e;
      savedTilt[savedCount] = tiltDeg;
      savedCount++;
    }

    postRecorded++;

    if(postRecorded >= REC_POST || savedCount >= REC_TOTAL)
    {
      recordingStrike = false;
      lastStrikeReady = true;
    }
  }

  preEnergy[preIndex] = e;
  preTilt[preIndex] = tiltDeg;

  preIndex++;

  if(preIndex >= REC_PRE)
    preIndex = 0;

  if(preCount < REC_PRE)
    preCount++;
}

// =====================================================
// DETECTOR RESET
// =====================================================

void resetDetectors()
{
  knockActive = false;
  knockAbove = 0;

  for(int i = 0; i < 8; i++)
    knockTimes[i] = 0;

  sharpPending = false;

  pullBucket = 0;
  dropBucket = 0;

  pullEpisodePeak = 0;
  dropEpisodePeak = 0;
  pullEpisodeStart = 0;
  dropEpisodeStart = 0;
}

void resetSettleWindow()
{
  calmTimerRunning = false;

  settleSum = vec(0, 0, 0);
  settleSamples = 0;
  settleMaxDev = 0;

  settlingConfidence = 0;
}

void resetSettleRing()
{
  settleRingIndex = 0;
  settleRingCount = 0;
  settleRingTick = 0;

  settlePullCandidate = false;
}

// =====================================================
// STRIKE
// =====================================================

void startStrike(String type)
{
  if(strikeActive)
    return;

  if(rodState == ST_PAUSED)
    return;

  strikeActive = true;

  strikeType = type;
  lastStrikeType = type;

  strikeEventID++;

  strikePeakEnergy = knockEnergy;
  strikePeakTilt = fabs(tiltDeg);

  strikeStart = millis();

  // A strike during settling ends settling
  if(rodState == ST_SETTLING)
  {
    rodState = ST_MONITORING;
    resetSettleWindow();
    resetSettleRing();
  }

  sharpPending = false;

  finishWatchRecording();
  startStrikeRecording();
  startWatchRecording();
  fightOnStrike();

#if defined(ESP32)
  server.pushLive();                    // phone + watch hear it now, not at the next tick
#endif

  Serial.println();
  Serial.print(">>> STRIKE: ");
  Serial.print(type);
  Serial.println(" <<<");
}

void clearStrike()
{
  strikeActive = false;

  strikeType = "";

  strikePeakEnergy = 0;
  strikePeakTilt = 0;
}

// =====================================================
// STATE CHANGES
// =====================================================

void goMonitoring()
{
  rodState = ST_MONITORING;

  resetDetectors();
  resetSettleWindow();
  resetSettleRing();

  settlingConfidence = 100;

  if(armedSince == 0)
    armedSince = millis();

  Serial.println("MONITORING");
}

void startPullTest()
{
  rodState = ST_PULLTEST;

  // After learning this is already measured; after READY use the wave tracker
  if(pullTestWaveDeg <= 0)
    pullTestWaveDeg = waveSwing;

  pullTestActive = false;
  pullTestAboveSince = 0;
  pullTestMax = 0;

  resetDetectors();

  Serial.println();
  Serial.println("PULL TEST: pull the line gently once");
}

void startSettling(bool fromReady)
{
  rodState = ST_SETTLING;

  dropWatch = false;

  pullTestAfterSettle = fromReady;

  settleGraceUntil =
    fromReady ? millis() + READY_GRACE_MS : 0;

  settleStart = millis();

  resetSettleWindow();
  resetSettleRing();
  resetDetectors();

  Serial.println();
  Serial.println("PROTECTED SETTLING STARTED");
}

void startEnvironmentLearning()
{
  rodState = ST_LEARNING;

  dropWatch = false;

  armedSince = 0;

  clearStrike();
  resetDetectors();
  resetSettleWindow();
  resetSettleRing();

  readSensor();

  gravity = rawAccel;
  slowPart = rawAccel;
  gravityUnit = vNorm(gravity);
  recentRest = gravityUnit;
  energySq = 0;
  gustNoise = 0.005;

  learnCount = 0;
  learnSum = vec(0, 0, 0);

  waveAmp = 0;
  waveSwing = 0;
  wavePeriod = 0;
  sideAmp = 0;
  windSwing = 0;
  tideRate = 0;

  lastUpCross = 0;
  waveBelow = false;

  learningStart = millis();

  Serial.println();
  Serial.println("ENVIRONMENT LEARNING STARTED");
}

// =====================================================
// LEARNING STEP
// =====================================================

int compareFloat(const void* a, const void* b)
{
  float fa = *(const float*)a;
  float fb = *(const float*)b;

  if(fa < fb) return -1;
  if(fa > fb) return 1;

  return 0;
}

void learningStep()
{
  unsigned long elapsed =
    millis() - learningStart;

  // Give the filters 0.5 s to settle first
  if(elapsed > 500 && learnCount < LEARN_MAX)
  {
    learnEnergy[learnCount] = knockEnergy;
    learnDir[learnCount] = gravityUnit;
    learnCount++;

    learnSum = vAdd(learnSum, gravityUnit);
  }

  if(elapsed < LEARN_TIME || learnCount < 10)
    return;

  // ---------- Background vibration: median and 95% ----------

  qsort(learnEnergy, learnCount, sizeof(float), compareFloat);

  float p50 = learnEnergy[learnCount / 2];
  float p95 = learnEnergy[(learnCount * 95) / 100];

  knockMean = p50;
  gustNoise = p50;

  updateSensorFloor(p50);
  knockSpread = (p95 - p50) / 2.0;

  if(knockSpread < 0.0005)
    knockSpread = 0.0005;

  // ---------- Rest position: average ----------

  Vec3 mean = vScale(learnSum, 1.0 / learnCount);

  setRest(mean);

  // ---------- Starting wave + wind size ----------

  float sumWave = 0;
  float sumSide = 0;

  for(int i = 0; i < learnCount; i++)
  {
    Vec3 g = learnDir[i];
    float c = vDot(g, restDir);

    if(haveDir)
    {
      sumWave += fabs(atan2(vDot(g, pullDir), c));
      sumSide += fabs(atan2(vDot(g, sideDir), c));
    }
    else
    {
      sumWave += acos(clampFloat(c, -1, 1));
    }
  }

  waveAmp = sumWave / learnCount * RAD_TO_DEGREES;
  waveSwing = waveAmp * 1.57;

  // Usual bend from waves (90% of readings) for the pull test.
  // learnEnergy is not needed any more, reuse it.
  for(int i = 0; i < learnCount; i++)
    learnEnergy[i] = vAngleDeg(learnDir[i], restDir);

  qsort(learnEnergy, learnCount, sizeof(float), compareFloat);

  pullTestWaveDeg = learnEnergy[(learnCount * 90) / 100];

  sideAmp = sumSide / learnCount * RAD_TO_DEGREES;
  windSwing = sideAmp * 1.57;

  // Starting guess for how full one wave gets the bucket,
  // replaced by real measurements after a few waves
  waveFill = 0.3 * DEFAULT_WAVE_PERIOD * waveSwing;

  updateConditions(true);
  updateKnockThreshold();

  Serial.println();
  Serial.println("LEARNING COMPLETE");
  Serial.print("Knock threshold (g): ");
  Serial.println(knockThreshold, 4);
  Serial.print("Wave swing (deg): ");
  Serial.println(waveSwing, 2);
  Serial.print("Sea state: ");
  Serial.println(SEA_NAMES[seaState]);

  // The pull direction is kept from the first pull test: no test again
  if(haveDir && !teachDirAsked)
  {
    Serial.println("PULL DIRECTION: the saved one (no pull test)");
    goMonitoring();
    return;
  }

  teachDirAsked = false;
  startPullTest();
}

// =====================================================
// PULL TEST STEP
// =====================================================

float pullTestNeed()
{
  return PULLTEST_MIN_DEG + 1.5 * pullTestWaveDeg;
}

void pullTestStep()
{
  float needed = pullTestNeed();

  if(!pullTestActive)
  {
    if(totalDeg > needed)
    {
      if(pullTestAboveSince == 0)
        pullTestAboveSince = millis();

      if(millis() - pullTestAboveSince >= PULLTEST_CONFIRM_MS)
      {
        pullTestActive = true;
        pullTestMax = 0;
        pullTestMaxTime = millis();
        pullTestVec = vec(0, 0, 0);
      }
    }
    else
    {
      pullTestAboveSince = 0;
    }

    return;
  }

  if(totalDeg > pullTestMax)
  {
    pullTestMax = totalDeg;
    pullTestMaxTime = millis();
  }

  // Average the bend over the whole pull, big bends count most,
  // so one wave at the wrong moment can't spoil the direction
  pullTestVec =
    vAdd(
      pullTestVec,
      vScale(vPerp(gravityUnit, restDir), totalDeg * totalDeg)
    );

  // Pull finished when the tip comes back, or (in waves)
  // when the bend stopped growing
  if(
    totalDeg < pullTestMax * 0.4 ||
    millis() - pullTestMaxTime >= PULLTEST_SETTLE_MS
  )
  {
    storedDir = vNorm(pullTestVec);
    haveStoredDir = true;
    storedRest = restDir;             // to turn the direction with the rod later
    haveStoredRest = true;

    saveStoredDirection();
    refreshDirection();

    dirLearnedAt = millis();

    Serial.print("PULL DIRECTION LEARNED (");
    Serial.print(pullTestMax, 1);
    Serial.println(" deg)");

    goMonitoring();
  }
}

// =====================================================
// KNOCK DETECTOR
//
// Returns true when a valid knock just ended.
// =====================================================

bool updateKnockDetector()
{
  unsigned long now = millis();

  // Must stay above the normal noise, or a knock never ends
  float releaseLevel =
    knockMean + (knockThreshold - knockMean) * KNOCK_RELEASE;

  if(!knockActive)
  {
    if(
      now - lastKnockEnd >= KNOCK_GAP_MS &&
      knockEnergy > knockThreshold
    )
    {
      knockAbove++;

      if(knockAbove >= KNOCK_CONFIRM)
      {
        knockActive = true;
        knockStart = now - SAMPLE_MS * (KNOCK_CONFIRM - 1);
        knockPeak = knockEnergy;
        knockTiltAtStart = tiltDeg;
        gustNoted = false;
      }
    }
    else
    {
      knockAbove = 0;
    }

    return false;
  }

  if(knockEnergy > knockPeak)
    knockPeak = knockEnergy;

  unsigned long duration =
    now - knockStart;

  bool ended =
    knockEnergy < releaseLevel;

  // Never stay stuck in a long shake
  if(!ended && duration > 3000)
    ended = true;

  if(!ended)
    return false;

  knockActive = false;
  knockAbove = 0;
  lastKnockEnd = now;

  if(duration > KNOCK_MAX_MS)
    return false;      // wind gust or handling, not a knock

  lastKnockStrength =
    knockPeak / knockThreshold;

  lastValidKnock = now;

  knockTimes[knockTimeIndex] = now;
  knockTimeIndex = (knockTimeIndex + 1) % 8;

  return true;
}

int recentKnockCount()
{
  unsigned long now = millis();

  int count = 0;

  for(int i = 0; i < 8; i++)
  {
    if(
      knockTimes[i] != 0 &&
      now - knockTimes[i] <= settings.repeatWindow * 1000.0
    )
    {
      count++;
    }
  }

  return count;
}

// How strong a single hit must be for a SHARP STRIKE. Never below
// sharpWithPull x the knock limit: in strong wind the knock limit can
// rise above the fixed sharp level, and then every knock would count.
float sharpLevel()
{
  const SeaProfile& p = profile();

  float level = p.sharpAlone * knockThreshold;

  if(level > settings.sharpG)
    level = settings.sharpG;

  if(level < p.sharpWithPull * knockThreshold)
    level = p.sharpWithPull * knockThreshold;

  return level;
}

void onKnock(bool settlingMode)
{
#if defined(ESP32)
  server.pushLive();                    // the knock shows at once on phone + watch
#endif
  const SeaProfile& p = profile();

  if(settlingMode)
  {
    // Right after READY the angler is still handling the rod
    if(millis() < settleGraceUntil)
      return;

    if(knockPeak >= sharpLevel())
    {
      startStrike("SHARP STRIKE");
      return;
    }

    knockAlertID++;
    return;
  }

  if(knockPeak >= sharpLevel())
  {
    startStrike("SHARP STRIKE");
    return;
  }

  if(recentKnockCount() >= (int)settings.repeatCount)
  {
    startStrike("REPEATED BITE");
    return;
  }

  if(lastKnockStrength >= p.sharpWithPull)
  {
    // Strong knock: sharp strike if the tip also pulls down
    sharpPending = true;
    sharpPendingStart = millis();
    sharpPendingTilt = knockTiltAtStart;
  }

  knockAlertID++;
}

// A strong shake. A short one (a hit) is judged when it ends, in
// onKnock(). A long one is a fish only if the tip also pulls toward
// the water (hit and run); otherwise it is a wind gust: a gust lasts,
// a hit does not.
bool checkViolentShake()
{
  if(!knockActive)
    return false;

  // Still short: wait until it ends (at most KNOCK_MAX_MS)
  if(millis() - knockStart <= KNOCK_MAX_MS)
    return false;

  if(knockPeak < sharpLevel())
    return false;

  // In 0.6 s a wave moves the tip at most ~0.75 x its swing
  float needed = SHARP_PULL_MIN_DEG;

  if(waveSwing * 0.8 > needed)
    needed = waveSwing * 0.8;

  if(haveDir && tiltDeg - knockTiltAtStart >= needed)
  {
    startStrike("SHARP STRIKE");
    return true;
  }

  if(!gustNoted)
  {
    gustNoted = true;
    gustIgnoredAt = millis();

    Serial.println("STRONG LONG SHAKE, NO PULL: wind gust, ignored");
  }

  return false;
}

void checkSharpPending()
{
  if(!sharpPending)
    return;

  if(millis() - sharpPendingStart > SHARP_PULL_WINDOW_MS)
  {
    sharpPending = false;
    return;
  }

  // In 0.6 s a wave moves the tip at most ~0.75 x its swing
  float needed = SHARP_PULL_MIN_DEG;

  if(waveSwing * 0.8 > needed)
    needed = waveSwing * 0.8;

  if(tiltDeg - sharpPendingTilt >= needed)
  {
    startStrike("SHARP STRIKE");
  }
}

// =====================================================
// SLOW PULL + DROP-BACK (bucket / CUSUM)
// =====================================================

// A wave fills the bucket a little and empties it again within
// part of a wave period. When it is empty again, remember how
// full that wave got it. Long episodes are not waves (a fish
// that pulled but not enough) and are not learned.
void learnWaveFill(float bucket, float& episodePeak, unsigned long& episodeStart)
{
  if(bucket > 0 && episodeStart == 0)
    episodeStart = millis();

  if(bucket > episodePeak)
    episodePeak = bucket;

  if(bucket > 0 || episodeStart == 0)
    return;

  float period =
    wavePeriod > 0 ? wavePeriod : DEFAULT_WAVE_PERIOD;

  float maxLength = period * 0.8;

  if(maxLength < 3.0)
    maxLength = 3.0;

  bool waveLike =
    (millis() - episodeStart) / 1000.0 <= maxLength;

  if(waveLike && episodePeak > waveFill)
    waveFill += (episodePeak - waveFill) * 0.5;

  episodePeak = 0;
  episodeStart = 0;
}

// How long the bucket has been filled without emptying (s)
float episodeLength(unsigned long episodeStart)
{
  if(episodeStart == 0)
    return 0;

  return (millis() - episodeStart) / 1000.0;
}

void updatePullBuckets()
{
  const SeaProfile& p = profile();

  if(haveDir)
  {
    // Waves go + then -, so a small allowance is enough: each wave
    // pours a little into the bucket and then drains it again.
    // The bucket is made clearly bigger than the waves ever fill it.
    pullAllow = p.allowMul * waveSwing;
    bucketSize = p.bucketMul * waveFill;
  }
  else
  {
    // Only "how much" is known: the allowance must be above the waves
    pullAllow = p.noDirAllowMul * waveSwing;
    bucketSize = 0;
  }

  if(pullAllow < settings.pullDeg)
    pullAllow = settings.pullDeg;

  float pullTime = settings.pullTime * p.holdMul;

  // A pull of 1.5x the allowance fills it in the pull time
  if(bucketSize < pullAllow * pullTime * 0.5)
    bucketSize = pullAllow * pullTime * 0.5;

  if(bucketSize < BUCKET_MIN)
    bucketSize = BUCKET_MIN;

  // Slow pull sensitivity: works in waves too (pull angle is only a minimum)
  pullAllow *= settings.pullMul;
  bucketSize *= settings.pullMul;

  // A wave fills the bucket and empties it again within about half
  // a wave period. A fish keeps it filled. When there are waves,
  // the bucket must stay filled longer than a wave can manage.
  sustainNeed = pullTime;

  if(waveSwing > WAVE_SMALL)
  {
    float period =
      wavePeriod > 0 ? wavePeriod : DEFAULT_WAVE_PERIOD;

    float crest = period * SUSTAIN_PERIOD_PART;

    if(crest > sustainNeed)
      sustainNeed = crest;
  }

  // ---------- Toward water ----------

  float excess = tiltDeg - pullAllow;

  if(excess > 0 && haveDir)
  {
    float gustLevel = windSwing * 2.5;

    if(gustLevel < GUST_SIDE_MIN)
      gustLevel = GUST_SIDE_MIN;

    if(fabs(sideDeg) > gustLevel)
      excess *= GUST_FACTOR;
  }

  pullBucket += excess * DT;

  pullBucket = clampFloat(pullBucket, 0, bucketSize);

  learnWaveFill(pullBucket, pullEpisodePeak, pullEpisodeStart);

  if(
    pullBucket >= bucketSize &&
    episodeLength(pullEpisodeStart) >= sustainNeed
  )
  {
    startStrike("SLOW PULL");
    return;
  }

  // ---------- Away from water ----------

  if(!haveDir)
  {
    dropBucket = 0;
    dropEpisodeStart = 0;
    return;
  }

  float dropExcess = -tiltDeg - pullAllow;

  dropBucket += dropExcess * DT;

  dropBucket = clampFloat(dropBucket, 0, bucketSize);

  learnWaveFill(dropBucket, dropEpisodePeak, dropEpisodeStart);

  if(
    dropBucket >= bucketSize &&
    episodeLength(dropEpisodeStart) >= sustainNeed
  )
  {
    noteDropBack(true);
  }
}

// ---------- Drop-back watch ----------

void noteDropBack(bool watch)
{
  if(dropWatch)
    return;

  // While settling the same slack movement can last a while: note it once
  if(!watch && dropSeenAt != 0 && millis() - dropSeenAt < 10000)
    return;

  dropSeenAt = millis();
  dropSeenID++;

  dropWatch = watch;
  dropPullSince = 0;

  Serial.println(watch ? "DROP-BACK SEEN (no alarm) - watching for a pull forward" : "DROP-BACK SEEN while settling (no alarm)");
}

void updateDropWatch()
{
  if(!dropWatch)
    return;

  // Forward past the normal position, further than the biggest wave
  float needed = pullAllow;

  if(waveSwing > needed)
    needed = waveSwing;

  needed += settings.pullDeg * 0.5;

  if(tiltDeg >= needed)
  {
    if(dropPullSince == 0)
      dropPullSince = millis();

    if(millis() - dropPullSince >= DROP_PULL_HOLD_MS && settings.dropOn > 0.5)
    {
      dropWatch = false;
      startStrike("DROP-BACK + PULL");
      return;
    }
  }
  else
  {
    dropPullSince = 0;
  }

  if(millis() - dropSeenAt >= DROP_WATCH_MS)
  {
    // Nothing followed: the slack line is the new normal position
    dropWatch = false;

    setRest(recentRest);

    pullBucket = 0;
    dropBucket = 0;
    pullEpisodePeak = 0;
    dropEpisodePeak = 0;
    pullEpisodeStart = 0;
    dropEpisodeStart = 0;

    Serial.println("DROP-BACK: no pull followed, new rod position accepted");
  }
}

// =====================================================
// MONITORING STEP
// =====================================================

void monitoringStep()
{
  bool knock = updateKnockDetector();

  if(strikeActive)
  {
    if(knockEnergy > strikePeakEnergy)
      strikePeakEnergy = knockEnergy;

    if(fabs(tiltDeg) > strikePeakTilt)
      strikePeakTilt = fabs(tiltDeg);

    return;
  }

  updateKnockNoise();

  if(checkViolentShake())
    return;

  if(knock)
  {
    onKnock(false);

    if(strikeActive)
      return;
  }

  checkSharpPending();

  if(strikeActive)
    return;

  updatePullBuckets();

  if(strikeActive)
    return;

  updateDropWatch();

  if(strikeActive)
    return;

  // Slowly forget big waves once the sea calms down
  waveFill -= waveFill * alphaFor(WAVE_FILL_TAU);

  // Learn waves/wind/tide only while nothing looks like a fish:
  // no knock, and the bucket is neither unusually full nor filled
  // for longer than a wave can keep it filled
  bool quiet =
    !dropWatch &&
    !knockActive &&
    millis() - lastKnockEnd > 1000 &&
    episodeLength(pullEpisodeStart) < sustainNeed &&
    episodeLength(dropEpisodeStart) < sustainNeed &&
    pullBucket < bucketSize * 0.6 &&
    dropBucket < bucketSize * 0.6;

  if(quiet)
    updateWaveWindTide();

  updateConditions(false);
}

void finishSettling()
{
  if(pullTestAfterSettle && !haveDir)
  {
    pullTestWaveDeg = 0;
    startPullTest();
  }
  else
  {
    goMonitoring();
  }
}

// =====================================================
// SETTLING STEP
//
// Learns the new rest position. Knocks are still detected
// (they do not need a rest position) and a continuing pull
// is detected from local motion only.
// =====================================================

void settlingStep()
{
  unsigned long now = millis();

  bool knock = updateKnockDetector();

  updateKnockNoise();

  if(now >= settleGraceUntil && checkViolentShake())
    return;

  if(knock)
  {
    onKnock(true);

    if(strikeActive)
      return;
  }

  // ---------- Continuing pull while settling ----------

  settleRingTick++;

  if(settleRingTick >= 5)
  {
    settleRingTick = 0;

    Vec3 oldest = settleRing[settleRingIndex];

    settleRing[settleRingIndex] = gravityUnit;
    settleRingIndex = (settleRingIndex + 1) % SETTLE_RING;

    if(settleRingCount < SETTLE_RING)
    {
      settleRingCount++;
    }
    else if(now >= settleGraceUntil)
    {
      float moved = vAngleDeg(gravityUnit, oldest);

      float pullLevel = waveSwing * 2.0;

      if(pullLevel < SETTLE_PULL_MIN)
        pullLevel = SETTLE_PULL_MIN;

      if(pullLevel < settings.pullDeg * 1.5)
        pullLevel = settings.pullDeg * 1.5;

      if(moved > pullLevel)
      {
        if(!settlePullCandidate)
        {
          settlePullCandidate = true;
          settlePullStart = now;
        }

        settlePullAway =
          haveDir &&
          vDot(vSub(gravityUnit, oldest), pullDir) < 0;
      }
      else if(moved < pullLevel * 0.45)
      {
        settlePullCandidate = false;
      }
    }
  }

  if(
    settlePullCandidate &&
    now - settlePullStart >= SETTLE_PULL_TIME
  )
  {
    if(settlePullAway)
    {
      // Line went slack: only noted, the new position is learned anyway
      noteDropBack(false);
      settlePullCandidate = false;
    }
    else
    {
      startStrike("SLOW PULL");
      return;
    }
  }

  // ---------- Taking too long (fish still active, big waves) ----------

  if(
    now >= settleGraceUntil &&
    now - settleStart >= SETTLE_MAX_WAIT + (settleGraceUntil > settleStart ? settleGraceUntil - settleStart : 0)
  )
  {
    setRest(recentRest);

    fastFollowUntil = now + FAST_FOLLOW_MS;

    Serial.println();
    Serial.println("SETTLE TIMEOUT - USING AVERAGE POSITION");

    finishSettling();
    return;
  }

  // ---------- Learn new rest position ----------

  bool calm =
    knockEnergy < knockMean + (knockThreshold - knockMean) * 0.75 &&
    !knockActive;

  if(!calm)
  {
    // Movement restarted, do not accept yet
    resetSettleWindow();
    return;
  }

  if(!calmTimerRunning)
  {
    calmTimerRunning = true;
    calmStart = now;

    settleAnchor = gravityUnit;
    settleSum = vec(0, 0, 0);
    settleSamples = 0;
    settleMaxDev = 0;
  }

  settleSum = vAdd(settleSum, gravityUnit);
  settleSamples++;

  float dev = vAngleDeg(gravityUnit, settleAnchor);

  if(dev > settleMaxDev)
    settleMaxDev = dev;

  // Waves: wait at least one wave so they average out
  float tolerance = waveSwing * 2.5;

  if(tolerance < SETTLE_TOL_MIN)
    tolerance = SETTLE_TOL_MIN;

  unsigned long settleTime = SETTLE_TIME_MIN;

  if(waveSwing > 0.5 && wavePeriod > 0)
  {
    settleTime =
      (unsigned long)clampFloat(
        wavePeriod * 1000.0,
        SETTLE_TIME_MIN,
        SETTLE_TIME_MAX
      );
  }

  unsigned long elapsed = now - calmStart;

  float timeScore =
    clampFloat((float)elapsed / settleTime, 0, 1);

  float stabilityScore =
    clampFloat(1.0 - settleMaxDev / tolerance, 0, 1);

  settlingConfidence =
    (int)(timeScore * stabilityScore * 100.0);

  if(elapsed < settleTime)
    return;

  if(settleMaxDev >= tolerance || settleSamples == 0)
  {
    resetSettleWindow();
    return;
  }

  setRest(settleSum);

  Serial.println();
  Serial.println("NEW ROD POSITION LEARNED");

  finishSettling();
}

// =====================================================
// ONE READING
// =====================================================

void processSample()
{
  readSensor();
  updateSignals();

  switch(rodState)
  {
    case ST_LEARNING:
      learningStep();
      break;

    case ST_PULLTEST:
      pullTestStep();
      break;

    case ST_SETTLING:
      settlingStep();
      break;

    case ST_MONITORING:
      monitoringStep();
      break;

    case ST_PAUSED:
      dropWatch = false;
      pullBucket = 0;
      dropBucket = 0;
      settlingConfidence = 0;
      break;
  }

  updateRecording();
  updateWatchRecording();
  updateFight();
}

// =====================================================
// DASHBOARD
// =====================================================

const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>

<head>

<meta charset="UTF-8">

<meta name="viewport"
content="width=device-width,initial-scale=1">

<title>Laarif StrikeSense</title>

<link rel="icon" href="/logo.webp">

<style>

* {
  box-sizing:border-box;
}

html,
body {
  margin:0;
  width:100%;
  min-height:100%;
}

body {
  font-family:Arial,Helvetica,sans-serif;
  color:#eaf3ff;

  background:
    radial-gradient(
      circle at 40% -20%,
      #0d2a5c 0,
      #040b1c 42%,
      #000206 100%
    );

  padding:10px;
}

.app {
  width:100%;
  max-width:1550px;
  margin:auto;
}

/* HEADER */

.header {
  min-height:68px;
  display:grid;
  grid-template-columns:1fr auto 1fr;
  align-items:center;
  gap:12px;
  margin-bottom:9px;
}

.brand {
  display:flex;
  align-items:center;
  gap:12px;
  font-size:25px;
  font-weight:900;
  letter-spacing:-.7px;
}

.brandLogo {
  width:58px;
  height:58px;
  border-radius:50%;
  flex:0 0 auto;
  box-shadow:0 0 18px rgba(30,150,255,.55), 0 0 0 1px rgba(90,170,255,.35);
}

.brandName b {
  background:linear-gradient(90deg,#1e7bff,#22d3ff);
  -webkit-background-clip:text;
  background-clip:text;
  color:transparent;
}

/* PULL TEST POP-UPS */

.ptModal {
  position:fixed;
  inset:0;
  z-index:40;
  display:none;
  align-items:center;
  justify-content:center;
  padding:16px;
  background:rgba(1,4,12,.72);
  backdrop-filter:blur(3px);
  -webkit-backdrop-filter:blur(3px);
}

.ptModal.show {
  display:flex;
  animation:ptIn .25s ease-out;
}

@keyframes ptIn {
  from { opacity:0; transform:scale(.97); }
  to   { opacity:1; transform:none; }
}

.ptCard {
  width:min(460px, 100%);
  max-height:calc(100vh - 32px);
  overflow:auto;
  padding:18px;
  border-radius:18px;
  background:linear-gradient(180deg, #0f1d38, #071026);
  border:1px solid rgba(34,211,255,.35);
  box-shadow:0 0 40px rgba(30,150,255,.25);
  color:#eaf3ff;
}

.ptTitle {
  font-size:20px;
  font-weight:900;
  color:#ffba52;
  letter-spacing:.4px;
}

.ptLead {
  margin-top:4px;
  font-size:13px;
  color:#8ea6c8;
}

#ptCanvas {
  display:block;
  width:100%;
  height:220px;
  margin-top:12px;
  border-radius:12px;
  border:1px solid #15274a;
}

.ptSteps {
  margin:12px 0 0;
  padding:0;
  list-style:none;
  font-size:14px;
}

.ptSteps li {
  display:flex;
  gap:10px;
  align-items:flex-start;
  padding:5px 0;
  color:#c3d2ea;
}

.ptSteps b {
  flex:0 0 22px;
  height:22px;
  border-radius:50%;
  background:#0d2a5c;
  color:#22d3ff;
  font-size:12px;
  display:flex;
  align-items:center;
  justify-content:center;
}

.ptProgress {
  margin-top:12px;
  padding:10px 12px;
  border-radius:12px;
  background:#050b18;
  border:1px solid #15274a;
}

.ptBar {
  height:10px;
  margin-top:7px;
  border-radius:5px;
  background:#13213f;
  overflow:hidden;
}

#ptFill {
  height:100%;
  width:0;
  border-radius:5px;
  background:linear-gradient(90deg, #1e7bff, #22d3ff);
  transition:width .15s linear;
}

#ptFill.full {
  background:linear-gradient(90deg, #22d3ff, #7fe3ff);
}

#ptMsg {
  font-size:13px;
  font-weight:700;
  color:#eaf3ff;
}

.ptButtons {
  display:flex;
  gap:8px;
  margin-top:14px;
}

.ptButtons button {
  flex:1;
  padding:11px;
  border-radius:10px;
  border:1px solid #22406e;
  background:#0b1630;
  color:#eaf3ff;
  font-weight:900;
  font-size:13px;
  cursor:pointer;
}

.ptButtons button.main {
  background:#2f6bff;
  border-color:#2f6bff;
}

.ptCard.done {
  text-align:center;
}

.ptTick {
  width:86px;
  height:86px;
  margin:6px auto 8px;
  display:block;
}

.ptTick circle,
.ptTick path {
  fill:none;
  stroke:#22d3ff;
  stroke-width:3.5;
  stroke-linecap:round;
  stroke-linejoin:round;
}

.ptTick circle {
  stroke-dasharray:151;
  stroke-dashoffset:151;
  animation:ptDraw .6s ease-out forwards;
}

.ptTick path {
  stroke-dasharray:48;
  stroke-dashoffset:48;
  animation:ptDraw .35s .55s ease-out forwards;
}

.ptCard.skipped .ptTick circle,
.ptCard.skipped .ptTick path {
  stroke:#ffba52;
}

@keyframes ptDraw {
  to { stroke-dashoffset:0; }
}

#ptDoneTitle {
  font-size:21px;
  font-weight:900;
  color:#22d3ff;
}

.ptCard.skipped #ptDoneTitle {
  color:#ffba52;
}

#ptDoneText {
  margin-top:6px;
  font-size:14px;
  color:#c3d2ea;
  line-height:1.45;
}

/* START-UP SPLASH */

#splash {
  position:fixed;
  inset:0;
  z-index:50;
  display:flex;
  align-items:center;
  justify-content:center;
  background:radial-gradient(circle at 50% 45%, #0d2a5c 0, #020611 55%, #000 100%);
  transition:opacity .6s ease;
}

#splash img {
  width:min(78vw, 360px);
  height:auto;
  border-radius:50%;
  box-shadow:0 0 60px rgba(30,150,255,.45);
  animation:splashGlow 1.6s ease-in-out infinite alternate;
}

#splash.hide {
  opacity:0;
  pointer-events:none;
}

@keyframes splashGlow {
  from { box-shadow:0 0 30px rgba(30,150,255,.30); }
  to   { box-shadow:0 0 70px rgba(34,211,255,.60); }
}

.brand small {
  display:block;
  color:#6f86a8;
  font-size:11px;
  margin-top:2px;
}

#mainStatus {
  min-width:230px;
  text-align:center;
  border-radius:13px;
  padding:10px 18px;
  background:#071026;
  border:1px solid #15274a;
  font-size:23px;
  font-weight:900;
}

.rightHeader {
  text-align:right;
  color:#7189ad;
  font-size:11px;
}

.connected {
  color:#22d3ff;
}

#linkInfo {
  display:inline-flex;
  align-items:center;
  gap:6px;
  margin-top:3px;
  padding:3px 8px;
  border-radius:10px;
  background:#0d1a30;
  border:1px solid #1b2c4c;
  color:#dbe9ff;
  font-size:12px;
  font-weight:700;
}

.sigBars {
  display:inline-flex;
  align-items:flex-end;
  gap:2px;
  height:14px;
}

.sigBars i {
  display:block;
  width:4px;
  border-radius:2px;
  background:#2a3956;
}

.sigBars i:nth-child(1) { height:5px; }
.sigBars i:nth-child(2) { height:8px; }
.sigBars i:nth-child(3) { height:11px; }
.sigBars i:nth-child(4) { height:14px; }

.sigDbm {
  color:#8fa6c9;
  font-size:10px;
  font-weight:600;
}

.presetRow {
  display:flex;
  align-items:center;
  gap:6px;
  margin-bottom:6px;
}

.presetRow .pName {
  flex:1;
  text-align:left;
  background:#050b18;
  border:1px solid #16305a;
  color:#e3eeff;
  border-radius:10px;
  padding:9px 10px;
  font-size:12px;
  font-weight:800;
  cursor:pointer;
}

.presetRow .pName:disabled {
  color:#4d6182;
  font-weight:600;
  font-style:italic;
  border-style:dashed;
  cursor:default;
}

.presetRow .pName.def {
  border-color:#22406e;
  background:#0b1630;
}

.presetRow .pName small {
  display:block;
  color:#7189ad;
  font-size:9px;
  font-weight:600;
  font-style:normal;
  margin-top:2px;
}

.presetRow .pSave,
.presetRow .pDel,
.soundRow .play {
  border:1px solid #22406e;
  background:#0b1630;
  color:#c3d2ea;
  border-radius:10px;
  padding:9px 9px;
  font-size:10px;
  font-weight:900;
  cursor:pointer;
}

.presetRow .pDel.sure {
  border-color:#ff4d5e;
  color:#ff4d5e;
}

#presetNameIn {
  width:100%;
  box-sizing:border-box;
  background:#050b18;
  border:1px solid #22406e;
  border-radius:10px;
  color:#e3eeff;
  padding:9px 10px;
  font-size:12px;
  margin:2px 0 6px;
}

.soundRow {
  display:flex;
  align-items:center;
  gap:8px;
}

.soundRow span {
  flex:1;
  color:#c3d2ea;
  font-size:12px;
  font-weight:700;
}

.soundRow select {
  background:#0b1630;
  color:#e3eeff;
  border:1px solid #22406e;
  border-radius:8px;
  padding:6px 6px;
  font-size:11px;
  font-weight:700;
}

.soundRow .play {
  color:#22d3ff;
  padding:6px 10px;
}

#savedToast {
  position:fixed;
  left:50%;
  bottom:28px;
  transform:translateX(-50%);
  background:#0e2a1c;
  border:1px solid #3ddc84;
  color:#3ddc84;
  border-radius:20px;
  padding:9px 18px;
  font-size:13px;
  font-weight:900;
  z-index:9999;
  opacity:0;
  transition:opacity .25s;
  pointer-events:none;
}

#savedToast.show {
  opacity:1;
}

.audioState {
  margin-top:4px;
  font-size:10px;
}

#audioStatus {
  color:#ffba52;
}

.soundControls {
  display:flex;
  justify-content:flex-end;
  flex-wrap:wrap;
  gap:4px;
  margin-top:5px;
}

.soundButton {
  border:1px solid #22406e;
  background:#0b1630;
  color:#c3d2ea;
  border-radius:7px;
  padding:5px 7px;
  font-size:9px;
  font-weight:800;
  cursor:pointer;
}

.soundButton.enabled {
  color:#22d3ff;
  border-color:#1f8fd6;
  background:#082440;
}

/* MAIN */

.layout {
  height:calc(100vh - 98px);
  min-height:620px;
  display:grid;
  grid-template-columns:280px minmax(400px,1fr);
  gap:9px;
}

.sidebar {
  display:grid;
  grid-template-rows:auto auto 1fr;
  gap:9px;
  min-height:0;
  overflow:auto;
}

.card {
  background:rgba(6,13,30,.92);
  border:1px solid rgba(90,150,255,.16);
  border-radius:15px;
  padding:12px;
  box-shadow:0 10px 28px rgba(0,0,0,.28);
}

.cardHead {
  display:flex;
  justify-content:space-between;
  align-items:center;
  margin-bottom:9px;
}

.cardTitle {
  color:#8399bb;
  font-size:10px;
  font-weight:900;
  letter-spacing:1.5px;
  margin-bottom:9px;
}

.cardHead .cardTitle {
  margin-bottom:0;
}

.metrics {
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:6px;
}

.metric {
  background:#050b18;
  border:1px solid #0f1d38;
  border-radius:10px;
  padding:8px;
}

.metricLabel {
  color:#5f7799;
  font-size:9px;
  margin-bottom:4px;
}

.metricValue {
  font-weight:900;
  font-size:18px;
}

.green {
  color:#22d3ff !important;
}

.blue {
  color:#9b8cff !important;
}

.orange {
  color:#ffba52 !important;
}

.red {
  color:#ff5757 !important;
}

/* CONDITIONS */

.modeBadge {
  font-size:9px;
  font-weight:900;
  letter-spacing:1px;
  padding:3px 7px;
  border-radius:6px;
  color:#22d3ff;
  background:#082440;
  border:1px solid #1f8fd6;
}

.modeBadge.manual {
  color:#ffba52;
  background:#241a0c;
  border-color:#7a5a26;
}

.seaRow {
  display:flex;
  justify-content:space-between;
  align-items:center;
  background:#050b18;
  border:1px solid #0f1d38;
  border-radius:10px;
  padding:8px 10px;
}

.seaLabel {
  color:#5f7799;
  font-size:10px;
  font-weight:900;
  letter-spacing:1px;
}

.seaValue {
  font-size:18px;
  font-weight:900;
}

.cond {
  margin-top:8px;
}

.condTop {
  display:flex;
  justify-content:space-between;
  align-items:baseline;
  font-size:11px;
  color:#8298ba;
  margin-bottom:4px;
}

.condTop strong {
  color:#eaf3ff;
  font-size:13px;
}

.condNum {
  color:#5f7799;
  font-size:10px;
  margin-left:6px;
  font-weight:400;
}

.track {
  height:10px;
  border-radius:20px;
  overflow:hidden;
  background:#12203d;
}

.track.small {
  height:7px;
}

.condFill {
  width:0%;
  height:100%;
  border-radius:20px;
  background:#22d3ff;
  transition:width .5s, background .5s;
}

.seg {
  display:flex;
  gap:4px;
  margin-top:10px;
}

.segBtn {
  flex:1;
  border:1px solid #22406e;
  background:#0b1630;
  color:#c3d2ea;
  border-radius:7px;
  padding:6px 0;
  font-size:9px;
  font-weight:900;
  cursor:pointer;
}

.segBtn.on {
  color:#001226;
  background:#1fb6ff;
  border-color:#1fb6ff;
}

.details {
  white-space:pre-line;
  margin-top:9px;
  color:#5a7296;
  font-size:9px;
  line-height:1.5;
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:8px;
}

/* SCORES */

.scoreBlock {
  margin-top:9px;
}

.scoreTop {
  display:flex;
  justify-content:space-between;
  font-size:11px;
  color:#8298ba;
  margin-bottom:4px;
}

.scoreTop strong {
  color:white;
  font-size:16px;
}

#slowFill,
#dropFill {
  width:0%;
  height:100%;
  border-radius:20px;
  background:linear-gradient(90deg,#2f7bff,#22d3ff,#ffc147,#ff5151);
}

#settleFill {
  width:0%;
  height:100%;
  border-radius:20px;
  background:linear-gradient(90deg,#ff5151,#ffba4f,#22d3ff);
}

.dirLine {
  margin-top:9px;
  font-size:10px;
  color:#7189ad;
}

/* STATE */

.stateText {
  font-size:20px;
  font-weight:900;
}

.stateSub {
  color:#7189ad;
  font-size:11px;
  margin-top:5px;
  line-height:1.4;
}

.controls {
  display:flex;
  gap:6px;
  flex-wrap:wrap;
  margin-top:10px;
}

button.action {
  flex:1;
  border:0;
  min-width:70px;
  padding:9px 10px;
  border-radius:9px;
  font-size:11px;
  font-weight:900;
  cursor:pointer;
}

.details button.action {
  flex:0 0 auto;
  min-width:0;
  padding:6px 9px;
  font-size:9px;
}

.relearn {
  background:#2f6bff;
  color:#fff;
}

.watch {
  background:#e8f1ff;
  color:#111;
}

.landCard {
  border:1px solid #ff8a3d;
  border-radius:16px;
  padding:12px;
  text-align:center;
  margin-bottom:12px;
  background:rgba(255,138,61,.06);
}

.landCard.done { border-color:#3ddc84; background:rgba(61,220,132,.06); }
.landCard.lost { border-color:#ff5d6c; background:rgba(255,93,108,.06); }
.landTitle { color:#ff8a3d; font-weight:800; letter-spacing:1px; font-size:14px; }
.landCard.done .landTitle { color:#3ddc84; }
.landCard.lost .landTitle { color:#ff8a94; }
.landTime { font-size:64px; font-weight:800; font-variant-numeric:tabular-nums; line-height:1.1; }
.landSub { color:#8fa0bf; font-size:13px; }

.fishout {
  background:#3ddc84 !important;
  color:#05210f !important;
}

.lost {
  background:transparent !important;
  border:2px solid #ff5d6c !important;
  color:#ff8a94 !important;
}

button.bigsym {
  display:none;
  flex-direction:column;
  align-items:center;
  justify-content:center;
  gap:4px;
  min-height:150px;
  font-size:18px;
  font-weight:900;
}

button.bigsym svg { width:100%; max-width:110px; height:auto; }
button.bigsym small { font-size:12px; font-weight:700; opacity:.8; }

/* phone: the timer on its own row, the three buttons side by side under it */
@media (max-width:600px) {
  #landCard { flex-basis:100%; }
  button.bigsym { flex:1 1 0; min-width:0; min-height:130px; font-size:15px; }
}

.nofish {
  background:transparent !important;
  border:2px solid #6b7790 !important;
  color:#a9bbd6 !important;
}

.reel {
  background:#ee4747;
  color:#fff;
}

.ready {
  background:#1fb6ff;
  color:#001226;
}

.skip {
  background:#13213f;
  color:#c3d2ea;
}

/* GRAPH */

.graphCard {
  min-width:0;
  min-height:0;
  display:flex;
  flex-direction:column;
}

.graphHeader {
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:10px;
  margin-bottom:5px;
  flex-wrap:wrap;
}

.graphTitle {
  font-weight:900;
  font-size:17px;
}

.graphSub {
  color:#62799d;
  font-size:10px;
}

.graphRight {
  display:flex;
  align-items:center;
  gap:12px;
  flex-wrap:wrap;
}

.legend {
  display:flex;
  gap:14px;
  color:#7b90b2;
  font-size:10px;
}

.dot {
  display:inline-block;
  width:8px;
  height:8px;
  border-radius:50%;
  margin-right:4px;
}

.moveDot {
  background:#22d3ff;
}

.tiltDot {
  background:#9b8cff;
}

.viewToggle {
  display:flex;
  gap:4px;
}

.soundButton:disabled {
  opacity:.4;
  cursor:default;
}

.graphNumbers {
  display:grid;
  grid-template-columns:repeat(4,1fr);
  gap:6px;
  margin:5px 0 6px;
}

.gNumber {
  padding:7px 9px;
  background:#050b18;
  border:1px solid #0f1d38;
  border-radius:9px;
  transition:border-color .2s, box-shadow .2s;
}

.gNumber.knockFlash {
  border-color:#22d3ff;
  box-shadow:0 0 14px rgba(34,211,255,.45);
}

.gLabel {
  color:#60779b;
  font-size:8px;
}

.gValue {
  font-size:17px;
  font-weight:900;
  margin-top:2px;
}

.canvasWrap {
  flex:1;
  min-height:0;
  background:#030814;
  border:1px solid #0f1d38;
  border-radius:12px;
  overflow:hidden;
}

canvas {
  display:block;
  width:100%;
  height:100%;
}

/* STRIKE FLASH */

body.strikeFlash {
  animation:strikeFlashAnimation 0.65s infinite;
}

@keyframes strikeFlashAnimation {
  0% { box-shadow:inset 0 0 0 0 rgba(255,40,40,0); }
  50% { box-shadow:inset 0 0 90px 0 rgba(255,40,40,.20); }
  100% { box-shadow:inset 0 0 0 0 rgba(255,40,40,0); }
}

/* RUNNING TIME */

.uptime {
  display:flex;
  justify-content:flex-end;
  flex-wrap:wrap;
  gap:4px;
  margin-top:6px;
}

.timeChip {
  display:flex;
  align-items:baseline;
  gap:5px;
  padding:4px 8px;
  border-radius:8px;
  background:#071026;
  border:1px solid #15274a;
  font-size:9px;
  font-weight:900;
  letter-spacing:.8px;
  color:#60779b;
}

.timeChip b {
  color:#eaf3ff;
  font-size:12px;
  letter-spacing:0;
  font-variant-numeric:tabular-nums;
}

.timeChip.armed b {
  color:#22d3ff;
}

.batIcon {
  position:relative;
  display:inline-block;
  width:20px;
  height:10px;
  border:1.5px solid #60779b;
  border-radius:3px;
  align-self:center;
}

.batIcon::after {
  content:"";
  position:absolute;
  right:-4px;
  top:2px;
  width:2px;
  height:4px;
  border-radius:0 1px 1px 0;
  background:#60779b;
}

#batFill {
  position:absolute;
  left:1px;
  top:1px;
  bottom:1px;
  width:0;
  border-radius:1px;
  background:#22d3ff;
}

.timeChip.batOk b { color:#22d3ff; }
.timeChip.batMid b { color:#ffba52; }
.timeChip.batMid #batFill { background:#ffba52; }
.timeChip.batLow { border-color:#ff4d5e; animation:batBlink 1s step-end infinite; }
.timeChip.batLow b { color:#ff4d5e; }
.timeChip.batLow #batFill { background:#ff4d5e; }

@keyframes batBlink {
  50% { border-color:#15274a; }
}

/* SETTINGS DRAWER */

.drawer {
  position:fixed;
  top:0;
  right:0;
  bottom:0;
  width:360px;
  max-width:100%;
  z-index:20;
  background:rgba(4,10,24,.97);
  border-left:1px solid rgba(90,150,255,.22);
  box-shadow:-20px 0 50px rgba(0,0,0,.5);
  padding:14px;
  overflow-y:auto;
  transform:translateX(105%);
  transition:transform .25s ease;
}

.drawer.open {
  transform:translateX(0);
}

.drawerHead {
  display:flex;
  justify-content:space-between;
  align-items:center;
  margin-bottom:10px;
}

.drawerTitle {
  font-size:17px;
  font-weight:900;
}

.closeBtn {
  border:1px solid #22406e;
  background:#0b1630;
  color:#c3d2ea;
  border-radius:8px;
  width:32px;
  height:32px;
  font-size:16px;
  cursor:pointer;
}

.liveBox {
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:6px;
  margin-bottom:10px;
}

.liveItem {
  background:#050b18;
  border:1px solid #0f1d38;
  border-radius:9px;
  padding:7px 9px;
}

.liveItem .gLabel {
  margin-bottom:2px;
}

.liveItem b {
  font-size:14px;
}

.setGroup {
  color:#8399bb;
  font-size:10px;
  font-weight:900;
  letter-spacing:1.5px;
  margin:14px 0 6px;
}

.setRow {
  background:#050b18;
  border:1px solid #0f1d38;
  border-radius:10px;
  padding:9px 10px;
  margin-bottom:6px;
}

.setTop {
  display:flex;
  justify-content:space-between;
  align-items:baseline;
  font-size:12px;
  font-weight:800;
}

.setTop strong {
  color:#22d3ff;
  font-size:14px;
  font-variant-numeric:tabular-nums;
}

.setHelp {
  color:#5f7799;
  font-size:10px;
  margin-top:3px;
  line-height:1.35;
}

.setRow input[type=range] {
  width:100%;
  margin:8px 0 2px;
  accent-color:#1fb6ff;
}

.setEnds {
  display:flex;
  justify-content:space-between;
  color:#4c6286;
  font-size:9px;
}

.switchRow {
  display:flex;
  justify-content:space-between;
  align-items:center;
}

.switch {
  border:1px solid #22406e;
  background:#0b1630;
  color:#c3d2ea;
  border-radius:20px;
  padding:5px 12px;
  font-size:10px;
  font-weight:900;
  cursor:pointer;
  min-width:52px;
}

.switch.on {
  color:#001226;
  background:#1fb6ff;
  border-color:#1fb6ff;
}

.saveState {
  font-size:10px;
  color:#ffba52;
  margin:12px 0 6px;
}

.saveState.saved {
  color:#22d3ff;
}

/* MOBILE */

/* WATCH LINK, MY STRIKES, PHONE + WATCH / WATCH ONLY (phone app) */

#watchInfo {
  display:none;
  align-items:center;
  gap:6px;
  margin-top:3px;
  padding:3px 8px;
  border-radius:10px;
  background:#0d1a30;
  border:1px solid #1b2c4c;
  color:#dbe9ff;
  font-size:12px;
  font-weight:700;
}

#watchInfo.show {
  display:inline-flex;
}

#watchVia {
  font-weight:800;
}

/* rod / watch lines: symbol + name, always one line */
#linkInfo, #watchInfo { white-space:nowrap; }
.whoName { display:inline-flex; align-items:center; gap:4px; font-weight:900; color:#eaf3ff; }
.whoName svg.who { width:15px; height:15px; }
#watchInfo.off .whoName { color:#8fa0bf; }
.viaDim { color:#8fa6c9; font-weight:700; font-size:11px; }

.appOnly {
  display:none !important;
}

body.inApp .appOnly {
  display:inline-block !important;
}

body.inApp #whoBox.appOnly {
  display:block !important;
}

#whoBox {
  margin:6px 0 2px auto;
  max-width:330px;
}

.whoSeg {
  display:flex;
  background:#0b1428;
  border:1px solid #24324f;
  border-radius:11px;
  padding:3px;
}

.whoSeg button {
  flex:1;
  border:0;
  background:none;
  color:#8ea6c8;
  border-radius:8px;
  padding:7px 4px;
  font-size:12px;
  font-weight:800;
  cursor:pointer;
}

.whoSeg button.on {
  background:linear-gradient(90deg,#1e7bff,#22d3ff);
  color:#04121a;
}

.whoNote {
  margin-top:4px;
  color:#a9bbd6;
  font-size:10.5px;
  line-height:1.35;
}

.woBanner {
  display:none;
  margin:6px 0 2px auto;
  max-width:330px;
  padding:8px 10px;
  border-radius:11px;
  background:#3a2a08;
  border:1px solid #ffba52;
  color:#ffd38a;
  font-size:11.5px;
  font-weight:700;
  text-align:left;
  line-height:1.35;
}

body.watchOnly .woBanner {
  display:block;
}

body.watchOnly .layout {
  opacity:.35;
  pointer-events:none;
}

@media(max-width:600px) {

  #whoBox,
  .woBanner {
    margin-left:auto;
    margin-right:auto;
  }
}

@media(max-width:850px) {

  .layout {
    height:auto;
    display:flex;
    flex-direction:column;
  }

  .sidebar {
    display:grid;
    grid-template-columns:1fr 1fr;
    overflow:visible;
  }

  .sidebar .stateCard {
    grid-column:1 / -1;
  }

  .graphCard {
    height:460px;
  }
}

@media(max-width:600px) {

  .uptime {
    justify-content:center;
  }

  .drawer {
    top:auto;
    width:100%;
    height:62vh;
    border-left:0;
    border-top:1px solid rgba(90,150,255,.22);
    border-radius:16px 16px 0 0;
    transform:translateY(105%);
  }

  .drawer.open {
    transform:translateY(0);
  }

  .header {
    grid-template-columns:1fr;
    text-align:center;
  }

  .brand,
  .rightHeader {
    text-align:center;
  }

  .soundControls {
    justify-content:center;
  }

  .sidebar {
    display:flex;
    flex-direction:column;
  }

  .graphNumbers {
    grid-template-columns:1fr 1fr;
  }
}

</style>

</head>

<body>

<div id="ptModal" class="ptModal">
<div class="ptCard">
<div class="ptTitle">PULL TEST</div>
<div class="ptLead">The rod needs to learn which way is "toward the water". Do this once:</div>
<canvas id="ptCanvas"></canvas>
<ol class="ptSteps">
<li><b>1</b><span>Hold the line in front of the rod tip.</span></li>
<li><b>2</b><span>Pull it <strong>gently</strong> toward the sea for about 1 second, until the bar is full.</span></li>
<li><b>3</b><span>Let go. The rod does the rest.</span></li>
</ol>
<div class="ptProgress">
<div id="ptMsg">Waiting for your pull…</div>
<div class="ptBar"><div id="ptFill"></div></div>
</div>
<div class="ptButtons">
<button onclick="skipPull()">SKIP</button>
<button class="main" onclick="hidePullTest()">OK, I'M DOING IT</button>
</div>
</div>
</div>

<div id="ptDone" class="ptModal">
<div class="ptCard done" id="ptDoneCard">
<svg class="ptTick" viewBox="0 0 52 52"><circle cx="26" cy="26" r="24"/><path id="ptDonePath" d="M14 27l8 8 16-17"/></svg>
<div id="ptDoneTitle">DIRECTION LEARNED</div>
<div id="ptDoneText"></div>
<div class="ptButtons"><button class="main" onclick="closePullDone()">OK</button></div>
</div>
</div>

<div id="splash"><img src="/logo.webp" alt="Laarif StrikeSense"></div>

<div class="app">

<!-- HEADER -->

<div class="header">

<div class="brand">
<img class="brandLogo" src="/logo.webp" alt="">
<div class="brandName">
Laarif <b>StrikeSense</b>
<small>SMART FISHING STRIKE DETECTOR</small>
</div>
</div>

<div id="mainStatus" class="orange">
LEARNING
</div>

<div class="rightHeader">

<span class="connected" id="connState">&#9679; CONNECTED</span>
<br>
<span id="linkInfo"><span class="whoName"><svg class="who" viewBox="0 0 24 24"><path d="M3 21 L19 4" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/><circle cx="7" cy="17" r="2.6" fill="none" stroke="currentColor" stroke-width="2"/><path d="M19 4 Q22 9 21 15 q1 3 -1.5 3" stroke="currentColor" stroke-width="1.4" fill="none"/></svg>Rod</span><span id="linkName">Bluetooth</span><span class="sigBars" id="sigBars"><i></i><i></i><i></i><i></i></span><span class="sigDbm" id="sigDbm"></span></span>
<script>
// Called by the phone app every second: ready = linked to the rod, rssi = dBm (0 = unknown)
function setLink(ready, rssi)
{
  var n = ready && rssi ? (rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1) : 0;
  var c = '#3ddc84';                    // Bluetooth bars: green (how many = how strong)
  var b = document.querySelectorAll('#sigBars i');

  for(var i = 0; i < b.length; i++)
    b[i].style.background = i < n ? c : '';

  document.getElementById('linkName').textContent = ready ? 'Bluetooth' : 'Reconnecting\u2026';
  document.getElementById('linkName').style.color = ready ? '#22a8ff' : '#ffba52';
  document.getElementById('sigDbm').textContent = ready && rssi ? rssi + ' dBm' : '';
}
</script>
<br>
<span id="watchInfo"><span class="whoName" id="watchWho"><svg class="who" viewBox="0 0 24 24"><rect x="8" y="1.5" width="8" height="4" rx="1" fill="currentColor"/><rect x="8" y="18.5" width="8" height="4" rx="1" fill="currentColor"/><circle cx="12" cy="12" r="6.6" fill="none" stroke="currentColor" stroke-width="2.2"/><path d="M12 8.5 V12 L14.5 13.5" stroke="currentColor" stroke-width="1.8" fill="none" stroke-linecap="round"/></svg>Watch</span><span id="watchVia">&ndash;</span><span class="sigBars" id="wBars"><i></i><i></i><i></i><i></i></span></span>
<div id="crowdTip" style="display:none;margin:5px auto 0;max-width:330px;color:#ffba52;font-size:11px;font-weight:700"></div>
<script>
// How the watch is connected to the rod (rod 2.3+: "wlink" in /data)
function showWatch(d)
{
  var box = document.getElementById('watchInfo');

  if(d.wlink === undefined)
  {
    box.className = '';
    return;
  }

  var lora = d.wlink == 'lora', bt = d.wlink == 'bt';
  var c = bt ? '#3ddc84' : lora ? '#ffc928' : '#6b7790';     // bars: green Bluetooth, yellow LoRa, grey none
  var word = bt ? '#22a8ff' : lora ? '#ffc928' : '#6b7790';  // the word: blue Bluetooth, yellow LoRa
  var b = document.querySelectorAll('#wBars i');

  for(var i = 0; i < b.length; i++)
    b[i].style.background = i < d.wbars ? c : '';

  var via = document.getElementById('watchVia');
  var QUAL = ['Clear', 'Good', 'Busy', 'Congested'];
  via.innerHTML = bt ? '<b style="color:#22a8ff">Bluetooth</b>' :
    lora ? '<b style="color:#ffc928">LoRa</b> <span class="viaDim">' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined && d.lqual >= 2 ? ' &middot; ' + QUAL[d.lqual] : '') + '</span>' : 'not connected';

  // many rods on the channel: Bluetooth gives the fastest alerts
  var tip = document.getElementById('crowdTip');
  tip.style.display = lora && d.lqual == 3 ? '' : 'none';
  tip.textContent = 'Many devices nearby (' + (d.lrods || 0) + ' rods). For the fastest alerts use Bluetooth only (Settings).';
  via.style.color = bt || lora ? '#eaf3ff' : word;
  document.getElementById('wBars').style.display = bt || lora ? '' : 'none';
  box.className = bt || lora ? 'show' : 'show off';
}
</script>

<div class="audioState">
AUDIO: <span id="audioStatus">OFF</span>
</div>

<div class="soundControls">
<button class="soundButton" onclick="openSettings()">&#9881; SETTINGS</button>
</div>

<div class="woBanner" id="woBanner">&#128245; Phone off the rod: <u>WATCH ONLY</u>. No alarms on the phone. It checks every 2 min that the watch is still there.<br><span id="woInfo"></span></div>

<div class="appOnly" id="whoBox">
<div class="whoSeg">
<button id="whoBoth" class="on" onclick="setWho(false)">Phone + watch</button>
<button id="whoWatch" onclick="setWho(true)">Watch only</button>
</div>
<div class="whoNote">Watch only saves phone battery and a little rod battery. Live data and strike alarms are just as fast in both modes.</div>
</div>
<script>
if(window.StrikeApp)
  document.body.classList.add('inApp');

var watchOnly = false;

function openStrikes()
{
  if(window.StrikeApp && StrikeApp.openStrikes)
    StrikeApp.openStrikes();
}

function setWho(only)
{
  if(only == watchOnly || !window.StrikeApp || !StrikeApp.setWatchOnly)
    return;

  if(only && !confirm('Watch only: the phone leaves the rod and gives NO alarms.\nThe watch alerts you. The phone checks every 2 min that the watch is still there.\n\nOK?'))
    return;

  StrikeApp.setWatchOnly(only);
  setWatchOnlyView(only, '');
}

// Called by the phone app every second
function setWatchOnlyView(only, info)
{
  watchOnly = only;
  document.body.classList.toggle('watchOnly', only);
  document.getElementById('whoBoth').className = only ? '' : 'on';
  document.getElementById('whoWatch').className = only ? 'on' : '';
  document.getElementById('woInfo').textContent = info || '';

  if(only)
  {
    el('mainStatus').innerText = 'WATCH ONLY';
    el('connState').innerHTML = '&#9679; PHONE OFF THE ROD';
    el('connState').className = '';
    document.getElementById('linkInfo').style.display = 'none';
    document.getElementById('watchInfo').style.display = 'none';     // the banner says the last check
    stopContinuousAlarm();
  }
  else
  {
    document.getElementById('linkInfo').style.display = '';
    document.getElementById('watchInfo').style.display = '';
  }
}
</script>

<div class="uptime">
<span class="timeChip" id="batChip" title="Battery"><i class="batIcon"><i id="batFill"></i></i><b id="batText">&ndash;</b></span>
<span class="timeChip">&#9201; ON <b id="upTime">0:00:00</b></span>
<span class="timeChip armed">ARMED <b id="armedTime">&ndash;</b></span>
<span class="timeChip">TOTAL <b id="totalTime">0h</b></span>
</div>

</div>

</div>


<div class="layout">

<div class="sidebar">

<!-- CONDITIONS -->

<div class="card">

<div class="cardHead">
<div class="cardTitle">CONDITIONS</div>
<div class="modeBadge" id="modeBadge">AUTO</div>
</div>

<div class="seaRow">
<span class="seaLabel">SEA STATE</span>
<span class="seaValue orange" id="seaValue">&ndash;</span>
</div>

<div class="cond">
<div class="condTop">
<span>&#127754; WAVES</span>
<span><strong id="waveLabel">&ndash;</strong><span class="condNum" id="waveNum"></span></span>
</div>
<div class="track small"><div class="condFill" id="waveFill"></div></div>
</div>

<div class="cond">
<div class="condTop">
<span>&#128168; WIND</span>
<span><strong id="windLabel">&ndash;</strong><span class="condNum" id="windNum"></span></span>
</div>
<div class="track small"><div class="condFill" id="windFill"></div></div>
</div>

<div class="cond">
<div class="condTop">
<span>&#8600; TIDE</span>
<span><strong id="tideLabel">&ndash;</strong><span class="condNum" id="tideNum"></span></span>
</div>
<div class="track small"><div class="condFill" id="tideFill"></div></div>
</div>

<div class="seg">
<button class="segBtn" id="seg_auto" onclick="setSea('auto')">AUTO</button>
<button class="segBtn" id="seg_calm" onclick="setSea('calm')">CALM</button>
<button class="segBtn" id="seg_mod" onclick="setSea('mod')">MOD</button>
<button class="segBtn" id="seg_rough" onclick="setSea('rough')">ROUGH</button>
</div>

<div class="details">
<span id="details">&ndash;</span>
<button class="action relearn" onclick="relearn()">RELEARN</button>
<button class="action relearn" onclick="teachDir()">NEW PULL DIRECTION</button>
</div>

</div>


<!-- DETECTION -->

<div class="card">

<div class="cardTitle">DETECTION</div>

<div class="metrics">

<div class="metric">
<div class="metricLabel">KNOCK</div>
<div class="metricValue green" id="energy">0.000</div>
</div>

<div class="metric">
<div class="metricLabel">KNOCK LIMIT</div>
<div class="metricValue" id="knockThr">0.000</div>
</div>

<div class="metric">
<div class="metricLabel">TILT &deg;</div>
<div class="metricValue blue" id="tilt">0.0</div>
</div>

<div class="metric">
<div class="metricLabel">KNOCKS (4s)</div>
<div class="metricValue" id="knockCount">0</div>
</div>

</div>

<div class="scoreBlock">
<div class="scoreTop">
<span>SLOW PULL</span>
<strong id="slowScore">0%</strong>
</div>
<div class="track"><div id="slowFill"></div></div>
</div>

<div class="scoreBlock">
<div class="scoreTop">
<span>DROP-BACK</span>
<strong id="dropScore">0%</strong>
</div>
<div class="track"><div id="dropFill"></div></div>
</div>

<div class="dirLine" id="dirLine">Direction: &ndash;</div>

</div>


<!-- STATE -->

<div class="card stateCard">

<div class="cardTitle">ROD STATE</div>

<div class="stateText" id="stateText">LEARNING</div>

<div class="stateSub" id="stateSub">Learning environment.</div>

<div class="scoreBlock">
<div class="scoreTop">
<span>SETTLING CONFIDENCE</span>
<strong id="settleScore">0%</strong>
</div>
<div class="track"><div id="settleFill"></div></div>
</div>

<div class="controls">
<button id="watchButton" class="action watch" onclick="watching()" style="display:none">I'M WATCHING</button>
<button id="reelButton" class="action reel" onclick="reelIn()" style="display:none">REEL IN</button>
<div id="landCard" class="landCard" style="display:none">
<div class="landTitle" id="landTitle">FIGHT</div>
<div class="landTime" id="landTime">0:00</div>
<div class="landSub" id="landSub">from REEL IN</div>
<button id="landPhotos" class="action fishout" onclick="addPhotos()" style="display:none;margin-top:10px">&#128247; ADD PHOTOS</button>
</div>
<button id="fishOutButton" class="action fishout bigsym" onclick="fishOut()" style="display:none"><svg viewBox="0 0 120 80"><path d="M10 40 Q40 8 78 26 L100 12 L94 40 L100 68 L78 54 Q40 72 10 40 Z" fill="#05210f" opacity=".9"/><circle cx="28" cy="36" r="4" fill="#3ddc84"/><circle cx="92" cy="22" r="17" fill="#05210f" stroke="#3ddc84" stroke-width="3"/><path d="M84 22 l6 6 l11 -12" stroke="#3ddc84" stroke-width="4.5" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg><span>FISH OUT</span><small>caught it</small></button>
<button id="lostButton" class="action lost bigsym" onclick="lostFish()" style="display:none"><svg viewBox="0 0 120 80"><path d="M58 4 L58 30" stroke="#ff8a94" stroke-width="3"/><path d="M58 36 L58 44" stroke="#ff8a94" stroke-width="3" stroke-dasharray="3 3"/><path d="M52 31 l12 4 M52 37 l12 -4" stroke="#ff5d6c" stroke-width="3" stroke-linecap="round"/><path d="M16 62 Q38 44 62 56 L76 48 L72 62 L76 76 L62 68 Q38 80 16 62 Z" fill="none" stroke="#ff8a94" stroke-width="3"/><path d="M84 52 q8 -4 14 0 M84 62 q10 0 16 4" stroke="#ff8a94" stroke-width="2" fill="none" stroke-linecap="round"/></svg><span>LOST IT</span><small>it got off</small></button>
<button id="noFishButton" class="action nofish bigsym" onclick="noFish()" style="display:none"><svg viewBox="0 0 120 80"><path d="M60 4 L60 44 Q60 62 46 62 Q34 62 34 50" stroke="#a9bbd6" stroke-width="4" fill="none" stroke-linecap="round"/><path d="M34 50 l-5 6" stroke="#a9bbd6" stroke-width="4" stroke-linecap="round"/><circle cx="86" cy="44" r="16" fill="none" stroke="#a9bbd6" stroke-width="3"/><path d="M75 55 L97 33" stroke="#a9bbd6" stroke-width="3"/></svg><span>NO FISH</span><small>false alarm</small></button>
<button id="readyButton" class="action ready" onclick="ready()" style="display:none">READY</button>
<button id="skipButton" class="action skip" onclick="skipPull()" style="display:none">SKIP</button>
</div>

</div>

</div>


<!-- GRAPH -->

<div class="card graphCard">

<div class="graphHeader">

<div>
<div class="graphTitle" id="graphTitle">LIVE ROD ACTIVITY</div>
<div class="graphSub" id="graphSub">Top: knock energy &middot; Bottom: tilt (+ toward water)</div>
</div>

<div class="graphRight">

<div class="legend">
<span><span class="dot moveDot"></span>Knock</span>
<span><span class="dot tiltDot"></span>Tilt</span>
</div>

<div class="viewToggle">
<button class="soundButton enabled" id="viewLive" onclick="setView('live')">LIVE</button>
<button class="soundButton" id="viewRod" onclick="setView('rod')">LIVE ROD</button>
<button class="soundButton" id="viewStrike" onclick="setView('strike')" disabled>LAST STRIKE</button>
<button class="soundButton" id="view3d" onclick="setView('3d')">3D NOISE</button>
<button class="soundButton" id="view3dwave" onclick="setView('3dwave')">3D WAVES</button>
<button class="soundButton" id="view3dwind" onclick="setView('3dwind')">3D WIND</button>
</div>

</div>

</div>

<div class="graphNumbers">

<div class="gNumber" id="knockBox">
<div class="gLabel">KNOCK</div>
<div class="gValue green" id="graphEnergy">0.000</div>
</div>

<div class="gNumber">
<div class="gLabel">TILT &deg;</div>
<div class="gValue blue" id="graphTilt">0.0</div>
</div>

<div class="gNumber">
<div class="gLabel">SLOW PULL</div>
<div class="gValue" id="graphSlow">0%</div>
</div>

<div class="gNumber">
<div class="gLabel">LAST STRIKE</div>
<div class="gValue" id="lastStrike">NONE</div>
</div>

</div>

<div class="canvasWrap">
<canvas id="graph"></canvas>
</div>

</div>

</div>

</div>


<script>

// =====================================================
// HELPER
// =====================================================

function el(id)
{
  return document.getElementById(id);
}

// =====================================================
// AUDIO
// =====================================================

let soundEnabled = false;
let audioContext = null;
let lastPlayedStrikeID = 0;
let alarmTimer = null;
let activeAlarmType = "";
let wakeLock = null;

// WAV fallback for browsers without Web Audio

function makeToneWav(frequency, durationMs)
{
  const sampleRate = 8000;
  const samples = Math.floor(sampleRate * durationMs / 1000);
  const buffer = new ArrayBuffer(44 + samples * 2);
  const view = new DataView(buffer);

  function writeString(offset, text)
  {
    for(let i = 0; i < text.length; i++)
      view.setUint8(offset + i, text.charCodeAt(i));
  }

  writeString(0, "RIFF");
  view.setUint32(4, 36 + samples * 2, true);
  writeString(8, "WAVE");
  writeString(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeString(36, "data");
  view.setUint32(40, samples * 2, true);

  const amplitude = 12000;

  for(let i = 0; i < samples; i++)
  {
    const phase = (i * frequency / sampleRate) % 1;
    view.setInt16(44 + i * 2, phase < 0.5 ? amplitude : -amplitude, true);
  }

  const bytes = new Uint8Array(buffer);
  let binary = "";

  for(let i = 0; i < bytes.length; i++)
    binary += String.fromCharCode(bytes[i]);

  return "data:audio/wav;base64," + btoa(binary);
}

function fallbackTone(frequency, durationMs, delayMs)
{
  setTimeout(() =>
  {
    try
    {
      const sound = new Audio(makeToneWav(frequency, durationMs));
      sound.volume = 1.0;
      sound.play().catch(() => {});
    }
    catch(e)
    {
    }
  }, delayMs);
}

function webAudioTone(frequency, durationMs, delayMs)
{
  const start = audioContext.currentTime + delayMs / 1000;
  const duration = durationMs / 1000;

  const oscillator = audioContext.createOscillator();
  const gain = audioContext.createGain();

  oscillator.type = "square";
  oscillator.frequency.setValueAtTime(frequency, start);

  gain.gain.setValueAtTime(0.001, start);
  gain.gain.linearRampToValueAtTime(0.30, start + 0.015);
  gain.gain.setValueAtTime(0.30, start + Math.max(0.02, duration - 0.04));
  gain.gain.linearRampToValueAtTime(0.001, start + duration);

  oscillator.connect(gain);
  gain.connect(audioContext.destination);

  oscillator.start(start);
  oscillator.stop(start + duration + 0.03);
}

function tone(frequency, duration, delay)
{
  if(!soundEnabled)
    return;

  if(audioContext && audioContext.state === "running")
    webAudioTone(frequency, duration, delay);
  else
    fallbackTone(frequency, duration, delay);
}

// Keep the phone screen on while the dashboard is open
async function keepScreenOn()
{
  try
  {
    if("wakeLock" in navigator)
      wakeLock = await navigator.wakeLock.request("screen");
  }
  catch(e)
  {
  }
}

document.addEventListener("visibilitychange", () =>
{
  if(document.visibilityState === "visible" && soundEnabled)
    keepScreenOn();
});

// Sound is always on: started when the page opens (the app allows it),
// and again at the first touch in a normal browser (it needs one)
window.addEventListener("load", () => enableSound());
document.addEventListener("pointerdown", () =>
{
  if(!soundEnabled || (audioContext && audioContext.state !== "running"))
    enableSound();
}, { passive: true });

async function enableSound()
{
  try
  {
    if(!audioContext)
    {
      const AudioClass = window.AudioContext || window.webkitAudioContext;

      if(AudioClass)
        audioContext = new AudioClass();
    }

    if(audioContext)
    {
      try
      {
        await audioContext.resume();
      }
      catch(e)
      {
      }
    }

    soundEnabled = true;

    el("audioStatus").innerText = "READY";
    el("audioStatus").style.color = "#22d3ff";
    keepScreenOn();
  }
  catch(e)
  {
    soundEnabled = true;
    el("audioStatus").innerText = "FALLBACK";
    fallbackTone(900, 300, 0);
  }
}

// =====================================================
// ALARM PATTERNS
// =====================================================

function vibrate(pattern)
{
  if(navigator.vibrate)
    navigator.vibrate(pattern);
}

function sharpPattern()
{
  tone(1350, 110, 0);
  tone(1750, 110, 150);
  tone(1350, 110, 300);
  tone(1850, 180, 450);
  vibrate([100, 60, 100, 60, 180]);
}

function slowPattern()
{
  tone(430, 500, 0);
  tone(600, 600, 650);
  vibrate([450, 200, 550]);
}

function repeatedPattern()
{
  tone(850, 140, 0);
  tone(850, 140, 200);
  tone(1050, 180, 400);
  vibrate([100, 90, 100, 90, 160]);
}

function dropPattern()
{
  tone(900, 300, 0);
  tone(650, 300, 380);
  tone(450, 450, 760);
  vibrate([300, 80, 300, 80, 450]);
}

// Single knock: one short beep, no alarm
function knockPattern()
{
  playSoundFile(soundFor("knock"), () => tone(1250, 70, 0));
  vibrate(40);
}

// ---------- fishing alarm sounds (files from the phone app) ----------

const SOUND_NAMES = {
  reel_scream: "Screaming reel",
  reel_clicks: "Reel clicks",
  rod_bell: "Rod bell",
  falling_tone: "Falling tone"
};

const SOUND_DEFAULT = { strike: "reel_scream", slow: "reel_clicks", knock: "rod_bell", drop: "falling_tone" };
const SOUND_VIBRATE = {
  strike: [300, 100, 300, 100, 600],
  slow: [450, 200, 550],
  knock: [80, 80, 80],
  drop: [500, 150, 250]
};

const audioFiles = {};
let soundFilesOk = true;

function soundFor(cat)
{
  let v = null;

  try { v = localStorage.getItem("snd_" + cat); } catch(e) {}

  return SOUND_NAMES[v] ? v : SOUND_DEFAULT[cat];
}

function alarmCategory(type)
{
  if(type === "SLOW PULL") return "slow";
  if(type === "REPEATED BITE") return "knock";
  if(type.startsWith("DROP")) return "drop";
  return "strike";
}

// Plays /sounds/<name>.ogg; if the files are not there (rod web page), the old beeps
function playSoundFile(name, fallback)
{
  if(!soundFilesOk)
  {
    fallback();
    return;
  }

  try
  {
    let a = audioFiles[name];

    if(!a)
    {
      a = new Audio("/sounds/" + name + ".ogg");
      a.onerror = () => { soundFilesOk = false; };
      audioFiles[name] = a;
    }

    a.volume = 1.0;
    a.currentTime = 0;

    const p = a.play();

    if(p)
      p.catch(() => fallback());
  }
  catch(e)
  {
    fallback();
  }
}

function stopSoundFiles()
{
  for(const k in audioFiles)
  {
    try { audioFiles[k].pause(); } catch(e) {}
  }
}

function playAlarmPattern()
{
  if(!soundEnabled || activeAlarmType === "")
    return;

  const cat = alarmCategory(activeAlarmType);
  const type = activeAlarmType;

  playSoundFile(soundFor(cat), () =>
  {
    if(type === "SHARP STRIKE") sharpPattern();
    else if(type === "SLOW PULL") slowPattern();
    else if(type === "REPEATED BITE") repeatedPattern();
    else if(type.startsWith("DROP")) dropPattern();
    else sharpPattern();
  });

  vibrate(SOUND_VIBRATE[cat]);
}

// The phone app plays the same sounds when it is in the background
function tellAppSounds()
{
  if(window.StrikeApp && StrikeApp.setSound)
  {
    for(const c in SOUND_DEFAULT)
      StrikeApp.setSound(c, soundFor(c));
  }
}

window.addEventListener("load", () => tellAppSounds());

function showSoundChoices()
{
  for(const c in SOUND_DEFAULT)
  {
    if(el("snd_" + c))
      el("snd_" + c).value = soundFor(c);
  }
}

function chooseSound(cat)
{
  const v = el("snd_" + cat).value;

  try { localStorage.setItem("snd_" + cat, v); } catch(e) {}

  tellAppSounds();
  stopSoundFiles();
  playSoundFile(v, () => {});
}

function previewSound(cat)
{
  stopSoundFiles();
  playSoundFile(soundFor(cat), () => {});
}

function startContinuousAlarm(type)
{
  document.body.classList.add("strikeFlash");

  if(!soundEnabled)
    return;

  if(activeAlarmType === type && alarmTimer !== null)
    return;

  stopContinuousAlarm();

  document.body.classList.add("strikeFlash");

  activeAlarmType = type;

  playAlarmPattern();

  // again when the sound has finished (the old beeps were shorter)
  const cat = alarmCategory(type);
  let repeatTime = soundFilesOk
    ? (cat === "strike" ? 3000 : cat === "slow" ? 3600 : cat === "knock" ? 2800 : 2000)
    : (type === "SHARP STRIKE" ? 1200 : type === "SLOW PULL" ? 1800 : type === "REPEATED BITE" ? 1400 : 1900);

  alarmTimer = setInterval(playAlarmPattern, repeatTime);

  el("audioStatus").innerText = "ALARM";
}

function stopContinuousAlarm()
{
  const wasRinging = alarmTimer !== null;

  if(wasRinging)
  {
    clearInterval(alarmTimer);
    alarmTimer = null;
  }

  activeAlarmType = "";

  stopSoundFiles();

  document.body.classList.remove("strikeFlash");

  if(wasRinging)
    vibrate(0);

  if(soundEnabled)
    el("audioStatus").innerText = "READY";
}

async function testSharpSound()
{
  if(!soundEnabled)
    await enableSound();

  sharpPattern();
}

async function testSlowSound()
{
  if(!soundEnabled)
    await enableSound();

  slowPattern();
}

// =====================================================
// GRAPH
// =====================================================

const canvas = el("graph");
const ctx = canvas.getContext("2d");

const maxPoints = 300;     // about 30 s at 10 updates per second

let liveEnergy = [];
let liveThr = [];
let liveTilt = [];
let liveAllow = [];

let view = "live";
let replay = null;
let replayID = -1;

function resizeCanvas()
{
  const box = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;

  canvas.width = Math.floor(box.width * dpr);
  canvas.height = Math.floor(box.height * dpr);

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  redraw();
}

window.addEventListener("resize", resizeCanvas);

// =====================================================
// 3D NOISE VIEW
// =====================================================

const NOISE_POINTS = 300;        // 3 s at 100 per second

let noiseCloud = [];             // [x, y, z] in g
let noiseDown = [0, 0, -1];      // gravity direction, sensor frame
let noiseThr = 0.01;
let noiseSensor = "";
let noiseRange = 0.02;           // g at the edge of the view (auto)
let yaw = 0.7;
let pitch = 0.45;
let dragging = false;
let dragX = 0;
let dragY = 0;
let lastDrag = 0;

function addNoisePoints(d)
{
  if(!d.vib)
    return;

  for(let i = 0; i + 2 < d.vib.length; i += 3)
    noiseCloud.push([d.vib[i] / 10000, d.vib[i + 1] / 10000, d.vib[i + 2] / 10000]);

  if(noiseCloud.length > NOISE_POINTS)
    noiseCloud.splice(0, noiseCloud.length - NOISE_POINTS);

  if(d.g)
    noiseDown = [-d.g[0], -d.g[1], -d.g[2]];

  noiseThr = d.knockthr;
  noiseSensor = d.sname || "";
  cond3d = d;
  rodLiveData(d);
}

function noiseRms()
{
  if(noiseCloud.length === 0)
    return 0;

  let sum = 0;

  for(const p of noiseCloud)
    sum += p[0] * p[0] + p[1] * p[1] + p[2] * p[2];

  return Math.sqrt(sum / noiseCloud.length);
}

// Typical size of the shaking (median): one bump does not change it
function noiseTypical()
{
  if(noiseCloud.length === 0)
    return 0;

  const m = noiseCloud.map(p => Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2])).sort((a, b) => a - b);

  return m[Math.floor(m.length / 2)];
}

function noisePeak()
{
  let peak = 0;

  for(const p of noiseCloud)
    peak = Math.max(peak, Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));

  return peak;
}

function mg(g)
{
  const v = g * 1000;
  return (v < 10 ? v.toFixed(1) : v.toFixed(0)) + " mg";
}

function drawNoise3D()
{
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;

  ctx.clearRect(0, 0, w, h);

  const bg = ctx.createRadialGradient(w / 2, h / 2, 10, w / 2, h / 2, Math.max(w, h) * 0.7);
  bg.addColorStop(0, "#081631");
  bg.addColorStop(1, "#01040b");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);

  const rms = noiseTypical();
  const peak = noisePeak();

  // Zoom follows the noise and the knock limit, smoothly
  const target = Math.max(noiseThr * 1.3, rms * 4, 0.001);
  noiseRange += (target - noiseRange) * 0.06;

  if(!dragging && Date.now() - lastDrag > 4000)
    yaw += 0.004;

  const cx = w / 2;
  const cy = h / 2 + 10;
  const size = Math.min(w, h) * 0.38;
  const cyw = Math.cos(yaw), syw = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);

  // p in "view units" (1 = edge of the view)
  function proj(p)
  {
    const x = p[0] * cyw - p[1] * syw;
    const y = p[0] * syw + p[1] * cyw;
    const z = p[2];

    const y2 = y * cp - z * sp;
    const z2 = y * sp + z * cp;

    const k = 4 / (4 + y2);

    return [cx + x * size * k, cy - z2 * size * k, y2];
  }

  function lineP(a, b, colour, width, dash)
  {
    const pa = proj(a), pb = proj(b);

    ctx.beginPath();
    ctx.moveTo(pa[0], pa[1]);
    ctx.lineTo(pb[0], pb[1]);
    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  function ring(r, plane, colour, width, dash)
  {
    ctx.beginPath();

    for(let i = 0; i <= 48; i++)
    {
      const t = i / 48 * Math.PI * 2;
      const a = r * Math.cos(t), b = r * Math.sin(t);
      const p = plane === 0 ? [a, b, 0] : plane === 1 ? [a, 0, b] : [0, a, b];
      const q = proj(p);

      if(i === 0) ctx.moveTo(q[0], q[1]);
      else ctx.lineTo(q[0], q[1]);
    }

    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  // Floor rings (scale), axes
  ring(0.5, 0, "rgba(255,255,255,.06)", 1);
  ring(1.0, 0, "rgba(255,255,255,.09)", 1);

  const axes = [[[1, 0, 0], "#ff6b6b", "X"], [[0, 1, 0], "#22d3ff", "Y"], [[0, 0, 1], "#9b8cff", "Z"]];

  ctx.font = "bold 11px Arial";

  for(const a of axes)
  {
    lineP([0, 0, 0], a[0], a[1], 1.2);
    lineP([0, 0, 0], a[0].map(v => -v), "rgba(255,255,255,.08)", 1);

    const q = proj(a[0].map(v => v * 1.08));
    ctx.fillStyle = a[1];
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(a[2], q[0], q[1]);
  }

  // Which way is down
  const dn = noiseDown.map(v => v * 0.85);
  lineP([0, 0, 0], dn, "rgba(200,215,240,.35)", 2, [3, 4]);
  const qd = proj(dn);
  ctx.fillStyle = "rgba(200,215,240,.6)";
  ctx.fillText("down", qd[0], qd[1] + 10);

  // Knock limit sphere
  const rt = noiseThr / noiseRange;

  for(let pl = 0; pl < 3; pl++)
    ring(rt, pl, "rgba(255,186,82,.55)", 1.3, [5, 4]);

  // Points: older = fainter; outside the knock limit = orange
  const n = noiseCloud.length;

  for(let i = 0; i < n; i++)
  {
    const p = noiseCloud[i];
    const len = Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const q = proj(p.map(v => v / noiseRange));
    const age = i / Math.max(1, n - 1);
    const out = len > noiseThr;

    ctx.globalAlpha = 0.15 + 0.85 * age;
    ctx.fillStyle = out ? "#ffba52" : "#22d3ff";
    ctx.beginPath();
    ctx.arc(q[0], q[1], i === n - 1 ? 3.5 : 2.2, 0, Math.PI * 2);
    ctx.fill();
  }

  ctx.globalAlpha = 1;

  // Numbers
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.font = "bold 13px Arial";
  ctx.fillStyle = "#22d3ff";
  ctx.fillText("NOISE  " + mg(rms) + " typical", 12, 10);
  ctx.fillStyle = "#ffba52";
  ctx.fillText("KNOCK LIMIT  " + mg(noiseThr), 12, 28);
  ctx.fillStyle = "#7b90b2";
  ctx.font = "11px Arial";
  ctx.fillText("limit is " + (rms > 0 ? (noiseThr / rms).toFixed(1) : "-") + "x the noise · peak " + mg(peak) + (noiseSensor ? " · " + noiseSensor : ""), 12, 46);
  ctx.textBaseline = "bottom";
  ctx.fillText("axis length = " + mg(noiseRange) + " · zoom follows the noise", 12, h - 8);
}

// =====================================================
// 3D WAVES AND 3D WIND
// =====================================================

let cond3d = null;               // last /data
let seaAmp = 0.05;               // smoothed, view units
let seaChop = 0.01;
let seaPhase = 0;
let flagWind = 0;                // smoothed 0..1
let flagPhase = 0;
let lastSceneTime = 0;
let windStreaks = [];

function sceneSetup()
{
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;

  ctx.clearRect(0, 0, w, h);

  const bg = ctx.createRadialGradient(w / 2, h / 2, 10, w / 2, h / 2, Math.max(w, h) * 0.7);
  bg.addColorStop(0, "#081631");
  bg.addColorStop(1, "#01040b");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);

  if(!dragging && Date.now() - lastDrag > 4000)
    yaw += 0.002;

  const now = performance.now() / 1000;
  const dt = lastSceneTime ? Math.min(0.1, now - lastSceneTime) : 0.016;
  lastSceneTime = now;

  const cx = w / 2;
  const cy = h / 2 + 10;
  const size = Math.min(w, h) * 0.42;
  const cyw = Math.cos(yaw), syw = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);

  function proj(p)
  {
    const x = p[0] * cyw - p[1] * syw;
    const y = p[0] * syw + p[1] * cyw;
    const z = p[2];

    const y2 = y * cp - z * sp;
    const z2 = y * sp + z * cp;

    const k = 4 / (4 + y2);

    return [cx + x * size * k, cy - z2 * size * k, y2];
  }

  return { w, h, dt, proj };
}

// Painter's order: far quads first
function drawQuads(quads)
{
  quads.sort((a, b) => b.depth - a.depth);

  for(const q of quads)
  {
    ctx.beginPath();
    ctx.moveTo(q.p[0][0], q.p[0][1]);

    for(let i = 1; i < 4; i++)
      ctx.lineTo(q.p[i][0], q.p[i][1]);

    ctx.closePath();
    ctx.fillStyle = q.fill;
    ctx.fill();

    if(q.stroke)
    {
      ctx.strokeStyle = q.stroke;
      ctx.lineWidth = 0.6;
      ctx.stroke();
    }
  }
}

function sceneText(lines, footer, h)
{
  ctx.textAlign = "left";
  ctx.textBaseline = "top";

  let y = 10;

  for(const l of lines)
  {
    ctx.font = l[2] || "bold 13px Arial";
    ctx.fillStyle = l[1];
    ctx.fillText(l[0], 12, y);
    y += 18;
  }

  ctx.font = "11px Arial";
  ctx.fillStyle = "#7b90b2";
  ctx.textBaseline = "bottom";
  ctx.fillText(footer, 12, h - 8);
}

function mix(a, b, t)
{
  return Math.round(a + (b - a) * t);
}

// ---------- WAVES ----------

function drawWaves3D()
{
  const S = sceneSetup();
  const d = cond3d;

  const swing = d ? d.waveswing : 0;
  const period = d && d.period > 0 ? d.period : 6;
  const windLevel = d ? d.wind : 0;

  // Rod swing 0..4 deg -> wave height on the screen; wind adds chop
  const ampTarget = 0.03 + Math.min(swing, 4) / 4 * 0.32;
  const chopTarget = 0.008 + windLevel * 0.012;

  seaAmp += (ampTarget - seaAmp) * Math.min(1, S.dt * 1.5);
  seaChop += (chopTarget - seaChop) * Math.min(1, S.dt * 1.5);
  seaPhase += S.dt * Math.PI * 2 / period;

  const t = performance.now() / 1000;

  // Waves roll in along +x (toward the shore and the rod)
  function height(x, y)
  {
    return seaAmp * (
        0.62 * Math.sin(x * 3.2 - seaPhase)
      + 0.25 * Math.sin(x * 5.1 + y * 1.4 - seaPhase * 1.35 + 1.1)
      + 0.13 * Math.sin(x * 2.0 - y * 2.3 - seaPhase * 0.8 + 2.4))
      + seaChop * (
        Math.sin(x * 19 + y * 7 - t * 5.3)
      + Math.sin(x * 11 - y * 17 - t * 6.1 + 1.7)) * 0.5;
  }

  const N = 26;
  const pts = [];

  for(let i = 0; i <= N; i++)
  {
    pts.push([]);

    for(let j = 0; j <= N; j++)
    {
      const x = -1 + 2 * i / N;
      const y = -1 + 2 * j / N;
      const z = height(x, y);

      pts[i].push({ z, q: S.proj([x, y, z]) });
    }
  }

  const quads = [];
  const zMax = seaAmp + seaChop;

  for(let i = 0; i < N; i++)
  {
    for(let j = 0; j < N; j++)
    {
      const a = pts[i][j], b = pts[i + 1][j], c = pts[i + 1][j + 1], e = pts[i][j + 1];
      const z = (a.z + b.z + c.z + e.z) / 4;
      const slope = (b.z + c.z - a.z - e.z) / 2 * N;          // facing the light = brighter
      const tH = Math.max(0, Math.min(1, (z / zMax + 1) / 2));
      const lit = Math.max(0, Math.min(1, 0.55 + slope * 0.25));

      let r = mix(4, 20, tH), g = mix(30, 150, tH), bl = mix(80, 230, tH);

      r = mix(r * 0.55, r * 1.25, lit);
      g = mix(g * 0.55, g * 1.25, lit);
      bl = mix(bl * 0.55, Math.min(255, bl * 1.2), lit);

      // Foam on the top of big waves
      if(tH > 0.86 && seaAmp > 0.14)
      {
        const f = (tH - 0.86) / 0.14;
        r = mix(r, 220, f); g = mix(g, 240, f); bl = mix(bl, 255, f);
      }

      quads.push({
        p: [a.q, b.q, c.q, e.q],
        depth: (a.q[2] + b.q[2] + c.q[2] + e.q[2]) / 4,
        fill: "rgb(" + r + "," + g + "," + bl + ")",
        stroke: "rgba(34,211,255,.10)"
      });
    }
  }

  drawQuads(quads);

  // Float bobbing in the middle
  const fz = height(0, 0);
  const top = S.proj([0, 0, fz + 0.09]);
  const base = S.proj([0, 0, fz]);

  ctx.strokeStyle = "#eaf3ff";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(base[0], base[1]);
  ctx.lineTo(top[0], top[1]);
  ctx.stroke();

  ctx.fillStyle = "#ff8a3d";
  ctx.beginPath();
  ctx.arc(top[0], top[1], 5, 0, Math.PI * 2);
  ctx.fill();

  const lines = d ?
  [
    ["WAVES  " + WAVE_NAMES[d.wave], "#22d3ff"],
    ["rod swings ±" + d.waveswing.toFixed(1) + "°" + (d.period > 0 ? " · a wave every " + d.period.toFixed(0) + " s" : " · wave time not known yet"), "#c3d2ea", "12px Arial"],
    ["small ripples = wind: " + WIND_NAMES[d.wind], "#7b90b2", "11px Arial"]
  ] :
  [
    ["WAVES", "#22d3ff"],
    ["waiting for data…", "#7b90b2", "12px Arial"]
  ];

  sceneText(lines, "Height = rod swing · speed = wave time · ripples = wind · drag to turn", S.h);
}

// ---------- WIND (flag) ----------

function drawWind3D()
{
  const S = sceneSetup();
  const d = cond3d;

  // Same 0..1 strength as the WIND bar on the left
  let target = 0;

  if(d)
  {
    target = d.dir
      ? Math.max(d.windswing / 4.0, d.windvib / 0.03)
      : d.windvib / 0.03;

    target = Math.max(0, Math.min(1, target));
  }

  flagWind += (target - flagWind) * Math.min(1, S.dt * 1.2);

  const t = performance.now() / 1000;

  // Gusts: the strength wobbles a little
  const gust = flagWind * (1 + 0.25 * Math.sin(t * 0.7) * Math.sin(t * 1.9 + 1));
  const s = Math.max(0, Math.min(1.2, gust));

  flagPhase += S.dt * (2.5 + 11 * s);

  const ground = -0.85;
  const poleTop = 0.95;
  const poleX = -0.45;

  // Ground rings
  ctx.strokeStyle = "rgba(255,255,255,.08)";
  ctx.lineWidth = 1;

  for(const r of [0.5, 1.0])
  {
    ctx.beginPath();

    for(let i = 0; i <= 48; i++)
    {
      const a = i / 48 * Math.PI * 2;
      const q = S.proj([r * Math.cos(a), r * Math.sin(a), ground]);

      if(i === 0) ctx.moveTo(q[0], q[1]);
      else ctx.lineTo(q[0], q[1]);
    }

    ctx.stroke();
  }

  // Wind streaks (more and faster with more wind)
  const want = Math.round(4 + 36 * Math.min(1, s));

  while(windStreaks.length < want)
    windStreaks.push({ x: -1.3 + Math.random() * 2.6, y: -1 + Math.random() * 2, z: ground + 0.15 + Math.random() * 1.7, l: 0.1 + Math.random() * 0.25 });

  windStreaks.length = want;

  ctx.lineWidth = 1.2;

  for(const p of windStreaks)
  {
    p.x += S.dt * (0.15 + 2.2 * s);

    if(p.x > 1.3)
    {
      p.x = -1.3;
      p.y = -1 + Math.random() * 2;
      p.z = ground + 0.15 + Math.random() * 1.7;
    }

    const q0 = S.proj([p.x, p.y, p.z]);
    const q1 = S.proj([p.x - p.l * (0.4 + s), p.y, p.z]);

    ctx.strokeStyle = "rgba(160,200,255," + (0.08 + 0.25 * Math.min(1, s)) + ")";
    ctx.beginPath();
    ctx.moveTo(q0[0], q0[1]);
    ctx.lineTo(q1[0], q1[1]);
    ctx.stroke();
  }

  // Flag: hangs down when calm, flies out flat in a strong wind
  const L = 1.05, H = 0.6;
  const angle = (0.12 + 0.88 * Math.sqrt(Math.min(1, s))) * Math.PI / 2;
  const dx = Math.sin(angle), dz = -Math.cos(angle);
  const flap = 0.03 + 0.2 * Math.min(1, s);
  const NU = 18, NV = 8;
  const pts = [];

  for(let i = 0; i <= NU; i++)
  {
    pts.push([]);

    for(let j = 0; j <= NV; j++)
    {
      const u = i / NU, v = j / NV;

      // Sideways ripple runs from the pole to the free end
      const side = flap * Math.pow(u, 1.2) * Math.sin(u * 7 - flagPhase + v * 0.7)
                 + 0.03 * u * Math.sin(u * 15 - flagPhase * 1.7) * Math.min(1, s);

      const p = [poleX + u * L * dx, side, poleTop - v * H + u * L * dz];

      pts[i].push({ side, q: S.proj(p) });
    }
  }

  const quads = [];

  for(let i = 0; i < NU; i++)
  {
    for(let j = 0; j < NV; j++)
    {
      const a = pts[i][j], b = pts[i + 1][j], c = pts[i + 1][j + 1], e = pts[i][j + 1];
      const slope = (b.side + c.side - a.side - e.side) / 2 * NU;
      const lit = Math.max(0, Math.min(1, 0.6 + slope * 0.9));
      const u = (i + 0.5) / NU;

      // Brand colours along the flag: electric blue -> cyan
      const r = mix(mix(30, 34, u) * 0.45, mix(30, 34, u) * 1.3, lit);
      const g = mix(mix(123, 211, u) * 0.45, Math.min(255, mix(123, 211, u) * 1.2), lit);
      const bl = mix(255 * 0.45, 255, lit);

      quads.push({
        p: [a.q, b.q, c.q, e.q],
        depth: (a.q[2] + b.q[2] + c.q[2] + e.q[2]) / 4,
        fill: "rgb(" + r + "," + g + "," + bl + ")",
        stroke: "rgba(0,10,30,.25)"
      });
    }
  }

  // Pole, then the flag in front of it
  const pb = S.proj([poleX, 0, ground]), pt = S.proj([poleX, 0, poleTop + 0.04]);

  ctx.strokeStyle = "#8399bb";
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.moveTo(pb[0], pb[1]);
  ctx.lineTo(pt[0], pt[1]);
  ctx.stroke();

  drawQuads(quads);

  ctx.fillStyle = "#c3d2ea";
  ctx.beginPath();
  ctx.arc(pt[0], pt[1], 4, 0, Math.PI * 2);
  ctx.fill();

  const lines = d ?
  [
    ["WIND  " + WIND_NAMES[d.wind], "#22d3ff"],
    [(d.dir ? "rod sways ±" + d.windswing.toFixed(1) + "° · " : "") + "shake " + d.windvib.toFixed(3) + " g · strength " + Math.round(flagWind * 100) + "%", "#c3d2ea", "12px Arial"],
    ["strength only: the rod cannot tell the wind direction", "#7b90b2", "11px Arial"]
  ] :
  [
    ["WIND", "#22d3ff"],
    ["waiting for data…", "#7b90b2", "12px Arial"]
  ];

  sceneText(lines, "Calm = flag hangs · strong = flies and flaps · drag to turn", S.h);
}

// =====================================================
// LIVE ROD VIEW
// =====================================================

let rodD = null;                 // last /data
let rodTilt = 0;                 // smoothed tilt shown
let rodKnock = 0;                // 1 = knock now, fades out
let rodKnockID = -1;
let rodLastFrame = 0;
let rodAnim = null;              // { mode: "reel" or "cast", start: ms }
let rodPrevState = "";

const ROD_REEL_MS = 4500;
const ROD_CAST_MS = 3400;

function startRodAnim(mode)
{
  rodAnim = { mode: mode, start: performance.now() };
}

function rodLiveData(d)
{
  if(rodKnockID >= 0 && d.knockid !== rodKnockID)
    rodKnock = 1;

  rodKnockID = d.knockid;

  // Reeled in or cast from somewhere else (another phone, the watch)
  if(rodPrevState && rodPrevState !== d.state)
  {
    const fresh = rodAnim && performance.now() - rodAnim.start < 8000;

    if(d.state === "paused" && !(fresh && rodAnim.mode === "reel"))
      startRodAnim("reel");
    else if(rodPrevState === "paused" && !(fresh && rodAnim.mode === "cast"))
      startRodAnim("cast");
  }

  rodPrevState = d.state;

  if(d.energy > d.knockthr * 1.5)
    rodKnock = Math.max(rodKnock, Math.min(1, d.energy / d.knockthr / 4));

  rodD = d;
}

function drawRodLive()
{
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  const now = performance.now() / 1000;
  const dt = rodLastFrame ? Math.min(0.1, now - rodLastFrame) : 0.016;
  rodLastFrame = now;

  const d = rodD;
  let state = "armed";
  let label = el("mainStatus").innerText;

  if(d)
  {
    rodTilt += (d.tilt - rodTilt) * Math.min(1, dt * 8);

    if(d.state === "monitoring")
      label = "ROD ARMED";

    if(d.strike)
    {
      state = "strike";
      label = "STRIKE!  " + d.type;
    }
    else if(d.dropwatch)
    {
      state = "drop";
      label = "DROP-BACK – watching for a pull…";
    }
    else if(d.bucket >= 30)
    {
      state = "pull";
      label = "SLOW PULL " + Math.min(100, d.bucket) + "%";
    }
    else if(d.drop >= 30)
    {
      state = "drop";
      label = "DROP-BACK " + Math.min(100, d.drop) + "%";
    }
    else if(rodKnock > 0.3)
    {
      label = "KNOCK";
    }
  }

  let mode = "normal";
  let anim = 0;

  if(rodAnim)
  {
    const e = performance.now() - rodAnim.start;
    const dur = rodAnim.mode === "reel" ? ROD_REEL_MS : ROD_CAST_MS;

    if(e < dur)
    {
      mode = rodAnim.mode;
      anim = e / dur;
    }
    else
    {
      rodAnim = null;
    }
  }

  if(mode === "normal" && d && d.state === "paused")
    mode = "out";

  if(mode !== "normal")
  {
    state = "armed";
    label = mode === "reel" ? "REELING IN…" : mode === "cast" ? "CASTING…" : "REELED IN – cast, then press READY";
  }

  drawRodScene(ctx, w, h,
  {
    mode: mode,
    anim: anim,
    t: now,
    tilt: rodTilt,
    knock: mode === "normal" ? rodKnock : 0,
    state: state,
    label: label,
    dir: d ? d.dir : true,
    waveswing: d ? d.waveswing : 0.5,
    period: d && d.period > 0 ? d.period : 6,
    wind: d ? d.wind : 0,
    allow: d ? d.allow : 0,
    bucket: d ? d.bucket : 0,
    drop: d ? d.drop : 0
  });

  rodKnock = Math.max(0, rodKnock - dt * 2);
}

// Rounded box path (ctx.roundRect is missing on older phones)
function roundBox(c, x, y, w, h, r)
{
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}

// Surf rod in a sand spike, on the beach at night (v3, detailed).
// s: t, tilt (+ toward water), knock 0..1, state (armed/pull/drop/strike),
//    label, dir, waveswing, period, wind (0..3), allow, bucket, drop,
//    mode (normal / reel / out / cast) and anim 0..1 for reel in and cast,
//    hideLine / noHud (for the pull test picture). Returns the tip and line end.
function drawRodScene(ctx, w, h, s)
{
  const t = s.t;
  const k = Math.max(0.6, Math.min(h, w * 0.9) / 520);   // size scale
  const swing = s.waveswing;
  const period = s.period;
  const horizon = h * 0.47;
  const TAU = Math.PI * 2;

  const colour = s.state === "strike" ? "#ff4d5e" : s.state === "pull" || s.state === "drop" ? "#ffba52" : "#22d3ff";
  const rgb = s.state === "strike" ? "255,77,94" : s.state === "pull" || s.state === "drop" ? "255,186,82" : "34,211,255";
  const flash = s.state === "strike" ? 0.55 + 0.45 * Math.sin(t * 14) : 1;

  ctx.clearRect(0, 0, w, h);

  // ================= SKY =================
  let g = ctx.createLinearGradient(0, 0, 0, horizon);
  g.addColorStop(0, "#020611");
  g.addColorStop(0.55, "#0a1a3c");
  g.addColorStop(0.9, "#173f73");
  g.addColorStop(1, "#2a5f94");
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, w, horizon + 1);

  // stars
  for(let i = 0; i < 90; i++)
  {
    const sx = (i * 97.31 % 1) * w;
    const sy = Math.pow((i * 57.73) % 1, 1.4) * horizon * 0.85;
    const r = (i % 7 === 0 ? 1.4 : 0.8) * k;

    ctx.globalAlpha = 0.25 + 0.35 * (1 + Math.sin(t * (0.8 + i % 5 * 0.4) + i));
    ctx.fillStyle = i % 9 === 0 ? "#bfe3ff" : "#ffffff";
    ctx.beginPath();
    ctx.arc(sx, sy, r, 0, TAU);
    ctx.fill();
  }

  ctx.globalAlpha = 1;

  // moon
  const mr = 26 * k;
  const mx = w * 0.58;
  const my = h * 0.22;

  g = ctx.createRadialGradient(mx, my, mr * 0.5, mx, my, mr * 7);
  g.addColorStop(0, "rgba(170,210,255,.30)");
  g.addColorStop(1, "rgba(170,210,255,0)");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(mx, my, mr * 7, 0, TAU);
  ctx.fill();

  g = ctx.createRadialGradient(mx - mr * 0.35, my - mr * 0.35, mr * 0.1, mx, my, mr);
  g.addColorStop(0, "#ffffff");
  g.addColorStop(1, "#cfdcef");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(mx, my, mr, 0, TAU);
  ctx.fill();

  ctx.fillStyle = "rgba(120,140,170,.25)";

  for(const c of [[-0.3, -0.2, 0.22], [0.25, 0.1, 0.16], [-0.05, 0.4, 0.12], [0.35, -0.35, 0.1]])
  {
    ctx.beginPath();
    ctx.arc(mx + c[0] * mr, my + c[1] * mr, c[2] * mr, 0, TAU);
    ctx.fill();
  }

  // clouds drift with the wind
  const windSpeed = (4 + (s.wind || 0) * 14) * k;

  for(let i = 0; i < 5; i++)
  {
    const cw = (160 + i * 40) * k;
    const cx = ((i * 0.27 * w + t * windSpeed * (0.6 + i * 0.15)) % (w + cw * 2)) - cw;
    const cy = horizon * (0.18 + i * 0.13);

    ctx.fillStyle = "rgba(150,180,220," + (0.05 + (i % 2) * 0.03) + ")";

    for(let j = 0; j < 4; j++)
    {
      ctx.beginPath();
      ctx.ellipse(cx + j * cw * 0.22, cy + Math.sin(j * 2.1) * 6 * k, cw * 0.3, 12 * k * (1 + (j % 2) * 0.4), 0, 0, TAU);
      ctx.fill();
    }
  }

  // ================= SEA =================
  g = ctx.createLinearGradient(0, horizon, 0, h);
  g.addColorStop(0, "#22629f");
  g.addColorStop(0.35, "#0f4583");
  g.addColorStop(1, "#06234c");
  ctx.fillStyle = g;
  ctx.fillRect(0, horizon, w, h - horizon);

  // horizon haze
  g = ctx.createLinearGradient(0, horizon - 10 * k, 0, horizon + 14 * k);
  g.addColorStop(0, "rgba(120,170,230,0)");
  g.addColorStop(0.5, "rgba(120,170,230,.25)");
  g.addColorStop(1, "rgba(120,170,230,0)");
  ctx.fillStyle = g;
  ctx.fillRect(0, horizon - 10 * k, w, 24 * k);

  // moonlight path
  for(let i = 0; i < 26; i++)
  {
    const f = i / 26;
    const yy = horizon + 2 + Math.pow(f, 1.6) * (h - horizon) * 0.75;
    const ww = (4 + f * 70 * k) * (0.5 + 0.5 * Math.abs(Math.sin(t * 1.7 + i * 1.3)));

    ctx.fillStyle = "rgba(210,232,255," + (0.42 - f * 0.32) + ")";
    ctx.fillRect(mx - ww / 2 + Math.sin(t * 0.9 + i) * 6 * k * f, yy, ww, (1 + f * 2) * k);
  }

  // swells: darker troughs and light crests, rolling toward the shore
  for(let j = 0; j < 9; j++)
  {
    const f = ((j + t / period) % 9) / 9;            // 0 at the horizon, 1 at the shore
    const y = horizon + Math.pow(f, 2) * (h - horizon);
    const amp = (0.5 + swing * 1.3) * (0.3 + f * 2) * k;

    ctx.beginPath();

    for(let x = 0; x <= w; x += 8)
    {
      const yy = y + Math.sin(x * 0.012 / (0.3 + f) + j * 1.9 + t * 0.4) * amp;

      if(x === 0) ctx.moveTo(x, yy);
      else ctx.lineTo(x, yy);
    }

    ctx.strokeStyle = "rgba(160,225,255," + (0.08 + f * 0.30) + ")";
    ctx.lineWidth = (0.7 + f * 2.2) * k;
    ctx.stroke();

    ctx.lineTo(w, y + amp + 6 * f * k);
    ctx.lineTo(0, y + amp + 6 * f * k);
    ctx.closePath();
    ctx.fillStyle = "rgba(2,15,40," + (0.06 + f * 0.10) + ")";
    ctx.fill();
  }

  // ================= BEACH =================
  const shoreY = x => h * 0.68 + (x / w) * h * 0.24 - Math.sin(x * 0.012 + 1) * 6 * k;
  const surge = (Math.sin(t * TAU / period) * 0.5 + 0.5) * (6 + swing * 9) * k;   // swash up and down the beach

  // breaking wave just off the beach
  const breakPhase = (t / period) % 1;
  const breakY = x => shoreY(x) - (26 - breakPhase * 18) * k;

  ctx.beginPath();

  for(let x = 0; x <= w; x += 6)
  {
    const yy = breakY(x) + Math.sin(x * 0.05 + t * 2) * 2 * k;

    if(x === 0) ctx.moveTo(x, yy);
    else ctx.lineTo(x, yy);
  }

  ctx.strokeStyle = "rgba(235,245,255," + (0.25 + 0.5 * Math.sin(breakPhase * Math.PI) * Math.min(1, 0.3 + swing / 2)) + ")";
  ctx.lineWidth = (2 + swing * 1.5) * k;
  ctx.stroke();

  // wet sand (shines)
  ctx.beginPath();
  ctx.moveTo(0, h);
  for(let x = 0; x <= w; x += 6) ctx.lineTo(x, shoreY(x) - surge - 4 * k);
  ctx.lineTo(w, h);
  ctx.closePath();
  g = ctx.createLinearGradient(0, h * 0.6, 0, h);
  g.addColorStop(0, "#5d5040");
  g.addColorStop(1, "#3d3226");
  ctx.fillStyle = g;
  ctx.fill();

  // moon shine on the wet sand
  ctx.fillStyle = "rgba(200,225,255,.10)";
  ctx.beginPath();
  ctx.ellipse(mx * 0.9, shoreY(mx * 0.9) + 6 * k, 60 * k, 5 * k, 0.2, 0, TAU);
  ctx.fill();

  // swash foam: lacy edge
  for(let layer = 0; layer < 3; layer++)
  {
    ctx.beginPath();

    for(let x = 0; x <= w; x += 5)
    {
      const yy = shoreY(x) - surge - 4 * k - layer * 4 * k + Math.sin(x * (0.09 + layer * 0.05) + t * (2 + layer)) * 1.6 * k;

      if(x === 0) ctx.moveTo(x, yy);
      else ctx.lineTo(x, yy);
    }

    ctx.strokeStyle = "rgba(245,250,255," + (0.85 - layer * 0.28) + ")";
    ctx.lineWidth = (2.4 - layer * 0.6) * k;
    ctx.stroke();
  }

  // dry sand / dune
  const sandY = x => shoreY(x) + 10 * k - Math.max(0, 1 - x / (w * 0.5)) * 30 * k;

  ctx.beginPath();
  ctx.moveTo(0, h);
  for(let x = 0; x <= w; x += 6) ctx.lineTo(x, sandY(x));
  ctx.lineTo(w, h);
  ctx.closePath();
  g = ctx.createLinearGradient(0, h * 0.55, 0, h);
  g.addColorStop(0, "#d8b98a");
  g.addColorStop(0.5, "#b8946a");
  g.addColorStop(1, "#7d6040");
  ctx.fillStyle = g;
  ctx.fill();

  // moonlit rim on the dune
  ctx.beginPath();
  for(let x = 0; x <= w; x += 6) x ? ctx.lineTo(x, sandY(x)) : ctx.moveTo(x, sandY(x));
  ctx.strokeStyle = "rgba(255,240,215,.35)";
  ctx.lineWidth = 1.5 * k;
  ctx.stroke();

  // sand grain and ripples
  for(let i = 0; i < 220; i++)
  {
    const gx = (i * 61.73 % 1) * w;
    const gy = sandY(gx) + 4 * k + ((i * 37.31) % 1) * (h - sandY(gx));

    ctx.fillStyle = i % 3 ? "rgba(70,45,20,.22)" : "rgba(255,240,210,.18)";
    ctx.fillRect(gx, gy, 1.3 * k, 1.3 * k);
  }

  ctx.strokeStyle = "rgba(90,60,30,.18)";
  ctx.lineWidth = 1 * k;

  for(let i = 0; i < 9; i++)
  {
    const rx = (i * 0.11 + 0.02) * w, ry = sandY(rx) + (20 + (i % 3) * 18) * k;

    ctx.beginPath();
    ctx.ellipse(rx, ry, 26 * k, 3 * k, 0.15, Math.PI * 1.1, Math.PI * 1.9);
    ctx.stroke();
  }

  // ================= SAND SPIKE =================
  const lean = 12 * Math.PI / 180;
  const ux = Math.sin(lean), uy = -Math.cos(lean);      // up the spike
  const nx = Math.cos(lean), ny = Math.sin(lean);       // toward the sea
  const baseX = Math.max(w * 0.17, 70 * k);
  const baseY = sandY(baseX) + 34 * k;
  const spikeLen = Math.min(h * 0.33, 190 * k * 1.1);
  const spikeTop = [baseX + ux * spikeLen, baseY + uy * spikeLen];

  // shadow (moon is up-right, so it falls down-left)
  ctx.fillStyle = "rgba(40,25,10,.30)";
  ctx.beginPath();
  ctx.moveTo(baseX, baseY);
  ctx.lineTo(baseX - spikeLen * 0.55, baseY + 10 * k);
  ctx.lineTo(baseX - spikeLen * 0.55, baseY + 14 * k);
  ctx.lineTo(baseX + 4 * k, baseY + 4 * k);
  ctx.closePath();
  ctx.fill();

  // sand mound where it goes in
  ctx.fillStyle = "#c2a074";
  ctx.beginPath();
  ctx.ellipse(baseX, baseY, 16 * k, 5 * k, 0, Math.PI, TAU);
  ctx.fill();

  function tube(x0, y0, x1, y1, width, dark, light)
  {
    const gg = ctx.createLinearGradient(x0 - nx * width, y0 - ny * width, x0 + nx * width, y0 + ny * width);
    gg.addColorStop(0, dark);
    gg.addColorStop(0.45, light);
    gg.addColorStop(1, dark);
    ctx.strokeStyle = gg;
    ctx.lineWidth = width;
    ctx.lineCap = "round";
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.stroke();
  }

  tube(baseX, baseY - 3 * k, spikeTop[0], spikeTop[1], 7 * k, "#0b3c8c", "#4aa3ff");

  // ================= ROD =================
  const buttCup = [baseX + ux * spikeLen * 0.2, baseY + uy * spikeLen * 0.2];
  const butt = [buttCup[0] + nx * 9 * k, buttCup[1] + ny * 9 * k];
  const L = Math.min(h * 0.6, spikeLen * 2.2);
  const a0 = -(Math.PI / 2 - lean);
  const B0 = 0.30;

  let B = B0 + 0.6 * Math.tanh(s.tilt / 4);

  if(s.knock > 0)
    B += s.knock * 0.05 * Math.sin(t * TAU * 7);

  B = Math.max(0, Math.min(1.5, B));

  const mode = s.mode || "normal";
  const p = s.anim || 0;
  const ease = f => (1 - Math.cos(Math.max(0, Math.min(1, f)) * Math.PI)) / 2;
  let swingRot = 0;                                   // whole rod turns about the butt (cast)

  if(mode === "reel")
  {
    B = B0 + 0.22 + 0.05 * Math.sin(t * 9);           // loaded by the lead coming in
  }
  else if(mode === "out")
  {
    B = 0.06;                                         // only the lead hanging
  }
  else if(mode === "cast")
  {
    if(p < 0.25)                                      // back over the shoulder, tip loads backward
    {
      swingRot = -0.55 * ease(p / 0.25);
      B = 0.06 - 0.35 * ease(p / 0.25);
    }
    else if(p < 0.38)                                 // whip forward
    {
      const f = (p - 0.25) / 0.13;
      swingRot = -0.55 + 0.7 * f;
      B = -0.3 + 1.2 * f;
    }
    else if(p < 0.6)                                  // follow through, the rod shivers
    {
      const f = (p - 0.38) / 0.22;
      swingRot = 0.15 * (1 - f);
      B = 0.1 + 0.8 * (1 - f) + 0.15 * Math.sin(f * 20) * (1 - f);
    }
    else                                              // line tightens, back to normal
    {
      B = 0.1 + (B0 - 0.1) * ease((p - 0.6) / 0.4);
    }
  }

  const cupU = (spikeLen * 0.8) / L;
  const N = 64;

  function rodPoints(bend, rot)
  {
    const pts = [];
    let x = butt[0], y = butt[1];

    for(let i = 0; i <= N; i++)
    {
      const u = i / N;
      const v = Math.max(0, (u - cupU) / (1 - cupU));
      const ang = a0 + rot + bend * v * (2 - v);

      pts.push([x, y, ang]);
      x += Math.cos(ang) * L / N;
      y += Math.sin(ang) * L / N;
    }

    return pts;
  }

  const pts = rodPoints(B, swingRot);
  const tip = pts[N];
  const at = u => pts[Math.min(N, Math.round(u * N))];
  const side = (p, d) => [p[0] + Math.sin(p[2]) * -d, p[1] + Math.cos(p[2]) * d];   // + d = underside (toward the sea)

  // normal position (dashed)
  const rest = rodPoints(B0, 0);
  if(mode === "normal")
  {
  ctx.setLineDash([5 * k, 6 * k]);
  ctx.strokeStyle = "rgba(234,243,255,.22)";
  ctx.lineWidth = 1.2 * k;
  ctx.beginPath();
  rest.forEach((q, i) => i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]));
  ctx.stroke();
  ctx.setLineDash([]);
  }

  const width = u => (9 - 7.6 * Math.pow(u, 0.8)) * k;

  // blank as a filled taper
  function blank(u0, u1, fill)
  {
    const a = Math.round(u0 * N), b = Math.round(u1 * N);

    ctx.beginPath();

    for(let i = a; i <= b; i++)
    {
      const q = side(pts[i], -width(i / N) / 2);
      if(i === a) ctx.moveTo(q[0], q[1]);
      else ctx.lineTo(q[0], q[1]);
    }

    for(let i = b; i >= a; i--)
    {
      const q = side(pts[i], width(i / N) / 2);
      ctx.lineTo(q[0], q[1]);
    }

    ctx.closePath();
    ctx.fillStyle = fill;
    ctx.fill();
  }

  // ---- line from the reel, through the guides, to the tip ----
  const reelU = 0.21;
  const rp = at(reelU);
  const spool = side(rp, 22 * k);
  const guides = [0.34, 0.47, 0.58, 0.68, 0.76, 0.83, 0.89, 0.945];
  const ringAt = u => side(at(u), (13 - 10 * u) * k);

  ctx.strokeStyle = "rgba(234,243,255,.55)";
  ctx.lineWidth = 0.8 * k;
  ctx.beginPath();
  ctx.moveTo(spool[0], spool[1]);
  for(const u of guides) { const q = ringAt(u); ctx.lineTo(q[0], q[1]); }
  ctx.lineTo(tip[0], tip[1]);
  ctx.stroke();

  // ---- handle (EVA), reel seat, blank ----
  blank(0, 0.17, "#20262f");
  blank(0.17, 0.25, "#55657c");
  blank(0.25, 1, "#1b2436");

  // gloss along the blank
  ctx.strokeStyle = "rgba(200,225,255,.55)";
  ctx.lineWidth = 1.4 * k;
  ctx.beginPath();
  for(let i = Math.round(0.25 * N); i <= N - 2; i++) { const q = side(pts[i], -width(i / N) * 0.22); i === Math.round(0.25 * N) ? ctx.moveTo(q[0], q[1]) : ctx.lineTo(q[0], q[1]); }
  ctx.stroke();

  // EVA grip texture
  ctx.strokeStyle = "rgba(255,255,255,.07)";
  ctx.lineWidth = 1 * k;

  for(let i = 1; i < Math.round(0.17 * N); i += 2)
  {
    const a = side(pts[i], -width(i / N) / 2), b = side(pts[i], width(i / N) / 2);
    ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
  }

  // tip section in the state colour (painted tip, like a real surf rod)
  ctx.globalAlpha = flash;
  blank(0.86, 1, colour);
  ctx.globalAlpha = 1;

  // whippings next to the guides (brand cyan)
  for(const u of guides)
  {
    const p = at(u - 0.008), q = at(u + 0.008);
    const wd = width(u) * 0.9;

    ctx.strokeStyle = "#1fb6ff";
    ctx.lineWidth = wd;
    ctx.lineCap = "butt";
    ctx.beginPath(); ctx.moveTo(p[0], p[1]); ctx.lineTo(q[0], q[1]); ctx.stroke();
  }

  // guides: foot + ring on the underside
  for(const u of guides)
  {
    const p = at(u), q = ringAt(u);
    const r = (6.5 - 5 * u) * k;

    ctx.strokeStyle = "#9fb3cf";
    ctx.lineWidth = 1.2 * k;
    ctx.beginPath(); ctx.moveTo(p[0], p[1]); ctx.lineTo(q[0], q[1]); ctx.stroke();
    ctx.lineWidth = 1.6 * k;
    ctx.beginPath(); ctx.arc(q[0], q[1], r, 0, TAU); ctx.stroke();
  }

  // ---- spinning reel ----
  ctx.save();
  ctx.translate(rp[0], rp[1]);
  ctx.rotate(rp[2] + Math.PI / 2);                    // x across the rod (toward the sea), y along it

  ctx.fillStyle = "#7c8aa3";                          // stem
  ctx.fillRect(2 * k, -2 * k, 10 * k, 4 * k);

  g = ctx.createLinearGradient(10 * k, 0, 30 * k, 0);  // body
  g.addColorStop(0, "#2b3446");
  g.addColorStop(0.5, "#56657f");
  g.addColorStop(1, "#1d2433");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.ellipse(21 * k, 4 * k, 10 * k, 13 * k, 0, 0, TAU);
  ctx.fill();

  ctx.fillStyle = "#c7d3e6";                          // spool with line
  ctx.beginPath();
  ctx.ellipse(21 * k, -12 * k, 9 * k, 5 * k, 0, 0, TAU);
  ctx.fill();
  ctx.fillStyle = "#1fb6ff";
  ctx.beginPath();
  ctx.ellipse(21 * k, -12 * k, 6.5 * k, 3.4 * k, 0, 0, TAU);
  ctx.fill();

  ctx.strokeStyle = "#a9b8cf";                         // handle arm + knob
  ctx.lineWidth = 2 * k;
  const crankA = mode === "reel" ? t * 14 : 0.67;      // the handle turns while reeling
  const knob = [30 * k + Math.cos(crankA) * 13 * k, 6 * k + Math.sin(crankA) * 13 * k];
  ctx.beginPath(); ctx.moveTo(30 * k, 6 * k); ctx.lineTo(knob[0], knob[1]); ctx.stroke();
  ctx.fillStyle = "#11151d";
  ctx.beginPath(); ctx.ellipse(knob[0] + 1 * k, knob[1] + 2 * k, 3 * k, 4.5 * k, 0, 0, TAU); ctx.fill();
  ctx.restore();

  // ---- cups on the spike ----
  function cup(p, wd, ht)
  {
    ctx.save();
    ctx.translate(p[0], p[1]);
    ctx.rotate(lean);
    ctx.fillStyle = "#0e1117";
    ctx.beginPath();
    ctx.moveTo(-wd / 2, -ht / 2);
    ctx.lineTo(wd / 2 + 8 * k, -ht / 2);
    ctx.lineTo(wd / 2 + 8 * k, ht / 2);
    ctx.lineTo(-wd / 2, ht / 2);
    ctx.closePath();
    ctx.fill();
    ctx.strokeStyle = "rgba(255,255,255,.15)";
    ctx.lineWidth = 1 * k;
    ctx.stroke();
    ctx.restore();
  }

  cup([spikeTop[0] + nx * 2 * k, spikeTop[1] + ny * 2 * k], 12 * k, 10 * k);
  cup([buttCup[0] + nx * 2 * k, buttCup[1] + ny * 2 * k], 12 * k, 16 * k);

  ctx.fillStyle = "#1b1f27";                          // thumb screw
  ctx.beginPath();
  ctx.arc(buttCup[0] - nx * 9 * k, buttCup[1] - ny * 9 * k, 3.5 * k, 0, TAU);
  ctx.fill();

  // ---- StrikeSense box on the spike, cable up to the sensor ----
  const box = [baseX + ux * spikeLen * 0.07 - nx * 4 * k, baseY + uy * spikeLen * 0.07 - ny * 4 * k];
  const sp = at(0.88);

  ctx.strokeStyle = "rgba(34,211,255,.55)";
  ctx.lineWidth = 1 * k;
  ctx.beginPath();
  ctx.moveTo(box[0], box[1] - 7 * k);
  for(let i = Math.round(0.05 * N); i <= Math.round(0.88 * N); i++) { const q = side(pts[i], -width(i / N) / 2 - 1.2 * k); ctx.lineTo(q[0], q[1]); }
  ctx.stroke();

  ctx.save();
  ctx.translate(box[0], box[1]);
  ctx.rotate(lean);
  ctx.fillStyle = "#0b1630";
  ctx.strokeStyle = "#22d3ff";
  ctx.lineWidth = 1.3 * k;
  roundBox(ctx, -10 * k, -8 * k, 20 * k, 16 * k, 3 * k);
  ctx.fill();
  ctx.stroke();
  ctx.fillStyle = "rgba(" + rgb + "," + flash + ")";
  ctx.beginPath();
  ctx.arc(5 * k, -3 * k, 1.8 * k, 0, TAU);                 // status LED
  ctx.fill();
  ctx.restore();

  ctx.fillStyle = "#22d3ff";                               // sensor
  ctx.save();
  ctx.translate(sp[0], sp[1]);
  ctx.rotate(sp[2]);
  ctx.fillRect(-4 * k, -4 * k, 8 * k, 8 * k);
  ctx.restore();

  // ================= LINE TO THE SEA =================
  const endX = w * 0.93, endY = horizon + (h - horizon) * 0.14;

  // lead + hook
  function lead(x, y)
  {
    const z = 1.7 * k;                              // drawn a bit big so it can be seen
    ctx.strokeStyle = "rgba(234,243,255,.6)";
    ctx.lineWidth = 0.8 * z;
    ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + 5 * z, y + 9 * z); ctx.stroke();      // snood
    ctx.strokeStyle = "#c3d2ea";
    ctx.lineWidth = 1.2 * z;
    ctx.beginPath(); ctx.arc(x + 5 * z, y + 12 * z, 2.6 * z, -Math.PI / 2, Math.PI * 0.9); ctx.stroke();  // hook
    ctx.fillStyle = "#8a96ab";
    ctx.beginPath(); ctx.ellipse(x, y + 4 * z, 3.2 * z, 5.5 * z, 0, 0, TAU); ctx.fill();   // lead
    ctx.fillStyle = "rgba(255,255,255,.35)";
    ctx.beginPath(); ctx.ellipse(x - 1 * z, y + 2.5 * z, 1 * z, 2 * z, 0, 0, TAU); ctx.fill();
  }

  function straightLine(x, y, alpha)
  {
    ctx.strokeStyle = "rgba(234,243,255," + alpha + ")";
    ctx.lineWidth = 1 * k;
    ctx.beginPath(); ctx.moveTo(tip[0], tip[1]); ctx.lineTo(x, y); ctx.stroke();
  }

  const hang = [tip[0] + Math.sin(t * 1.8) * 6 * k, tip[1] + 34 * k];   // lead hanging from the tip
  const inX = w * 0.44, inY = shoreY(w * 0.44) - surge - 10 * k;      // where the rig comes out of the surf

  if(mode === "reel")
  {
    if(p < 0.75)                                      // rig comes in through the waves
    {
      const f = ease(p / 0.75);
      const x = endX + (inX - endX) * f, y = endY + (inY - endY) * f;

      straightLine(x, y, 0.8);

      ctx.strokeStyle = "rgba(220,240,255,.45)";      // V wake behind it
      ctx.lineWidth = 1 * k;
      ctx.beginPath();
      ctx.moveTo(x + 22 * k, y - 4 * k); ctx.lineTo(x, y); ctx.lineTo(x + 22 * k, y + 5 * k);
      ctx.stroke();
    }
    else                                              // out of the water, up to the tip
    {
      const f = ease((p - 0.75) / 0.25);
      const x = inX + (hang[0] - inX) * f, y = inY + (hang[1] - inY) * f;

      straightLine(x, y, 0.8);
      lead(x, y);

      ctx.fillStyle = "rgba(200,230,255,.7)";         // drips
      for(let i = 0; i < 3; i++) ctx.fillRect(x - 1 * k, y + (14 + i * 6 + ((t * 40) % 6)) * k, 1.2 * k, 2.5 * k);
    }
  }
  else if(mode === "out")
  {
    straightLine(hang[0], hang[1], 0.7);
    lead(hang[0], hang[1]);
  }
  else if(mode === "cast" && p < 0.83)
  {
    const R = rodPoints(B0, 0)[N];                    // release point

    if(p < 0.33)                                      // lead swings with the rod
    {
      const x = tip[0] - Math.sin(swingRot) * 18 * k, y = tip[1] + 24 * k;
      straightLine(x, y, 0.7);
      lead(x, y);
    }
    else                                              // flying out over the sea
    {
      const f = Math.min(1, (p - 0.33) / 0.5);
      const H = Math.min(h * 0.32, Math.max(10 * k, (R[1] + endY) / 2 - h * 0.14));   // arc stays in view
      const fx = R[0] + (endX - R[0]) * f;
      const fy = R[1] + (endY - R[1]) * f - H * 4 * f * (1 - f);

      ctx.strokeStyle = "rgba(234,243,255,.6)";
      ctx.lineWidth = 1 * k;
      ctx.beginPath();
      ctx.moveTo(tip[0], tip[1]);
      ctx.quadraticCurveTo((tip[0] + fx) / 2, Math.min(tip[1], fy) - 10 * k, fx, fy);
      ctx.stroke();

      for(let i = 1; i <= 6; i++)                     // trail
      {
        const g2 = Math.max(0, f - i * 0.025);
        ctx.fillStyle = "rgba(200,230,255," + (0.35 - i * 0.05) + ")";
        ctx.beginPath();
        ctx.arc(R[0] + (endX - R[0]) * g2, R[1] + (endY - R[1]) * g2 - H * 4 * g2 * (1 - g2), 1.4 * k, 0, TAU);
        ctx.fill();
      }

      lead(fx, fy);
    }
  }
  else if(!s.hideLine)
  {
    const tension = Math.max(0, Math.min(1, (B - B0) / 0.25 + 0.55));
    const midX = (tip[0] + endX) / 2, midY = (tip[1] + endY) / 2;
    const cpX = midX + (tip[0] + (endX - tip[0]) * 0.2 - midX) * (1 - tension);
    const cpY = midY + (endY + h * 0.12 - midY) * (1 - tension);

    ctx.strokeStyle = s.state === "strike" ? "rgba(255,77,94," + flash + ")" : "rgba(234,243,255," + (0.35 + tension * 0.35) + ")";
    ctx.lineWidth = (s.state === "strike" ? 1.6 : 1) * k;
    ctx.beginPath();
    ctx.moveTo(tip[0], tip[1]);
    ctx.quadraticCurveTo(cpX, cpY, endX, endY);
    ctx.stroke();
  }

  // where the line goes in: rings; a splash on a strike or when the lead lands
  const lineIn = mode === "normal" || (mode === "cast" && p >= 0.83);

  for(let i = 0; i < 3 && lineIn; i++)
  {
    const f = ((t * 0.5 + i / 3) % 1);

    ctx.strokeStyle = "rgba(200,235,255," + (0.5 * (1 - f)) + ")";
    ctx.lineWidth = 1 * k;
    ctx.beginPath();
    ctx.ellipse(endX, endY, (4 + f * 18) * k, (1.2 + f * 4) * k, 0, 0, TAU);
    ctx.stroke();
  }

  const landing = mode === "cast" && p >= 0.83;

  if(lineIn && (s.state === "strike" || s.knock > 0.4 || landing))
  {
    const big = s.state === "strike" || landing ? 1 : 0.4;

    for(let i = 0; i < 14; i++)
    {
      const ph = (t * 1.6 + i * 0.13) % 1;
      const a = -Math.PI / 2 + (i / 13 - 0.5) * 1.8;
      const dist = ph * 26 * k * big;

      ctx.fillStyle = "rgba(230,245,255," + (1 - ph) + ")";
      ctx.beginPath();
      ctx.arc(endX + Math.cos(a) * dist, endY + Math.sin(a) * dist + ph * ph * 18 * k * big, 1.6 * k, 0, TAU);
      ctx.fill();
    }
  }

  // ================= TIP LIGHT =================
  const glowR = (s.state === "strike" ? 34 : 22) * k;

  g = ctx.createRadialGradient(tip[0], tip[1], 0, tip[0], tip[1], glowR);
  g.addColorStop(0, "rgba(" + rgb + "," + (0.9 * flash) + ")");
  g.addColorStop(0.3, "rgba(" + rgb + "," + (0.35 * flash) + ")");
  g.addColorStop(1, "rgba(" + rgb + ",0)");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(tip[0], tip[1], glowR, 0, TAU);
  ctx.fill();

  ctx.fillStyle = "#ffffff";
  ctx.beginPath();
  ctx.arc(tip[0], tip[1], 2.2 * k, 0, TAU);
  ctx.fill();

  if(s.knock > 0.15)
  {
    ctx.strokeStyle = "rgba(34,211,255," + (0.8 * s.knock) + ")";
    ctx.lineWidth = 1.5 * k;

    for(const r of [12, 19, 26])
    {
      ctx.beginPath();
      ctx.arc(tip[0], tip[1], r * k * (1.2 - s.knock * 0.2), -2.7, -0.4);
      ctx.stroke();
    }
  }

  // ================= VIGNETTE (+ red pulse on a strike) =================
  g = ctx.createRadialGradient(w / 2, h / 2, Math.min(w, h) * 0.35, w / 2, h / 2, Math.max(w, h) * 0.75);
  g.addColorStop(0, "rgba(0,0,0,0)");
  g.addColorStop(1, s.state === "strike" ? "rgba(255,40,60," + (0.35 * flash) + ")" : "rgba(0,0,0,.45)");
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, w, h);

  if(s.noHud)
    return { tip: tip, end: [endX, endY] };

  // ================= HUD =================
  const kh = Math.max(0.8, k);               // readable on a phone too
  const pw = Math.min(300 * kh, w - 24), ph = 96 * kh;
  const px = w - pw - 12, py = w < 600 ? h - ph - 12 : 12;     // phone: over the sand

  ctx.fillStyle = "rgba(4,10,24,.62)";
  ctx.strokeStyle = "rgba(" + rgb + ",.45)";
  ctx.lineWidth = 1;
  roundBox(ctx, px, py, pw, ph, 10 * kh);
  ctx.fill();
  ctx.stroke();

  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.font = "bold " + Math.round(15 * kh) + "px Arial";
  ctx.fillStyle = colour;
  ctx.fillText(s.label, px + 12 * kh, py + 10 * kh);

  ctx.font = Math.round(11 * kh) + "px Arial";
  ctx.fillStyle = "#8ea6c8";
  ctx.fillText("tip " + (s.tilt >= 0 ? "+" : "") + s.tilt.toFixed(1) + "°" + (s.dir ? "  (+ toward water)" : "  (direction not learned)"), px + 12 * kh, py + 30 * kh);

  // bend gauge: -4° .. +6°, 0 = normal, orange tick = pull allow
  const gx0 = px + 12 * kh, gx1 = px + pw - 12 * kh, gy = py + 56 * kh;
  const gpos = v => gx0 + (Math.max(-4, Math.min(6, v)) + 4) / 10 * (gx1 - gx0);

  ctx.fillStyle = "rgba(255,255,255,.08)";
  roundBox(ctx, gx0, gy - 3 * kh, gx1 - gx0, 6 * kh, 3 * kh);
  ctx.fill();

  g = ctx.createLinearGradient(gpos(0), 0, gpos(s.tilt), 0);
  g.addColorStop(0, "rgba(" + rgb + ",.25)");
  g.addColorStop(1, "rgba(" + rgb + ",1)");
  ctx.fillStyle = g;
  ctx.fillRect(Math.min(gpos(0), gpos(s.tilt)), gy - 3 * kh, Math.abs(gpos(s.tilt) - gpos(0)), 6 * kh);

  ctx.fillStyle = "#eaf3ff";
  ctx.fillRect(gpos(0) - 0.75, gy - 6 * kh, 1.5, 12 * kh);

  if(s.allow > 0)
  {
    ctx.fillStyle = "#ffba52";
    ctx.fillRect(gpos(s.allow) - 1, gy - 6 * kh, 2, 12 * kh);
  }

  ctx.fillStyle = "#ffffff";
  ctx.beginPath();
  ctx.arc(gpos(s.tilt), gy, 4.5 * kh, 0, TAU);
  ctx.fill();

  ctx.font = Math.round(9 * kh) + "px Arial";
  ctx.fillStyle = "#5f7799";
  ctx.textAlign = "left";
  ctx.fillText("back", gx0, gy + 9 * kh);
  ctx.textAlign = "center";
  ctx.fillText("normal", gpos(0), gy + 9 * kh);
  ctx.textAlign = "right";
  ctx.fillText("toward water", gx1, gy + 9 * kh);

  // slow pull / drop-back mini bars
  const by = py + ph - 12 * kh;
  const bw = (gx1 - gx0 - 10 * kh) / 2;

  for(const [i, name, val] of [[0, "PULL", s.bucket || 0], [1, "DROP", s.drop || 0]])
  {
    const bx = gx0 + i * (bw + 10 * kh);

    ctx.fillStyle = "rgba(255,255,255,.08)";
    ctx.fillRect(bx + 32 * kh, by - 2 * kh, bw - 32 * kh, 4 * kh);
    ctx.fillStyle = val >= 70 ? "#ff4d5e" : val >= 30 ? "#ffba52" : "#22d3ff";
    ctx.fillRect(bx + 32 * kh, by - 2 * kh, (bw - 32 * kh) * Math.min(100, val) / 100, 4 * kh);
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    ctx.fillStyle = "#8ea6c8";
    ctx.fillText(name, bx, by);
  }

  return { tip: tip, end: [endX, endY] };
}

function draw3DView()
{
  if(view === "3dwave")
    drawWaves3D();
  else if(view === "3dwind")
    drawWind3D();
  else
    drawNoise3D();
}

function noiseLoop()
{
  // The canvas box can change size without a window resize (buttons
  // wrapping to two lines): keep the drawing size the same as the box
  if(view.startsWith("3d") || view === "rod")
  {
    const box = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;

    if(canvas.width !== Math.floor(box.width * dpr) || canvas.height !== Math.floor(box.height * dpr))
      resizeCanvas();
  }

  if(view.startsWith("3d"))
    draw3DView();
  else if(view === "rod")
    drawRodLive();

  requestAnimationFrame(noiseLoop);
}

canvas.addEventListener("pointerdown", e =>
{
  if(!view.startsWith("3d"))
    return;

  dragging = true;
  dragX = e.clientX;
  dragY = e.clientY;
  canvas.setPointerCapture(e.pointerId);
});

canvas.addEventListener("pointermove", e =>
{
  if(!dragging)
    return;

  yaw += (e.clientX - dragX) * 0.01;
  pitch = Math.max(-1.4, Math.min(1.4, pitch + (e.clientY - dragY) * 0.01));
  dragX = e.clientX;
  dragY = e.clientY;
  lastDrag = Date.now();
});

canvas.addEventListener("pointerup", () =>
{
  dragging = false;
  lastDrag = Date.now();
});

function redraw()
{
  if(view.startsWith("3d"))
  {
    draw3DView();
    return;
  }

  if(view === "rod")
  {
    drawRodLive();
    return;
  }

  if(view === "strike" && replay)
  {
    const n = replay.e.length;

    drawLanes(
      replay.e,
      new Array(n).fill(replay.thr),
      replay.t,
      new Array(n).fill(replay.allow),
      n,
      replay.pre,
      replay.dir
    );
  }
  else
  {
    drawLanes(liveEnergy, liveThr, liveTilt, liveAllow, maxPoints, -1, lastDir);
  }
}

function drawLanes(E, THR, T, AL, total, marker, signed)
{
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;

  ctx.clearRect(0, 0, width, height);

  const bg = ctx.createLinearGradient(0, 0, 0, height);
  bg.addColorStop(0, "#07122a");
  bg.addColorStop(1, "#02060f");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, width, height);

  const left = 48;
  const right = 10;
  const top = 10;
  const bottom = 12;
  const gap = 16;

  const gw = width - left - right;
  const laneH = (height - top - bottom - gap) / 2;

  const lane1Top = top;
  const lane2Top = top + laneH + gap;

  const n = E.length;
  const offset = total - n;

  function xAt(i)
  {
    return left + gw * (i + offset) / Math.max(1, total - 1);
  }

  // ---------- Scales ----------

  let maxE = 0.02;

  for(const v of E) if(v > maxE) maxE = v;
  for(const v of THR) if(v * 1.6 > maxE) maxE = v * 1.6;

  maxE *= 1.12;

  let maxT = 1.0;

  for(const v of T) if(Math.abs(v) > maxT) maxT = Math.abs(v);
  for(const v of AL) if(v * 1.6 > maxT) maxT = v * 1.6;

  maxT *= 1.12;

  const minT = signed ? -maxT : 0;

  function yE(v)
  {
    return lane1Top + laneH - Math.min(v / maxE, 1) * laneH;
  }

  function yT(v)
  {
    const r = (Math.max(minT, Math.min(maxT, v)) - minT) / (maxT - minT);
    return lane2Top + laneH - r * laneH;
  }

  // ---------- Grid + labels ----------

  ctx.font = "10px Arial";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";

  for(let i = 0; i <= 4; i++)
  {
    const v = maxE * (1 - i / 4);
    const y = yE(v);

    ctx.strokeStyle = "rgba(255,255,255,.055)";
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(left + gw, y);
    ctx.stroke();

    ctx.fillStyle = "#4f6588";
    ctx.fillText(v.toFixed(3), left - 6, y);
  }

  for(let i = 0; i <= 4; i++)
  {
    const v = maxT - (maxT - minT) * i / 4;
    const y = yT(v);

    ctx.strokeStyle = Math.abs(v) < 0.0001 ? "rgba(255,255,255,.16)" : "rgba(255,255,255,.055)";
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(left + gw, y);
    ctx.stroke();

    ctx.fillStyle = "#4f6588";
    ctx.fillText(v.toFixed(1) + "°", left - 6, y);
  }

  for(let i = 0; i <= 8; i++)
  {
    const x = left + gw * i / 8;

    ctx.strokeStyle = "rgba(255,255,255,.035)";
    ctx.beginPath();
    ctx.moveTo(x, lane1Top);
    ctx.lineTo(x, lane1Top + laneH);
    ctx.moveTo(x, lane2Top);
    ctx.lineTo(x, lane2Top + laneH);
    ctx.stroke();
  }

  ctx.textAlign = "left";
  ctx.fillStyle = "#4c6286";
  ctx.fillText("KNOCK ENERGY (g)", left + 6, lane1Top + 9);
  ctx.fillText(signed ? "TILT (+ toward water / - away)" : "TILT (amount, direction not set)", left + 6, lane2Top + 9);

  // ---------- Series ----------

  function line(data, yFn, colour, width, dash)
  {
    if(data.length < 2)
      return;

    ctx.beginPath();
    ctx.moveTo(xAt(0), yFn(data[0]));

    for(let i = 1; i < data.length; i++)
      ctx.lineTo(xAt(i), yFn(data[i]));

    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  function area(data, yFn, baseY, fillColour)
  {
    if(data.length < 2)
      return;

    ctx.beginPath();
    ctx.moveTo(xAt(0), baseY);

    for(let i = 0; i < data.length; i++)
      ctx.lineTo(xAt(i), yFn(data[i]));

    ctx.lineTo(xAt(data.length - 1), baseY);
    ctx.closePath();
    ctx.fillStyle = fillColour;
    ctx.fill();
  }

  area(E, yE, yE(0), "rgba(34,211,255,.13)");
  line(E, yE, "#22d3ff", 1.6);
  line(THR, yE, "rgba(255,186,82,.75)", 1, [5, 4]);

  area(T, yT, yT(0), "rgba(155,140,255,.12)");
  line(T, yT, "#9b8cff", 1.8);
  line(AL, yT, "rgba(255,186,82,.75)", 1, [5, 4]);

  if(signed)
    line(AL.map(v => -v), yT, "rgba(255,186,82,.45)", 1, [5, 4]);

  // ---------- Strike marker ----------

  if(marker >= 0)
  {
    const x = xAt(marker);

    ctx.strokeStyle = "#ff5757";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(x, lane1Top);
    ctx.lineTo(x, lane2Top + laneH);
    ctx.stroke();

    ctx.fillStyle = "#ff5757";
    ctx.textAlign = "left";
    ctx.fillText("STRIKE", x + 4, lane1Top + 22);

    ctx.fillStyle = "#4f6588";
    ctx.textAlign = "center";
    ctx.fillText("-4 s", left + 14, lane2Top + laneH + 6);
    ctx.fillText("+4 s", left + gw - 14, lane2Top + laneH + 6);
  }
}

// =====================================================
// GRAPH VIEW
// =====================================================

let replayTryAt = 0;
let rstShown = false;
let replayFailed = false;

async function loadReplay(id)
{
  replayTryAt = Date.now();

  try
  {
    const response = await fetch("/strike");

    if(!response.ok)
      throw new Error("rod answered " + response.status);

    replay = await response.json();
    replayID = id;
    replayFailed = false;
  }
  catch(e)
  {
    replayFailed = true;
  }
}

async function setView(v)
{
  view = v;

  el("viewLive").classList.toggle("enabled", v === "live");
  el("viewRod").classList.toggle("enabled", v === "rod");
  el("viewStrike").classList.toggle("enabled", v === "strike");
  el("view3d").classList.toggle("enabled", v === "3d");
  el("view3dwave").classList.toggle("enabled", v === "3dwave");
  el("view3dwind").classList.toggle("enabled", v === "3dwind");

  canvas.style.touchAction = v.startsWith("3d") ? "none" : "auto";

  if(v === "strike")
  {
    if(replayID !== lastStrikeID)
    {
      el("graphSub").innerText = "Loading the strike from the rod\u2026";
      await loadReplay(lastStrikeID);
    }

    el("graphTitle").innerText = "LAST STRIKE REPLAY";
    el("graphSub").innerText = replay && replayID === lastStrikeID ? replay.type + " · 4 s before and after"
      : replayFailed ? "Couldn\u2019t load it \u2013 tap LAST STRIKE to try again"
      : "No strike recorded yet";
  }
  else if(v === "rod")
  {
    el("graphTitle").innerText = "LIVE ROD";
    el("graphSub").innerText = "Your rod on the beach, moving with the real readings · bend drawn bigger than real so you can see it";
  }
  else if(v === "3dwave")
  {
    el("graphTitle").innerText = "3D WAVES";
    el("graphSub").innerText = "The sea as the rod feels it · height = wave swing, speed = wave time, ripples = wind · drag to turn";
  }
  else if(v === "3dwind")
  {
    el("graphTitle").innerText = "3D WIND";
    el("graphSub").innerText = "Flag = wind strength on the rod · hangs when calm, flies and flaps when strong · drag to turn";
  }
  else if(v === "3d")
  {
    el("graphTitle").innerText = "3D NOISE";
    el("graphSub").innerText = "Each dot = how the tip shook (100 per second, last 3 s) · orange = knock limit · drag to turn";
  }
  else
  {
    el("graphTitle").innerText = "LIVE ROD ACTIVITY";
    el("graphSub").innerText = "Top: knock energy · Bottom: tilt (+ toward water)";
  }

  redraw();
}

// =====================================================
// BUTTONS
// =====================================================

// Only needed when the sensor is mounted differently: one pull test
async function teachDir()
{
  if(!confirm("New pull direction?\n\nOnly needed if the sensor is mounted differently.\nThe rod will ask you to pull the line gently once."))
    return;

  stopContinuousAlarm();
  await fetch("/teachdir");
}

async function relearn()
{
  stopContinuousAlarm();
  await fetch("/relearn");
}

async function watching()
{
  stopContinuousAlarm();
  await fetch("/watching");
}

async function reelIn()
{
  stopContinuousAlarm();
  startRodAnim("reel");
  await fetch("/reelin");
}

async function fishOut()
{
  await fetch("/fishout");
  if(window.StrikeApp && StrikeApp.fishOut) StrikeApp.fishOut();   // the app: the photo page
}

// in the app: the catch page (3 photos, kg, cm, fish)
function addPhotos()
{
  if(window.StrikeApp && StrikeApp.fishOut) StrikeApp.fishOut();
}

async function lostFish()
{
  await fetch("/lostfish");
}

async function noFish()
{
  await fetch("/nofish");
}

function fmtLand(sec)
{
  return Math.floor(sec / 60) + ":" + ("0" + (sec % 60)).slice(-2);
}

function showLand(d)
{
  var land = d.land || 0;
  var card = el("landCard");

  card.style.display = land ? "block" : "none";
  el("fishOutButton").style.display = land === 1 ? "flex" : "none";
  el("lostButton").style.display = land === 1 ? "flex" : "none";
  el("noFishButton").style.display = land === 1 ? "flex" : "none";

  if(!land)
    return;

  card.className = "landCard" + (land === 2 ? " done" : land >= 3 ? " lost" : "");
  el("landTitle").innerText = land === 1 ? "FIGHT" : land === 2 ? "FISH OUT" : land === 3 ? "LOST IT" : "NO FISH";
  el("landTime").innerText = fmtLand(d.landsec || 0);
  var app = !!(window.StrikeApp && StrikeApp.fishOut);
  el("landPhotos").style.display = land === 2 && app ? "block" : "none";
  el("landSub").innerText = land === 1 ? "from REEL IN - how did it end?" : land === 2 ? (app ? "fight time" : "fight time - add photos in the app") : land === 4 ? "no fish this time" : "fight time";
}

async function ready()
{
  stopContinuousAlarm();
  startRodAnim("cast");
  await fetch("/ready");
}

// =====================================================
// PULL TEST POP-UPS
// =====================================================

let ptPrev = "";
let ptOpen = false;
let ptHidden = false;
let ptDoneTimer = 0;
const ptCanvas = el("ptCanvas");
const ptCtx = ptCanvas.getContext("2d");

function pullTestPopup(d)
{
  if(d.state === "pulltest")
  {
    if(ptPrev !== "pulltest")
    {
      ptHidden = false;
      tone(880, 120, 0);
      tone(1175, 160, 160);
    }

    if(!ptHidden && !ptOpen)
      openPullTest();

    const frac = d.ptneed > 0 ? Math.min(1, Math.max(0, d.ptdeg) / d.ptneed) : 0;

    el("ptFill").style.width = (frac * 100) + "%";
    el("ptFill").classList.toggle("full", frac >= 1);
    el("ptMsg").innerText = frac >= 1
      ? "Good – now let go"
      : "Tip moved " + Math.max(0, d.ptdeg).toFixed(1) + "° of " + d.ptneed.toFixed(1) + "°";
  }
  else if(ptPrev === "pulltest")
  {
    closePullTest();
    showPullDone(d.dir && d.dirage >= 0 && d.dirage < 15);
  }

  ptPrev = d.state;
}

function openPullTest()
{
  ptOpen = true;
  el("ptModal").classList.add("show");
  ptLoop();
}

function hidePullTest()
{
  ptHidden = true;
  closePullTest();
}

function closePullTest()
{
  ptOpen = false;
  el("ptModal").classList.remove("show");
}

function showPullDone(ok)
{
  el("ptDoneCard").classList.toggle("skipped", !ok);
  el("ptDonePath").setAttribute("d", ok ? "M14 27l8 8 16-17" : "M26 14v15 M26 37v1");
  el("ptDoneTitle").innerText = ok ? "DIRECTION LEARNED" : "PULL TEST SKIPPED";
  el("ptDoneText").innerText = ok
    ? "The rod now knows which way is toward the water. Slow pulls and drop-backs are watched. Rod armed."
    : "No direction yet. Knocks and strong hits are still detected, but slow pulls and drop-backs need the pull test. Do it after the next READY.";

  // replay the drawing animation
  const svg = el("ptDoneCard").querySelector(".ptTick");
  svg.replaceWith(svg.cloneNode(true));

  el("ptDone").classList.add("show");

  if(ok)
  {
    tone(1047, 120, 0);
    tone(1319, 120, 130);
    tone(1568, 220, 260);
  }

  clearTimeout(ptDoneTimer);
  ptDoneTimer = setTimeout(closePullDone, ok ? 6000 : 15000);
}

function closePullDone()
{
  el("ptDone").classList.remove("show");
}

// How-to animation: hold the line, pull gently toward the sea, let go
function ptLoop()
{
  if(!ptOpen)
    return;

  const box = ptCanvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;

  if(ptCanvas.width !== Math.floor(box.width * dpr) || ptCanvas.height !== Math.floor(box.height * dpr))
  {
    ptCanvas.width = Math.floor(box.width * dpr);
    ptCanvas.height = Math.floor(box.height * dpr);
  }

  ptCtx.setTransform(dpr, 0, 0, dpr, 0, 0);

  const w = box.width, h = box.height;
  const now = performance.now() / 1000;
  const c = now % 4.2;
  const ease = f => (1 - Math.cos(Math.max(0, Math.min(1, f)) * Math.PI)) / 2;

  let pull = 0, hand = 1, step = "";

  if(c < 0.6)       { hand = c / 0.6; step = "1  Hold the line"; }
  else if(c < 1.9)  { pull = ease((c - 0.6) / 1.3); step = "2  Pull gently toward the sea"; }
  else if(c < 2.5)  { pull = 1; step = "2  Pull gently toward the sea"; }
  else if(c < 2.8)  { pull = 1 - (c - 2.5) / 0.3; hand = 1 - (c - 2.5) / 0.3; step = "3  Let go"; }
  else              { hand = 0; step = "3  Let go ✓"; }

  let tilt = pull * 3;

  if(c >= 2.5)
    tilt += 1.2 * Math.exp(-(c - 2.5) * 3) * Math.sin((c - 2.5) * 14);

  const r = drawRodScene(ptCtx, w, h,
  {
    t: now, tilt: tilt, knock: 0, state: pull > 0.05 ? "pull" : "armed", label: "",
    dir: true, waveswing: 0.6, period: 6, wind: 0, allow: 0, bucket: 0, drop: 0,
    mode: "normal", anim: 0, hideLine: hand > 0, noHud: true
  });

  const tip = r.tip, end = r.end;
  const dx = end[0] - tip[0], dy = end[1] - tip[1];
  const len = Math.hypot(dx, dy);
  const ux = dx / len, uy = dy / len;
  const p0 = [tip[0] + dx * 0.22, tip[1] + dy * 0.22];
  const hp = [p0[0] + (ux * 0.6 + 0.4) * pull * 26, p0[1] + (uy * 0.6 + 0.8) * pull * 26];

  if(hand > 0)
  {
    ptCtx.strokeStyle = "rgba(234,243,255,.75)";
    ptCtx.lineWidth = 1.2;
    ptCtx.beginPath();
    ptCtx.moveTo(tip[0], tip[1]);
    ptCtx.lineTo(hp[0], hp[1]);
    ptCtx.lineTo(end[0], end[1]);
    ptCtx.stroke();

    // arrow: which way to pull
    if(c > 0.6 && c < 2.5)
    {
      const a0 = [p0[0] + 16, p0[1] + 4], a1 = [p0[0] + 16 + 34, p0[1] + 4 + 30];

      ptCtx.strokeStyle = "#22d3ff";
      ptCtx.fillStyle = "#22d3ff";
      ptCtx.lineWidth = 2.5;
      ptCtx.beginPath(); ptCtx.moveTo(a0[0], a0[1]); ptCtx.lineTo(a1[0], a1[1]); ptCtx.stroke();
      ptCtx.beginPath(); ptCtx.moveTo(a1[0] + 3, a1[1] + 3); ptCtx.lineTo(a1[0] - 9, a1[1] - 1); ptCtx.lineTo(a1[0] - 1, a1[1] - 9); ptCtx.closePath(); ptCtx.fill();
      ptCtx.font = "bold 12px Arial";
      ptCtx.textAlign = "left";
      ptCtx.textBaseline = "middle";
      ptCtx.fillText("gently", a1[0] + 6, a1[1] + 4);
    }

    // hand gripping the line
    const z = Math.max(1.4, w / 280);
    const hx = hp[0], hy = hp[1];

    ptCtx.globalAlpha = hand;
    ptCtx.fillStyle = "#1e7bff";                                                                         // sleeve
    ptCtx.beginPath(); ptCtx.ellipse(hx + 13 * z, hy + 17 * z, 7 * z, 10 * z, -0.6, 0, Math.PI * 2); ptCtx.fill();
    ptCtx.fillStyle = "#f2c6a0";
    ptCtx.strokeStyle = "#8a5a3c";
    ptCtx.lineWidth = 1.2;
    ptCtx.beginPath(); ptCtx.ellipse(hx + 5 * z, hy + 6 * z, 7.5 * z, 9 * z, -0.6, 0, Math.PI * 2); ptCtx.fill(); ptCtx.stroke();    // fist

    for(let i = 0; i < 4; i++)                                                                            // fingers round the line
    {
      ptCtx.beginPath();
      ptCtx.ellipse(hx + (1 + i * 2.6) * z, hy + (-1 + i * 3.2) * z, 2.6 * z, 2 * z, -0.6, 0, Math.PI * 2);
      ptCtx.fill(); ptCtx.stroke();
    }

    ptCtx.beginPath(); ptCtx.ellipse(hx - 2 * z, hy + 5 * z, 2.4 * z, 5 * z, 0.5, 0, Math.PI * 2); ptCtx.fill(); ptCtx.stroke();     // thumb
    ptCtx.globalAlpha = 1;
  }

  ptCtx.font = "bold 15px Arial";
  ptCtx.textAlign = "left";
  ptCtx.textBaseline = "top";
  ptCtx.fillStyle = "rgba(4,10,24,.6)";
  ptCtx.fillRect(8, 8, ptCtx.measureText(step).width + 16, 26);
  ptCtx.fillStyle = c < 2.5 ? "#ffba52" : "#22d3ff";
  ptCtx.fillText(step, 16, 13);

  requestAnimationFrame(ptLoop);
}

async function skipPull()
{
  await fetch("/skippull");
}

// =====================================================
// LIVE SETTINGS
// =====================================================

let settings = null;
let slideTimer = null;

function showSettings(st)
{
  settings = st;

  for(const k of ["kmul", "pdeg", "ptime", "rcnt", "rwin", "sharp", "pmul"])
    el("s_" + k).value = st[k];

  el("v_kmul").innerText = "\u00D7" + st.kmul.toFixed(2);
  el("v_rep").innerText = Math.round(st.rcnt) + " in " + st.rwin.toFixed(1) + " s";
  el("v_sharp").innerText = st.sharp.toFixed(2) + " g (~" + (st.sharp / 0.3).toFixed(2) + " g hit)";
  el("v_pdeg").innerText = st.pdeg.toFixed(1) + "\u00B0";
  el("v_ptime").innerText = st.ptime.toFixed(1) + " s";
  el("v_pmul").innerText = "\u00D7" + st.pmul.toFixed(2);

  for(const k of ["kbeep", "drop"])
  {
    el("t_" + k).classList.toggle("on", st[k] > 0.5);
    el("t_" + k).innerText = st[k] > 0.5 ? "ON" : "OFF";
  }

  el("saveState").innerText = st.saved
    ? "\u2713 Saved \u2013 kept after power off"
    : "Changed \u2013 press SAVE to keep after power off";
  el("saveState").classList.toggle("saved", st.saved);
}

async function settingsCall(url)
{
  try
  {
    const response = await fetch(url);
    showSettings(await response.json());
  }
  catch(e)
  {
  }
}

function openSettings()
{
  el("drawer").classList.add("open");
  showAccount();
  settingsCall("/settings");
  presetCall("/presets");
  showSoundChoices();
}

// ---------- my setups (5 on the rod) + Default ----------

let presets = [];

function escapeHtml(t)
{
  return String(t).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

async function presetCall(url)
{
  try
  {
    const r = await fetch(url);

    if(r.ok)
    {
      presets = (await r.json()).presets || [];
      showPresets();
    }
  }
  catch(e)
  {
  }
}

function showPresets()
{
  let h = "";

  for(let i = 0; i < 5; i++)
  {
    const n = presets[i] || "";

    h += '<div class="presetRow">' +
         '<button class="pName" onclick="loadPreset(' + i + ')"' + (n ? "" : " disabled") + '>' +
         (n ? escapeHtml(n) : (i + 1) + " \u2013 empty") + '</button>' +
         '<button class="pSave" onclick="savePreset(' + i + ')">SAVE HERE</button>' +
         (n ? '<button class="pDel" onclick="delPreset(' + i + ', this)">&#10005;</button>' : '') +
         '</div>';
  }

  el("presetList").innerHTML = h;
}

function presetMsg(t)
{
  el("presetMsg").innerText = t;
}

async function loadPreset(i)
{
  try
  {
    const r = await fetch("/presetload?s=" + i);

    if(r.ok)
    {
      showSettings(await r.json());
      presetMsg("Loaded \u201c" + presets[i] + "\u201d \u2013 press SAVE to use it");
    }
  }
  catch(e)
  {
  }
}

function savePreset(i)
{
  const n = el("presetNameIn").value.trim() || presets[i] || "";

  presetCall("/presetsave?s=" + i + "&n=" + encodeURIComponent(n));
  el("presetNameIn").value = "";
  presetMsg("The current settings are kept in place " + (i + 1) + ".");
}

function delPreset(i, b)
{
  if(!b.classList.contains("sure"))
  {
    b.classList.add("sure");
    b.innerText = "SURE?";
    setTimeout(() => { b.classList.remove("sure"); b.innerHTML = "&#10005;"; }, 3000);
    return;
  }

  presetCall("/presetdel?s=" + i);
  presetMsg("Deleted.");
}

function loadDefault()
{
  defaultSettings();
  presetMsg("Default loaded \u2013 press SAVE to use it");
}

function closeSettings()
{
  el("drawer").classList.remove("open");

  if(window.StrikeApp && StrikeApp.settingsClosed)
    StrikeApp.settingsClosed();
}

// Send while sliding, but not more than ~6 times a second
function slide(k)
{
  const v = parseFloat(el("s_" + k).value);

  if(settings)
  {
    settings[k] = v;
    settings.saved = false;
    showSettings(settings);
  }

  clearTimeout(slideTimer);
  slideTimer = setTimeout(() => settingsCall("/set?k=" + k + "&v=" + v), 150);
}

function toggle(k)
{
  const v = settings && settings[k] > 0.5 ? 0 : 1;
  settingsCall("/set?k=" + k + "&v=" + v);
}

// SAVE: the rod keeps the settings, a short "Saved", the panel closes
async function saveSettings()
{
  await settingsCall("/savesettings");

  const t = el("savedToast");
  t.classList.add("show");
  setTimeout(() => t.classList.remove("show"), 1800);

  setTimeout(closeSettings, 500);
}

function defaultSettings()
{
  settingsCall("/defaultsettings");
}

function showLiveInDrawer(d)
{
  if(!el("drawer").classList.contains("open"))
    return;

  el("lvKnock").innerText = d.energy.toFixed(3) + " / " + d.knockthr.toFixed(3);
  el("lvTilt").innerText = (d.tilt >= 0 && d.dir ? "+" : "") + d.tilt.toFixed(1) + "\u00B0 / " + d.allow.toFixed(1) + "\u00B0";
  el("lvSlow").innerText = Math.max(d.bucket, d.drop) + "%";
  el("lvKnocks").innerText = d.knocks;
}

// =====================================================
// RUNNING TIME
// =====================================================

function clock(sec)
{
  const h = Math.floor(sec / 3600);
  const m = Math.floor(sec / 60) % 60;
  const s = sec % 60;

  return h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0");
}

function hours(sec)
{
  const h = sec / 3600;

  return h < 10 ? h.toFixed(1) + "h" : Math.round(h) + "h";
}

function showTimes(d)
{
  el("upTime").innerText = clock(d.up);
  showWatch(d);
  showLinkMode(d);
  showChan(d);
  showRod(d);

  if(d.bat >= 0)
  {
    el("batText").innerText = d.bat + "% · " + d.batv.toFixed(2) + " V";
    el("batFill").style.width = (d.bat * 0.17) + "px";
    el("batChip").className = "timeChip " + (d.bat < 15 ? "batLow" : d.bat < 35 ? "batMid" : "batOk");
    el("batChip").title = "Battery " + d.bat + "% (" + d.batv.toFixed(2) + " V)";
  }
  else
  {
    el("batText").innerText = "no battery";
    el("batFill").style.width = "0";
    el("batChip").className = "timeChip";
    el("batChip").title = "No battery reading (USB only, or the divider on D1 is not wired)";
  }
  el("armedTime").innerText = d.armed >= 0 ? clock(d.armed) : "\u2013";
  el("totalTime").innerText = hours(d.total);
}

async function setSea(mode)
{
  await fetch("/sea?m=" + mode);
}

// =====================================================
// LIVE UPDATE
// =====================================================

const WAVE_NAMES = ["Flat", "Small", "Medium", "Big"];
const WIND_NAMES = ["Calm", "Normal", "Strong", "Very strong"];
const TIDE_NAMES = ["Steady", "Slow drift", "Fast drift"];
const SEA_NAMES = ["CALM", "MODERATE", "ROUGH"];
const SEA_CLASS = ["green", "orange", "red"];
const SEA_KEYS = ["calm", "mod", "rough"];
const LEVEL_COLOURS = ["#22d3ff", "#22d3ff", "#ffba52", "#ff5757"];

let lastKnockID = -1;
let lastStrikeID = 0;
let lastDir = false;

function setCondition(name, label, num, pct, level)
{
  el(name + "Label").innerText = label;
  el(name + "Num").innerText = num;
  el(name + "Fill").style.width = Math.max(4, Math.min(100, pct)) + "%";
  el(name + "Fill").style.background = LEVEL_COLOURS[level];
}

function showConditions(d)
{
  if(d.state === "learning")
  {
    el("seaValue").innerText = "LEARNING";
    el("seaValue").className = "seaValue orange";

    for(const n of ["wave", "wind", "tide"])
    {
      el(n + "Label").innerText = "…";
      el(n + "Num").innerText = "";
      el(n + "Fill").style.width = "0%";
    }

    el("details").innerText = "Keep the rod still";
    return;
  }

  el("seaValue").innerText = "● " + SEA_NAMES[d.sea];
  el("seaValue").className = "seaValue " + SEA_CLASS[d.sea];

  el("modeBadge").innerText = d.seaauto ? "AUTO" : "MANUAL";
  el("modeBadge").classList.toggle("manual", !d.seaauto);

  for(const k of ["auto", "calm", "mod", "rough"])
    el("seg_" + k).classList.remove("on");

  el("seg_" + (d.seaauto ? "auto" : SEA_KEYS[d.sea])).classList.add("on");

  let waveNum = "±" + d.waveswing.toFixed(1) + "°";

  if(d.period > 0)
    waveNum += " · " + d.period.toFixed(0) + " s";

  setCondition("wave", WAVE_NAMES[d.wave], waveNum, d.waveswing / 4.0 * 100, d.wave);

  const windNum = d.dir
    ? "±" + d.windswing.toFixed(1) + "°"
    : d.windvib.toFixed(3) + " g";

  const windPct = d.dir
    ? Math.max(d.windswing / 4.0, d.windvib / 0.03) * 100
    : d.windvib / 0.03 * 100;

  setCondition("wind", WIND_NAMES[d.wind], windNum, windPct, d.wind);

  setCondition("tide", TIDE_NAMES[d.tide], d.tiderate.toFixed(1) + "°/min", d.tiderate / 3.0 * 100, d.tide);

  el("details").innerText =
    "knock limit " + d.knockthr.toFixed(3) + " g · pull allow " + d.allow.toFixed(1) + "°" +
    "\nsensor noise " + d.noise.toFixed(3) + " g · " + d.sname + (d.hr ? " ✓" : " high-res OFF") + (d.serr > 0 ? " · " + d.serr + " read errors" : "");
}

function showButtons(d)
{
  showLand(d);
  el("watchButton").style.display = "none";
  el("reelButton").style.display = "none";
  el("readyButton").style.display = "none";
  el("skipButton").style.display = "none";

  if(d.strike)
  {
    el("watchButton").style.display = "block";
    el("reelButton").style.display = "block";
  }
  else if(d.state === "paused")
  {
    el("readyButton").style.display = d.land === 1 ? "none" : "block";   // fighting: FISH OUT / Lost it first
  }
  else if(d.state === "pulltest")
  {
    el("skipButton").style.display = "block";
  }
}

function setStatus(main, cls, state, sub)
{
  el("mainStatus").innerText = main;
  el("mainStatus").className = cls;
  el("stateText").innerText = state;
  el("stateSub").innerText = sub;
}

function showState(d)
{
  if(d.strike)
  {
    setStatus(
      "STRIKE DETECTED", "red", d.type,
      "Peak knock " + d.peakenergy.toFixed(3) + " g | Peak tilt " + d.peaktilt.toFixed(1) +
      "° | Alarm continues until acknowledged."
    );
  }
  else if(d.state === "paused")
  {
    setStatus("DETECTION PAUSED", "orange", "REEL IN MODE", "Move or reel freely. Detection is disabled.");
  }
  else if(d.state === "learning")
  {
    setStatus("LEARNING", "orange", "LEARNING ENVIRONMENT", d.timeleft + " seconds remaining. Keep the rod still.");
  }
  else if(d.state === "pulltest")
  {
    setStatus(
      "PULL TEST", "orange", "PULL THE LINE GENTLY",
      "Pull the line toward the water once, then let go. Tip moved " + d.ptdeg.toFixed(1) +
      "° (need " + d.ptneed.toFixed(1) + "°)."
    );
  }
  else if(d.state === "settling")
  {
    setStatus(
      "WATCHING + SETTLING", "orange", "LEARNING NEW POSITION",
      d.grace
        ? "Place the rod. Knocks are ignored for a few seconds."
        : "Waiting for the rod to become stable. Knocks are still detected."
    );
  }
  else
  {
    let sub = "Watching knocks, slow pulls and drop-backs.";

    if(d.gustago >= 0 && d.gustago < 5)
      sub = "Strong gust ignored (long shake, no pull toward the water).";
    else if(d.dropwatch)
      sub = "Drop-back seen " + d.dropseen + " s ago (line slack) \u2013 no alarm. Watching for a pull forward\u2026";
    else if(d.dirage >= 0 && d.dirage < 6)
      sub = "Direction learned ✓ Rod armed.";
    else if(d.bucket > 60)
      sub = "Possible slow pull building up…";
    else if(d.drop > 60)
      sub = "Possible drop-back building up…";
    else if(d.knocks > 0)
      sub = d.knocks + " knock(s) in the last 4 s…";

    setStatus("MONITORING", "green", "ROD ARMED", sub);
  }
}

function flashKnock()
{
  el("knockBox").classList.add("knockFlash");
  setTimeout(() => el("knockBox").classList.remove("knockFlash"), 350);
}

async function updateData()
{
  try
  {
    const response = await fetch("/data");
    const d = await response.json();

    el("connState").innerHTML = "&#9679; CONNECTED";
    el("connState").className = "connected";

    hideSplash();

    lastDir = d.dir;

    el("energy").innerText = d.energy.toFixed(3);
    el("knockThr").innerText = d.knockthr.toFixed(3);
    el("tilt").innerText = (d.tilt >= 0 && d.dir ? "+" : "") + d.tilt.toFixed(1);
    el("knockCount").innerText = d.knocks;

    el("slowScore").innerText = d.bucket + "%";
    el("slowFill").style.width = d.bucket + "%";
    el("dropScore").innerText = d.dir ? d.drop + "%" : "OFF";
    el("dropFill").style.width = d.drop + "%";

    el("settleScore").innerText = d.confidence + "%";
    el("settleFill").style.width = d.confidence + "%";

    el("dirLine").innerText = d.dir
      ? "Direction: learned ✓ (+ = toward water)"
      : "Direction: not set – do a pull test after READY";

    el("graphEnergy").innerText = d.energy.toFixed(3);
    el("graphTilt").innerText = d.tilt.toFixed(1);
    el("graphSlow").innerText = Math.max(d.bucket, d.drop) + "%";
    el("lastStrike").innerText = d.lasttype;

    showConditions(d);
    showState(d);
    showButtons(d);
    pullTestPopup(d);
    showTimes(d);
    showLiveInDrawer(d);

    // ---------- Single knock pre-alert ----------

    if(lastKnockID >= 0 && d.knockid !== lastKnockID && !d.strike)
    {
      if(d.kbeep)
        knockPattern();

      flashKnock();
    }

    lastKnockID = d.knockid;

    // ---------- Continuous strike alarm ----------

    if(d.strike)
    {
      if(d.strikeid !== lastPlayedStrikeID || activeAlarmType === "")
      {
        lastPlayedStrikeID = d.strikeid;
        startContinuousAlarm(d.type);
      }
    }
    else
    {
      stopContinuousAlarm();
    }

    // ---------- The rod restarted by itself (watchdog, crash, low battery)? ----------

    if(d.rst && d.rst !== "power" && d.rst !== "software" && !rstShown)
    {
      rstShown = true;
      const t = el("savedToast");
      t.innerText = "\u26A0 The rod restarted by itself (" + d.rst + ") \u2013 it is watching again";
      t.style.color = "#ffba52";
      t.style.borderColor = "#ffba52";
      t.classList.add("show");
      setTimeout(() => { t.classList.remove("show"); t.innerText = "\u2713 Saved"; t.style.color = ""; t.style.borderColor = ""; }, 8000);
    }

    // ---------- Replay availability ----------

    lastStrikeID = d.strikeid;

    el("viewStrike").disabled = !d.strikeready;

    if(view === "strike" && d.strikeready && replayID !== d.strikeid && Date.now() - replayTryAt > 5000)
    {
      await setView("strike");
    }

    // ---------- Live graph ----------

    addNoisePoints(d);

    liveEnergy.push(d.energy);
    liveThr.push(d.knockthr);
    liveTilt.push(d.tilt);
    liveAllow.push(d.allow);

    if(liveEnergy.length > maxPoints)
    {
      liveEnergy.shift();
      liveThr.shift();
      liveTilt.shift();
      liveAllow.shift();
    }

    if(view === "live")
      redraw();
  }
  catch(e)
  {
    el("connState").innerHTML = "&#9679; NO CONNECTION";
    el("connState").className = "red";
  }
}

// Ask again only after the previous answer arrived
async function pollLoop()
{
  if(watchOnly)
  {
    setTimeout(pollLoop, 500);
    return;
  }

  await updateData();
  setTimeout(pollLoop, 50);             // 20 times a second
}

const splashShownAt = Date.now();
let splashDone = false;

function hideSplash()
{
  if(splashDone)
    return;

  splashDone = true;

  setTimeout(() =>
  {
    el("splash").classList.add("hide");
    setTimeout(() => el("splash").remove(), 700);
  }, Math.max(0, 1800 - (Date.now() - splashShownAt)));
}

setTimeout(hideSplash, 6000);   // never stay longer than this

resizeCanvas();
pollLoop();
noiseLoop();

</script>

<!-- SETTINGS DRAWER -->

<div class="drawer" id="drawer">

<div class="drawerHead">
<div class="drawerTitle">&#9881; LIVE SETTINGS</div>
<button class="closeBtn" onclick="closeSettings()">&times;</button>
</div>

<div class="setHelp">Changes work immediately. Watch the live values and the dashed lines on the graph.</div>

<div class="setGroup" id="lmGroup">WATCH LINK</div>

<div class="whoSeg" id="lmSeg">
<button id="lm_auto" onclick="setLinkMode('auto')">Auto</button>
<button id="lm_bt" onclick="setLinkMode('bt')">Bluetooth only</button>
<button id="lm_lora" onclick="setLinkMode('lora')">LoRa only</button>
</div>
<div class="setHelp" id="lmHelp">&ndash;</div>

<div class="setGroup" id="chGroup">LoRa CHANNEL</div>
<div class="whoSeg" id="chSeg">
<button id="ch_0" onclick="setChan(0)">Auto</button>
<button id="ch_1" onclick="setChan(1)">500 m</button>
<button id="ch_2" onclick="setChan(2)">1 km A</button>
<button id="ch_3" onclick="setChan(3)">1 km B</button>
</div>
<div class="setHelp" id="chHelp">&ndash;</div>

<script>
// rod 2.4: "lchm" (choice), "lchan" (now), "lqual", "lrods" in /data
var CH_HELP = [
  'Starts on 500 m (fast: the ring moves 3-4 times a second), moves to 1 km by itself when the watch is far, back when near.',
  '500 m, fast. Stay within about 500 m of the rod.',
  '1 km, about one update a second. Pick A or B so friends nearby use different ones.',
  '1 km, about one update a second. Pick A or B so friends nearby use different ones.'
];
var chWanted = -1, chWantedAt = 0;

function showChan(d)
{
  var show = d.lchm !== undefined;
  el('chGroup').style.display = el('chSeg').style.display = el('chHelp').style.display = show ? '' : 'none';

  if(!show)
    return;

  var m = chWanted >= 0 && Date.now() - chWantedAt < 3000 ? chWanted : d.lchm;

  for(var k = 0; k < 4; k++)
    el('ch_' + k).className = k == m ? 'on' : '';

  var QUAL = ['Clear', 'Good', 'Busy', 'Congested'];
  el('chHelp').innerText = CH_HELP[m] + '  Now: ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined ? ' · ' + QUAL[d.lqual] : '') + (d.lrods ? ' (' + d.lrods + ' rods near)' : '');
}

async function setChan(k)
{
  chWanted = k;
  chWantedAt = Date.now();

  for(var i = 0; i < 4; i++)
    el('ch_' + i).className = i == k ? 'on' : '';

  try
  {
    await fetch('/lorachan?m=' + ['auto', 'fast', 'long1', 'long2'][k]);
  }
  catch(e)
  {
  }
}
</script>

<script>
// How the watch talks to the rod (rod 2.3+: "lmode", "haslora" in /data)
var LM_HELP = {
  auto: 'Bluetooth near the rod (smooth live graph), LoRa when far (up to ~1 km). Best for most.',
  bt: 'LoRa off: saves rod and watch battery. Stay within ~30 m of the rod.',
  lora: 'The watch leaves Bluetooth: updates 1-2 times a second, up to ~1 km. If LoRa goes quiet for 1 min the watch tries Bluetooth again.'
};
var lmWanted = '', lmWantedAt = 0;

function showLinkMode(d)
{
  var show = d.lmode !== undefined;
  el('lmGroup').style.display = el('lmSeg').style.display = el('lmHelp').style.display = show ? '' : 'none';

  if(!show)
    return;

  // just tapped: show the choice until the rod confirms it
  var m = lmWanted && Date.now() - lmWantedAt < 3000 ? lmWanted : d.lmode;

  ['auto', 'bt', 'lora'].forEach(function(k)
  {
    var b = el('lm_' + k);
    b.className = k == m ? 'on' : '';
    b.disabled = k != 'auto' && !d.haslora;
    b.style.opacity = b.disabled ? .35 : 1;
  });

  el('lmHelp').innerText = d.haslora ? LM_HELP[m] || ''
    : 'This rod has no LoRa (the rod sketch without LoRa): the watch uses Bluetooth.';
}

async function setLinkMode(m)
{
  lmWanted = m;
  lmWantedAt = Date.now();
  el('lmHelp').innerText = LM_HELP[m];
  ['auto', 'bt', 'lora'].forEach(function(k) { el('lm_' + k).className = k == m ? 'on' : ''; });

  try
  {
    await fetch('/linkmode?m=' + m);
  }
  catch(e)
  {
  }
}
</script>

<div class="setGroup" id="rodGroup">MY ROD</div>
<div class="setHelp" id="rodInfo">&ndash;</div>
<button class="soundButton appOnly" id="forgetBtn" onclick="forgetRod()">FORGET MY DEVICES</button>

<div id="accBox" style="display:none">
<div class="setGroup">ACCOUNT</div>
<div class="setHelp" id="accInfo">&ndash;</div>
<button class="soundButton" id="accBtn" onclick="accountBtn()">SIGN IN</button>
</div>

<script>
// the phone app's account (not on the rod)
function showAccount()
{
  if(!window.StrikeApp || !StrikeApp.accountName)
    return;

  var name = StrikeApp.accountName();
  el('accBox').style.display = '';
  el('accInfo').innerHTML = name ? '<b style="font-size:15px;color:#eaf3ff">' + name + '</b><br>strikes and fishing spots saved online automatically'
                                 : 'Not signed in: strikes stay on this phone.';
  el('accBtn').innerText = name ? 'LOG OUT' : 'SIGN IN';
}

function accountBtn()
{
  if(!window.StrikeApp)
    return;

  if(StrikeApp.accountName())
  {
    if(confirm('Log out? Strikes already saved stay online. New strikes stay on this phone until you sign in again.'))
    {
      closeSettings();
      StrikeApp.logOut();
    }
  }
  else
  {
    closeSettings();
    StrikeApp.openAccount();
  }
}
</script>

<script>
// rod 2.3+: "rodid", "paired", "pairopen" in /data
function showRod(d)
{
  var show = d.rodid !== undefined;
  el('rodGroup').style.display = el('rodInfo').style.display = show ? '' : 'none';

  if(!show)
    return;

  el('rodInfo').innerHTML = '<b style="font-size:15px;color:#eaf3ff">' + d.rodid + ' &#128274;</b><br>' +
    d.paired + ' paired device' + (d.paired == 1 ? '' : 's') + ' (phone, watch) &middot; encrypted &middot; ' +
    (d.pairopen ? '<span style="color:#ffba52">open for pairing now</span>' : 'locked');
}

function forgetRod()
{
  if(!window.StrikeApp || !StrikeApp.forgetRod)
    return;

  if(!confirm('Forget my devices?\n\nThe rod forgets this phone AND the watch. To use them again, pair each one with the PIN on the rod box (in the 3 minutes after switching the rod on).'))
    return;

  closeSettings();
  StrikeApp.forgetRod();
}
</script>

<div class="setGroup">MY SETUPS</div>

<div class="presetRow">
<button class="pName def" onclick="loadDefault()">&#128274; Default<small>factory settings &ndash; cannot be changed</small></button>
</div>

<div id="presetList"></div>

<input id="presetNameIn" maxlength="24" placeholder="Name, then SAVE HERE (e.g. Calm beach)">
<div class="setHelp" id="presetMsg">Tap a setup to load it, then SAVE. SAVE HERE keeps the current settings in that place.</div>

<div class="setGroup">LIVE NOW</div>

<div class="liveBox">
<div class="liveItem"><div class="gLabel">KNOCK / LIMIT</div><b id="lvKnock">&ndash;</b></div>
<div class="liveItem"><div class="gLabel">TILT / PULL LINE</div><b id="lvTilt">&ndash;</b></div>
<div class="liveItem"><div class="gLabel">SLOW PULL</div><b id="lvSlow">&ndash;</b></div>
<div class="liveItem"><div class="gLabel">KNOCKS</div><b id="lvKnocks">&ndash;</b></div>
</div>

<div class="setGroup">KNOCKS</div>

<div class="setRow">
<div class="setTop"><span>Knock limit</span><strong id="v_kmul"></strong></div>
<input type="range" id="s_kmul" min="0.5" max="4" step="0.05" oninput="slide('kmul')">
<div class="setEnds"><span>more sensitive</span><span>stricter</span></div>
<div class="setHelp">How far above the normal noise a knock must be.</div>
</div>

<div class="setRow">
<div class="setTop"><span>Repeated bite</span><strong id="v_rep"></strong></div>
<input type="range" id="s_rcnt" min="2" max="5" step="1" oninput="slide('rcnt')">
<input type="range" id="s_rwin" min="2" max="8" step="0.5" oninput="slide('rwin')">
<div class="setHelp">Top: how many knocks. Bottom: within how many seconds.</div>
</div>

<div class="setRow">
<div class="setTop"><span>Sharp strike</span><strong id="v_sharp"></strong></div>
<input type="range" id="s_sharp" min="0.03" max="0.30" step="0.01" oninput="slide('sharp')">
<div class="setEnds"><span>lighter hit</span><span>harder hit</span></div>
<div class="setHelp">A single hit this hard is always a strike.</div>
</div>

<div class="setRow switchRow">
<div><div class="setTop"><span>Single knock beep</span></div><div class="setHelp">Short beep for one knock.</div></div>
<button class="switch" id="t_kbeep" onclick="toggle('kbeep')">ON</button>
</div>

<div class="setGroup">PULLS</div>

<div class="setRow">
<div class="setTop"><span>Slow pull sensitivity</span><strong id="v_pmul"></strong></div>
<input type="range" id="s_pmul" min="0.8" max="3" step="0.05" oninput="slide('pmul')">
<div class="setEnds"><span>more sensitive</span><span>stricter</span></div>
<div class="setHelp">How far and how long the tip must lean toward the water. Works in waves too (the waves set the normal lean, this multiplies it).</div>
</div>

<div class="setRow">
<div class="setTop"><span>Pull angle</span><strong id="v_pdeg"></strong></div>
<input type="range" id="s_pdeg" min="0.4" max="5" step="0.1" oninput="slide('pdeg')">
<div class="setEnds"><span>more sensitive</span><span>stricter</span></div>
<div class="setHelp">The tip must lean at least this far. In waves the line goes higher by itself.</div>
</div>

<div class="setRow">
<div class="setTop"><span>Pull time</span><strong id="v_ptime"></strong></div>
<input type="range" id="s_ptime" min="0.5" max="8" step="0.1" oninput="slide('ptime')">
<div class="setEnds"><span>faster alarm</span><span>slower alarm</span></div>
<div class="setHelp">How long the pull must last before the alarm.</div>
</div>

<div class="setRow switchRow">
<div><div class="setTop"><span>Drop-back + pull alarm</span></div><div class="setHelp">Line goes slack, then the tip pulls forward (fish). A drop-back alone is only shown, no alarm.</div></div>
<button class="switch" id="t_drop" onclick="toggle('drop')">ON</button>
</div>

<div class="setGroup">ALARM SOUNDS</div>

<div class="setRow soundRow"><span>Strike / run</span><select id="snd_strike" onchange="chooseSound('strike')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('strike')">&#9654;</button></div>
<div class="setRow soundRow"><span>Slow pull</span><select id="snd_slow" onchange="chooseSound('slow')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('slow')">&#9654;</button></div>
<div class="setRow soundRow"><span>Knocks</span><select id="snd_knock" onchange="chooseSound('knock')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('knock')">&#9654;</button></div>
<div class="setRow soundRow"><span>Drop-back</span><select id="snd_drop" onchange="chooseSound('drop')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('drop')">&#9654;</button></div>
<div class="setHelp">Chosen on this phone. The app plays the same sound in the background.</div>

<div class="saveState" id="saveState">&ndash;</div>

<div class="controls">
<button class="action ready" onclick="saveSettings()">SAVE</button>
<button class="action skip" onclick="defaultSettings()">DEFAULT</button>
</div>

</div>

<div id="savedToast">&#10003; Saved</div>

</body>

</html>
)rawliteral";

// =====================================================
// WEB HANDLERS
// =====================================================

void handleLogo();

void handleRoot()
{
  server.send_P(200, "text/html", webpage);
}

// I'M WATCHING: stop alert, keep protection, learn new rest position
void handleWatching()
{
  clearStrike();
  startSettling(false);

  Serial.println("I'M WATCHING");

  server.send(200, "text/plain", "OK");
}

void handleReelIn()
{
  clearStrike();

  rodState = ST_PAUSED;

  armedSince = 0;

  resetSettleWindow();
  resetSettleRing();
  resetDetectors();

  Serial.println();
  Serial.println("REEL IN - DETECTION PAUSED");

  landOnReelIn();

  server.send(200, "text/plain", "OK");
}

void handleReady()
{
  if(rodState != ST_PAUSED)
  {
    server.send(200, "text/plain", "ALREADY ACTIVE");
    return;
  }

  startSettling(true);

  Serial.println("READY - WAITING FOR STABLE POSITION");

  server.send(200, "text/plain", "OK");
}

void handleRelearn()
{
  startEnvironmentLearning();

  server.send(200, "text/plain", "OK");
}

// NEW PULL DIRECTION (phone settings / watch): one pull test now
void handleTeachDir()
{
  if(rodState == ST_LEARNING)
  {
    teachDirAsked = true;             // right after learning
  }
  else
  {
    Serial.println("NEW PULL DIRECTION ASKED");
    pullTestWaveDeg = 0;
    startPullTest();
  }

  server.send(200, "text/plain", "OK");
}

void handleSkipPull()
{
  if(rodState == ST_PULLTEST)
  {
    Serial.println("PULL TEST SKIPPED");
    goMonitoring();
  }

  server.send(200, "text/plain", "OK");
}

void handleSea()
{
  String m = server.arg("m");

  if(m == "auto")
  {
    seaAuto = true;
    updateConditions(true);
  }
  else
  {
    for(int i = 0; i < 3; i++)
    {
      if(m == SEA_NAMES[i])
      {
        seaAuto = false;
        seaState = i;
        updateKnockThreshold();
      }
    }
  }

  server.send(200, "text/plain", "OK");
}

// ---------- Live settings over the web ----------

void sendSettings()
{
  String json = "{";

  for(int i = 0; i < SETTING_COUNT; i++)
  {
    if(i > 0) json += ",";

    json += "\"";
    json += SETTING_INFO[i].key;
    json += "\":";
    json += String(settings.*(SETTING_INFO[i].field), 3);
  }

  json += ",\"saved\":";
  json += settingsSaved() ? "true" : "false";
  json += "}";

  server.send(200, "application/json", json);
}

void handleSettings()
{
  sendSettings();
}

// /set?k=pdeg&v=1.5  - takes effect immediately
void handleSet()
{
  String k = server.arg("k");
  float v = server.arg("v").toFloat();

  for(int i = 0; i < SETTING_COUNT; i++)
  {
    const SettingInfo& info = SETTING_INFO[i];

    if(k == info.key)
      settings.*(info.field) = clampFloat(v, info.low, info.high);
  }

  updateKnockThreshold();

  sendSettings();
}

void handleSaveSettings()
{
  saveSettings();
  sendSettings();
}

// ---------- Time and date (from the phone or the watch) ----------

bool clockSet = false;
unsigned long clockEpochAt = 0;          // seconds since 1970 (UTC)...
unsigned long clockMillisAt = 0;         // ...at this millis()
long clockTzMin = 0;                     // local time = UTC + this

unsigned long nowEpoch()
{
  if(!clockSet)
    return 0;

  return clockEpochAt + (millis() - clockMillisAt) / 1000;
}

void handleSetTime()
{
  long t = server.arg("t").toInt();
  long tz = server.arg("tz").toInt();

  if(t < 1600000000L || tz < -14 * 60 || tz > 14 * 60)
  {
    server.send(400, "text/plain", "BAD TIME");
    return;
  }

  bool first = !clockSet;

  clockEpochAt = (unsigned long)t;
  clockMillisAt = millis();
  clockTzMin = tz;
  clockSet = true;

  if(first)
    Serial.printf("TIME SET: %lu (UTC%+ld min)\n", (unsigned long)t, tz);

  server.send(200, "text/plain", "OK");
}

// ---------- Saved setups (5, named, in flash) ----------

const int PRESET_COUNT = 5;
const int PRESET_NAME_MAX = 24;          // bytes

String presetKey(int slot, const char* key)
{
  return String("s") + String(slot) + key;   // e.g. "s0pdeg" (flash keys: max 15 letters)
}

// A name safe to put in JSON: no quotes, backslashes or control
// characters; letters like n with tilde (UTF-8) are kept; cut whole.
String cleanPresetName(const String& in)
{
  String out;

  for(unsigned i = 0; i < in.length(); i++)
  {
    unsigned char c = (unsigned char)in[i];

    if(c == '"' || c == '\\' || c < 0x20 || c == 0x7F)
      continue;

    out += (char)c;
  }

  out.trim();

  while(out.length() > PRESET_NAME_MAX)
  {
    int cut = PRESET_NAME_MAX;

    while(cut > 0 && (((unsigned char)out[cut]) & 0xC0) == 0x80)
      cut--;                             // not in the middle of a letter

    out = out.substring(0, cut);
  }

  return out;
}

void sendPresets()
{
  String json = "{\"presets\":[";

  prefs.begin("presets", true);

  for(int p = 0; p < PRESET_COUNT; p++)
  {
    if(p > 0) json += ",";

    json += "\"";
    json += prefs.getString(presetKey(p, "name").c_str(), "");
    json += "\"";
  }

  prefs.end();

  json += "]}";

  server.send(200, "application/json", json);
}

int presetSlot()
{
  int p = server.arg("s").toInt();
  return p >= 0 && p < PRESET_COUNT ? p : -1;
}

void handlePresets()
{
  sendPresets();
}

void handlePresetSave()
{
  int p = presetSlot();

  if(p < 0)
  {
    server.send(400, "text/plain", "BAD SETUP");
    return;
  }

  String name = cleanPresetName(server.arg("n"));

  if(name.length() == 0)
    name = String("Setup ") + String(p + 1);

  prefs.begin("presets", false);

  for(int i = 0; i < SETTING_COUNT; i++)
    prefs.putFloat(presetKey(p, SETTING_INFO[i].key).c_str(), settings.*(SETTING_INFO[i].field));

  prefs.putString(presetKey(p, "name").c_str(), name);
  prefs.end();

  Serial.printf("SETUP %d SAVED: %s\n", p + 1, name.c_str());

  sendPresets();
}

void handlePresetLoad()
{
  int p = presetSlot();

  prefs.begin("presets", true);
  bool used = p >= 0 && prefs.getString(presetKey(p, "name").c_str(), "").length() > 0;

  if(used)
  {
    for(int i = 0; i < SETTING_COUNT; i++)
    {
      const SettingInfo& info = SETTING_INFO[i];

      settings.*(info.field) =
        clampFloat(
          prefs.getFloat(presetKey(p, info.key).c_str(), DEFAULT_SETTINGS.*(info.field)),
          info.low,
          info.high
        );
    }
  }

  prefs.end();

  if(!used)
  {
    server.send(404, "text/plain", "EMPTY SETUP");
    return;
  }

  updateKnockThreshold();

  sendSettings();
}

void handlePresetDelete()
{
  int p = presetSlot();

  if(p >= 0)
  {
    prefs.begin("presets", false);

    for(int i = 0; i < SETTING_COUNT; i++)
      prefs.remove(presetKey(p, SETTING_INFO[i].key).c_str());

    prefs.remove(presetKey(p, "name").c_str());
    prefs.end();
  }

  sendPresets();
}

void handleDefaultSettings()
{
  settings = DEFAULT_SETTINGS;

  updateKnockThreshold();

  sendSettings();
}

const char* stateName()
{
  switch(rodState)
  {
    case ST_LEARNING: return "learning";
    case ST_PULLTEST: return "pulltest";
    case ST_SETTLING: return "settling";
    case ST_PAUSED: return "paused";
    default: return "monitoring";
  }
}

void addField(String& json, const char* name, float value, int decimals)
{
  json += ",\"";
  json += name;
  json += "\":";
  json += String(value, decimals);
}

void addField(String& json, const char* name, long value)
{
  json += ",\"";
  json += name;
  json += "\":";
  json += String(value);
}

void addField(String& json, const char* name, bool value)
{
  json += ",\"";
  json += name;
  json += "\":";
  json += value ? "true" : "false";
}

// Only what the watch shows (much smaller than /data). Also the live
// message the rod sends to everyone 10 times a second.
String watchJson()
{
  String json;
  json.reserve(320);

  json += "{\"state\":\"";
  json += stateName();
  json += "\",\"type\":\"";
  json += strikeType;
  json += "\"";

  addField(json, "strike", strikeActive);
  addField(json, "strikeid", (long)strikeEventID);
  addField(json, "knockid", (long)knockAlertID);
  addField(json, "energy", energyPeakSinceWatch, 4);
  energyPeakSinceWatch = 0;
  addField(json, "knockthr", knockThreshold, 4);
  addField(json, "tilt", tiltDeg, 2);
  addField(json, "ft", fastTiltDeg, 2);
  addField(json, "time", (long)nowEpoch());
  addField(json, "tz", clockTzMin);
  addField(json, "allow", pullAllow, 2);
  addField(json, "bucket", (long)(pullBucket / bucketSize * 100.0));
  addField(json, "drop", (long)(dropBucket / bucketSize * 100.0));
  addField(json, "knocks", (long)recentKnockCount());
  addField(json, "dir", haveDir);
  addField(json, "dropwatch", dropWatch);
  addField(json, "ptdeg", totalDeg, 2);
  addField(json, "ptneed", pullTestNeed(), 2);
  addField(json, "wave", (long)waveLevel);
  addField(json, "wind", (long)windLevel);
  addField(json, "bat", (long)batPercent);
  addField(json, "batleft", batLeftMin);
  json += ",\"lmode\":\"";
  json += linkModeName();
  json += "\"";
  addWatchChanMode(json);
  addLand(json);

  json += "}";

  return json;
}

// ---------- Private Bluetooth: rod ID, PIN, paired devices ----------

int bondedDevices()
{
#if defined(CONFIG_BLUEDROID_ENABLED)
  return esp_ble_get_bond_device_num();
#elif defined(CONFIG_NIMBLE_ENABLED)
  int n = 0;
  ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
  return n;
#else
  return 0;
#endif
}

// pairing is open while nothing is paired yet, or just after switching on
bool pairWindowOpen()
{
  return bondedDevices() == 0 || millis() < PAIR_WINDOW_MS;
}

// ID from the chip (Bluetooth address), PIN made once at random
void privacyBegin()
{
  uint8_t mac[6] = { 0 };
  esp_read_mac(mac, ESP_MAC_BT);

  char id[12];
  snprintf(id, sizeof(id), "ROD-%02X%02X", mac[4], mac[5]);
  rodId = id;

  Preferences p;
  p.begin("strikesense", false);
  rodPin = p.getULong("pin", 0);

  if(rodPin < 100000 || rodPin > 999999)
  {
    rodPin = 100000 + esp_random() % 900000;
    p.putULong("pin", rodPin);
  }

  p.end();

  Serial.println();
  Serial.printf("==== %s   PIN %06lu   (write both on the rod box) ====\n", rodId.c_str(), (unsigned long)rodPin);
}

// "I am the phone" / "I am the watch": the rod keeps one of each
void handleIam()
{
#if defined(ESP32)
  String d = server.arg("d");
  String addr = server.askerAddr();

  if(addr.length() == 0 || (!(d == "phone") && !(d == "watch")))
  {
    server.send(200, "text/plain", "OK");     // (address not known: as before)
    return;
  }

  const char* key = d == "phone" ? "phoneAddr" : "watchAddr";
  Preferences p;
  p.begin("strikesense", false);
  String mine = p.getString(key, "");

  if(mine.length() == 0)
  {
    p.putString(key, addr);
    p.end();
    Serial.printf("Bluetooth: this rod's %s is %s\n", d.c_str(), addr.c_str());
    server.send(200, "text/plain", "OK");
    return;
  }

  p.end();

  if(mine == addr)
  {
    server.send(200, "text/plain", "OK");
    return;
  }

  Serial.printf("Bluetooth: a second %s (%s) refused - this rod already has one (%s)\n", d.c_str(), addr.c_str(), mine.c_str());
  server.send(403, "text/plain", "TAKEN");
  server.dropAsker();
#else
  server.send(200, "text/plain", "OK");
#endif
}

void forgetPairedDevices()
{
  Preferences fp;
  fp.begin("strikesense", false);
  fp.remove("phoneAddr");
  fp.remove("watchAddr");
  fp.end();

#if defined(CONFIG_BLUEDROID_ENABLED)
  int n = esp_ble_get_bond_device_num();

  if(n > 0)
  {
    esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * n);

    if(list && esp_ble_get_bond_device_list(&n, list) == ESP_OK)
    {
      for(int i = 0; i < n; i++)
        esp_ble_remove_bond_device(list[i].bd_addr);
    }

    free(list);
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  ble_store_clear();
#endif

  Serial.println("Bluetooth: all paired devices forgotten - pairing open");
}

void addWatchChanMode(String&) {}      // no LoRa on this rod

// ---------- Watch link: auto / Bluetooth only / LoRa only (phone app) ----------

int linkMode = 0;                       // 0 auto, 1 Bluetooth only, 2 LoRa only

const char* linkModeName()
{
  return linkMode == 1 ? "bt" : linkMode == 2 ? "lora" : "auto";
}

void loadLinkMode()
{
  prefs.begin("strikesense", true);
  linkMode = prefs.getInt("lmode", 0);
  prefs.end();

  if(linkMode < 0 || linkMode > 2)
    linkMode = 0;
}

// /linkmode?m=auto|bt|lora
void handleLinkMode()
{
  String m = server.arg("m");
  int v = m == "auto" ? 0 : m == "bt" ? 1 : m == "lora" ? 2 : -1;

  if(v < 0)
  {
    server.send(400, "text/plain", "BAD MODE");
    return;
  }

  setLinkMode(v);
  server.send(200, "application/json", String("{\"lmode\":\"") + linkModeName() + "\"}");
}

void setLinkMode(int v)
{
  if(v < 0 || v > 2)
    return;

  linkMode = v;

  prefs.begin("strikesense", false);
  prefs.putInt("lmode", v);
  prefs.end();

  Serial.printf("WATCH LINK: %s\n", linkModeName());
}

void addLinkMode(String& json)
{
  bool hasLora = false;

#if defined(ENABLE_LORA) && ENABLE_LORA
  hasLora = loraRadioOk();
#endif

  json += ",\"lmode\":\"";
  json += linkModeName();
  json += "\"";
  addField(json, "haslora", hasLora);
}

// ---------- The watch's link, for the phone ----------

unsigned long watchBtAt = 0;            // last "/wping" or "/watch" from the watch over Bluetooth
int watchBtRssi = 0;                    // how strong the watch hears the rod (dBm, 0 = unknown)
int watchBtConn = -1;                   // the watch's Bluetooth connection (the rod can measure it too)

// what the watch said about the signal (only when it says something)
void noteWatchHere()
{
  watchBtAt = millis();
#if defined(ESP32)
  watchBtConn = server.askerConn();

  int r = server.hasArg("r") ? server.arg("r").toInt() : 0;

  if(r != 0)
    watchBtRssi = r;                    // (0 = the watch did not know: the last real one stays)

  static unsigned long saidAt = 0;

  if(millis() - saidAt > 30000)
  {
    saidAt = millis();
    Serial.printf("Bluetooth: watch signal %d dBm (watch says), %d dBm (rod measures)\n", watchBtRssi, server.connRssi(watchBtConn));
  }
#else
  watchBtRssi = server.arg("r").toInt();
#endif
}

// the watch is on Bluetooth: its ping in the last 6 s, or its link still up
// (pinged in the last minute) - a busy link must not show "not connected"
bool watchOnBluetooth()
{
  if(watchBtAt != 0 && millis() - watchBtAt < 6000)     // it pings every 2 s
    return true;

#if defined(ESP32)
  if(watchBtAt != 0 && millis() - watchBtAt < 60000 && server.connUp(watchBtConn))
    return true;                        // its link is up: a late ping is not "gone"
#endif
  return false;
}

// the watch's signal: what it said, else what the rod measures
int watchSignal()
{
#if defined(ESP32)
  if(watchBtRssi == 0)
    return server.connRssi(watchBtConn);
#endif
  return watchBtRssi;
}

int rssiBars(int rssi)
{
  if(rssi == 0) return 0;
  if(rssi >= -60) return 4;
  if(rssi >= -70) return 3;
  if(rssi >= -80) return 2;

  return 1;
}

// "wlink": bt / lora / none. Bluetooth wins when both work (the watch uses it).
void addWatchLink(String& json)
{
  bool bt = watchOnBluetooth();
  bool lora = false;
  int loraBarsNow = 0;

#if defined(ENABLE_LORA) && ENABLE_LORA
  lora = watchConnected();
  loraBarsNow = lora ? loraWatchBars() : 0;
#endif

  json += ",\"wlink\":\"";
  json += bt ? "bt" : lora ? "lora" : "none";
  json += "\"";

  addField(json, "wbars", (long)(bt ? rssiBars(watchSignal()) : loraBarsNow));
  addField(json, "wlora", lora);
}

// ---------- The last strikes, for the phone ----------
//
// A fight lasts from the strike until the rod is quiet (tilt back inside the
// pull allowance, no knock) for 10 s, at most 10 minutes or until reeled in.

const int SLIST_N = 10;
const unsigned long FIGHT_QUIET_MS = 10000;
const unsigned long FIGHT_MAX_MS = 600000;

// the strike log (flash), see STRIKE LOG further down
bool logReady = false;
uint32_t logSeq = 0;                    // the newest strike number given out (never repeats)
void logSaveOldest();

struct StrikeSummary
{
  unsigned long id;
  uint32_t seq;                         // number in the strike log (0 = no log)
  bool stored;                          // written to the log
  char type[20];
  unsigned long startMs;
  unsigned long lastActiveMs;
  float peakTilt;                       // strongest pull, degrees
  float peakKnock;                      // biggest knock, x the knock limit
  int knocks;
  bool done;
  int16_t land;                         // REEL IN -> FISH OUT / LOST IT, seconds
  uint8_t result;                       // 0 none, 1 fish out, 2 lost it, 3 no fish
};

StrikeSummary slist[SLIST_N];
int slistCount = 0;                     // newest is slist[slistCount - 1]
bool fightWasKnock = false;

void fightOnStrike()
{
  if(slistCount > 0 && !slist[slistCount - 1].done)
    slist[slistCount - 1].done = true;      // a new strike ends the old fight

  if(slistCount == SLIST_N)
  {
    logSaveOldest();                    // many strikes in a row: save it now, before it drops out

    for(int i = 1; i < SLIST_N; i++)
      slist[i - 1] = slist[i];

    slistCount--;
  }

  StrikeSummary& s = slist[slistCount++];
  memset(&s, 0, sizeof(s));
  s.id = strikeEventID;
  s.seq = logReady ? ++logSeq : 0;
  strncpy(s.type, strikeType.c_str(), sizeof(s.type) - 1);
  s.startMs = millis();
  s.lastActiveMs = s.startMs;
  s.peakTilt = fabs(tiltDeg);
}

// Every reading
void updateFight()
{
  landTick();

  if(slistCount == 0)
    return;

  StrikeSummary& s = slist[slistCount - 1];

  if(s.done)
    return;

  unsigned long now = millis();
  float thr = knockThreshold > 0 ? knockThreshold : 1;
  bool knock = knockEnergy > thr;
  bool pulled = fabs(tiltDeg) > pullAllow;

  if(knock && !fightWasKnock)
    s.knocks++;

  fightWasKnock = knock;

  if(knock || pulled)                 // an alarm nobody answered is not a fight
    s.lastActiveMs = now;

  if(fabs(tiltDeg) > s.peakTilt)
    s.peakTilt = fabs(tiltDeg);

  if(knockEnergy / thr > s.peakKnock)
    s.peakKnock = knockEnergy / thr;

  if(now - s.lastActiveMs > FIGHT_QUIET_MS || now - s.startMs > FIGHT_MAX_MS || rodState == ST_PAUSED)
    s.done = true;
}

// From a paired phone: forget every paired phone and watch (new phone, lost watch).
// Pairing is open again until the next device pairs.
void handleForget()
{
  server.send(200, "text/plain", "OK");
  forgetPairedDevices();
}

// The watch: "I'm here over Bluetooth, hearing you this strong" (every 2 s)
void handleWatchPing()
{
  noteWatchHere();
  server.send(200, "text/plain", "OK");
}

void handleStrikes()
{
  String json;
  json.reserve(120 + SLIST_N * 120);

  json += "{\"time\":";
  json += String((unsigned long)nowEpoch());
  json += ",\"list\":[";

  unsigned long now = millis();

  for(int i = 0; i < slistCount; i++)
  {
    StrikeSummary& s = slist[i];
    unsigned long end = s.done ? s.lastActiveMs : now;

    if(i > 0) json += ",";

    json += "{\"id\":";
    json += String(s.id);
    json += ",\"seq\":";
    json += String((unsigned long)s.seq);
    json += ",\"type\":\"";
    json += s.type;
    json += "\"";
    addField(json, "ago", (long)((now - s.startMs) / 1000));
    addField(json, "fight", (long)((end - s.startMs) / 1000));
    addField(json, "peak", s.peakTilt, 2);
    addField(json, "knock", s.peakKnock, 2);
    addField(json, "knocks", (long)s.knocks);
    addField(json, "done", s.done);
    addField(json, "land", (long)s.land);
    json += ",\"result\":\"";
    json += s.result == 1 ? "fish" : s.result == 2 ? "lost" : s.result == 3 ? "nofish" : "";
    json += "\"}";
  }

  json += "]}";

  server.send(200, "application/json", json);
}

// =====================================================
// STRIKE LOG (flash): every strike with its graph, also when no phone is
// near, and after the rod was switched off. The newest ~200 are kept (a
// ring: the oldest is replaced). The app collects what it has not got yet
// (/log, /logstrike) and remembers how far it got: nothing on the rod is
// marked or deleted. Each strike is written once, when its fight is over
// and the rod is calm (writing flash pauses the rod for a moment).
// =====================================================

struct LogRec
{
  uint32_t magic;                       // LOG_MAGIC: a real record
  uint32_t seq;
  uint32_t utc;                         // start, seconds since 1970 (0 = the rod did not know the time)
  uint32_t boot;                        // which power-on
  uint32_t startMs;                     // millis() at the start (same power-on: "ago")
  char type[20];
  float peakTilt;                       // strongest pull, degrees
  float peakKnock;                      // biggest knock, x the knock limit
  int16_t knocks;
  int16_t fight;                        // seconds
  int16_t count;                        // graph points, 200 ms each (0 = no graph)
  int16_t pre;
  int16_t zpre;                         // zoom: 50 a second around the strike
  int16_t zn;
  int16_t k[WREC_N];
  int16_t t[WREC_N];
  int16_t zk[ZOOM_EACH * 2];
  int16_t zt[ZOOM_EACH * 2];
  // rod 3.1: the landing (older records end before this: read as 0)
  int16_t land;                         // REEL IN -> FISH OUT / LOST IT, seconds
  int16_t result;                       // 0 none, 1 fish out, 2 lost it, 3 no fish
};

const size_t LOG_OLD_SIZE = offsetof(LogRec, land);

const uint32_t LOG_MAGIC = 0x53534C31;  // "SSL1"
const int LOG_MAX = 300;
int logSlots = 0;
uint32_t logBoot = 0;
uint32_t logSeqSaved = 0;
unsigned long logLastWrite = 0;

// (in a struct: the Arduino IDE would put plain functions with these
// types at the top of the file, before the types are known)
struct StrikeLog
{
  static String path(uint32_t seq)
  {
    return "/sl/" + String((unsigned long)(seq % logSlots)) + ".bin";
  }

  static bool read(uint32_t seq, LogRec& r)
  {
    File f = LittleFS.open(path(seq).c_str(), "r");

    if(!f)
      return false;

    memset(&r, 0, sizeof(r));
    size_t n = f.read((uint8_t*)&r, sizeof(r));
    bool ok = (n == sizeof(r) || n == LOG_OLD_SIZE) && r.magic == LOG_MAGIC && r.seq == seq;
    f.close();
    return ok;
  }

  // a strike not written yet (its fight goes on, or the rod was busy)
  static bool pending(uint32_t seq)
  {
    for(int i = 0; i < slistCount; i++)
      if(slist[i].seq == seq && !slist[i].stored)
        return true;

    return false;
  }

  static void write(StrikeSummary& s)
  {
    static LogRec r;                    // (1.5 KB: not on the stack)
    memset(&r, 0, sizeof(r));

    unsigned long ep = nowEpoch();
    r.magic = LOG_MAGIC;
    r.seq = s.seq;
    r.utc = ep ? ep - (millis() - s.startMs) / 1000 : 0;
    r.boot = logBoot;
    r.startMs = s.startMs;
    strncpy(r.type, s.type, sizeof(r.type) - 1);
    r.peakTilt = s.peakTilt;
    r.peakKnock = s.peakKnock;
    r.knocks = s.knocks;
    r.fight = (int16_t)((s.lastActiveMs - s.startMs) / 1000);
    r.land = s.land;
    r.result = s.result;

    // its graph, if the rod still has it (the last 5)
    for(int i = 0; i < wsKeepN; i++)
    {
      WsRec& w = wsKeep[i];

      if(w.id != s.id)
        continue;

      r.count = w.count;
      r.pre = w.pre;
      memcpy(r.k, w.k, sizeof(r.k));
      memcpy(r.t, w.t, sizeof(r.t));
      r.zpre = w.zpre;
      r.zn = w.zn;
      memcpy(r.zk, w.zk, sizeof(r.zk));
      memcpy(r.zt, w.zt, sizeof(r.zt));
    }

    File f = LittleFS.open(path(s.seq).c_str(), "w");

    if(f)
    {
      f.write((uint8_t*)&r, sizeof(r));
      f.close();
    }

    if(s.seq > logSeqSaved)
    {
      Preferences p;
      p.begin("strikesense", false);
      p.putULong("lseq", s.seq);
      p.end();
      logSeqSaved = s.seq;
    }

    Serial.printf("STRIKE LOG: strike %lu saved (%s%s)\n", (unsigned long)s.seq, r.type, r.count > 1 ? ", with graph" : "");
  }

  // FISH OUT after the strike was saved: add the landing to its record
  static void setLand(uint32_t seq, int16_t land, int16_t result)
  {
    static LogRec r;

    if(!read(seq, r))
      return;

    r.land = land;
    r.result = result;

    File f = LittleFS.open(path(seq).c_str(), "w");

    if(f)
    {
      f.write((uint8_t*)&r, sizeof(r));
      f.close();
    }
  }
};

// =====================================================
// LANDING: REEL IN within 5 minutes of a real strike starts the fight
// timer; FISH OUT, LOST IT or NO FISH (watch or app) stops it. Kept with
// the strike. REEL IN with no strike before (new bait): no timer.
// =====================================================

const unsigned long LAND_AFTER_STRIKE_MS = 300000;   // REEL IN up to 5 min after the strike

int landState = 0;                      // 0 none, 1 fighting, 2 fish out, 3 lost it, 4 no fish
unsigned long landStartMs = 0;
unsigned long landEndMs = 0;
unsigned long landStrikeId = 0;

long landSeconds()
{
  if(landState == 0)
    return 0;

  unsigned long end = landState == 1 ? millis() : landEndMs;
  long sec = (long)((end - landStartMs) / 1000);
  return sec > 32000 ? 32000 : sec;
}

void landOnReelIn()
{
  if(landState == 1 || slistCount == 0)
    return;

  StrikeSummary& s = slist[slistCount - 1];

  if(millis() - s.startMs > LAND_AFTER_STRIKE_MS || s.result != 0)
  {
    Serial.println("REEL IN with no strike in the last 5 min: no fight timer (new bait)");
    return;
  }

  landState = 1;
  landStartMs = millis();
  landEndMs = 0;
  landStrikeId = s.id;
  landNews();
  Serial.println("FIGHT: timer started (REEL IN after a strike)");
}

// Every reading: casting again (READY / resume / learning) ends the landing
void landTick()
{
  if(landState != 0 && rodState != ST_PAUSED)
  {
    landState = 0;
    landNews();
  }
}

// how the fight ended: 1 FISH OUT, 2 LOST IT, 3 NO FISH
bool cmdLand(int result)
{
  if(landState != 1 || result < 1 || result > 3)
    return false;

  landState = result + 1;
  landEndMs = millis();

  int16_t sec = (int16_t)landSeconds();
  landNews();

  for(int i = 0; i < slistCount; i++)
  {
    StrikeSummary& st = slist[i];

    if(st.id != landStrikeId)
      continue;

    st.land = sec;
    st.result = (uint8_t)result;

    if(st.stored && logReady)
      StrikeLog::setLand(st.seq, st.land, st.result);   // else written with it later
  }

  Serial.printf("FIGHT: %s after %d:%02d\n", result == 1 ? "FISH OUT" : result == 2 ? "LOST IT" : "NO FISH", sec / 60, sec % 60);
  return true;
}

void handleFishOut()
{
  server.send(200, "text/plain", cmdLand(1) ? "OK" : "NO FIGHT");
}

void handleLostFish()
{
  server.send(200, "text/plain", cmdLand(2) ? "OK" : "NO FIGHT");
}

void handleNoFish()
{
  server.send(200, "text/plain", cmdLand(3) ? "OK" : "NO FIGHT");
}

void addLand(String& json)
{
  addField(json, "land", (long)landState);
  addField(json, "landsec", landSeconds());
}

void logBegin()
{
  if(!LittleFS.begin(true))             // the first time: prepares the storage
  {
    Serial.println("STRIKE LOG: no flash storage (Tools > Partition Scheme) - strikes kept in memory only");
    return;
  }

  LittleFS.mkdir("/sl");
  logSlots = (int)(LittleFS.totalBytes() * 7 / 10 / 4096);   // a strike takes one 4 KB block

  if(logSlots > LOG_MAX)
    logSlots = LOG_MAX;

  if(logSlots < 10)
  {
    Serial.println("STRIKE LOG: flash storage too small - strikes kept in memory only");
    return;
  }

  Preferences p;
  p.begin("strikesense", false);
  logSeq = p.getULong("lseq", 0);
  logBoot = p.getULong("lboot", 0) + 1;
  p.putULong("lboot", logBoot);
  p.end();

  logSeqSaved = logSeq;
  logReady = true;
  Serial.printf("STRIKE LOG: keeps the last %d strikes, newest number %lu\n", logSlots, (unsigned long)logSeq);
}

// From loop(): the oldest finished strike not saved yet, one a second, never
// while a strike is being recorded
void logTick()
{
  if(!logReady || wsRecording || millis() - logLastWrite < 1000)
    return;

  for(int i = 0; i < slistCount; i++)
  {
    StrikeSummary& s = slist[i];

    if(s.done && !s.stored && s.seq != 0)
    {
      StrikeLog::write(s);
      s.stored = true;
      logLastWrite = millis();
      return;
    }
  }
}

void logSaveOldest()
{
  StrikeSummary& s = slist[0];

  if(logReady && slistCount > 0 && !s.stored && s.seq != 0)
  {
    s.done = true;
    StrikeLog::write(s);
    s.stored = true;
  }
}

// /log?after=12: the saved strikes after number 12 (up to 8 at a time)
void handleLog()
{
  uint32_t after = (uint32_t)server.arg("after").toInt();

  String json;
  json.reserve(1600);

  json += "{\"boot\":";
  json += String((unsigned long)logBoot);
  addField(json, "last", (long)logSeq);
  addField(json, "time", (long)nowEpoch());
  json += ",\"list\":[";

  int n = 0;

  if(logReady)
  {
    static LogRec r;
    uint32_t from = after + 1;

    if(logSeq >= (uint32_t)logSlots && from < logSeq - logSlots + 1)
      from = logSeq - logSlots + 1;     // older ones were replaced

    for(uint32_t q = from; q <= logSeq && n < 8; q++)
    {
      if(!StrikeLog::read(q, r))
      {
        if(StrikeLog::pending(q))
          break;                        // not saved yet: next time
        continue;                       // lost (the rod was switched off before it was saved)
      }

      if(n++ > 0) json += ",";

      json += "{\"seq\":";
      json += String((unsigned long)r.seq);
      json += ",\"type\":\"";
      json += r.type;
      json += "\"";
      addField(json, "utc", (long)r.utc);
      addField(json, "ago", r.boot == logBoot ? (long)((millis() - r.startMs) / 1000) : -1L);
      addField(json, "fight", (long)r.fight);
      addField(json, "peak", r.peakTilt, 2);
      addField(json, "knock", r.peakKnock, 2);
      addField(json, "knocks", (long)r.knocks);
      addField(json, "graph", r.count > 1);
      addField(json, "land", (long)r.land);
      json += ",\"result\":\"";
      json += r.result == 1 ? "fish" : r.result == 2 ? "lost" : r.result == 3 ? "nofish" : "";
      json += "\"}";
    }
  }

  json += "]}";

  server.send(200, "application/json", json);
}

// /logstrike?seq=12: that strike's graph (like /laststrike)
void handleLogStrike()
{
  static LogRec r;
  uint32_t seq = (uint32_t)server.arg("seq").toInt();

  if(!logReady || seq == 0 || !StrikeLog::read(seq, r))
  {
    server.send(200, "application/json", "{\"have\":false}");
    return;
  }

  String json;
  json.reserve(WREC_N * 10 + ZOOM_EACH * 20 + 300);

  json += "{\"type\":\"";
  json += r.type;
  json += "\"";
  addField(json, "have", r.count > 1);
  addField(json, "recording", false);
  addField(json, "seq", (long)r.seq);
  addField(json, "pre", (long)r.pre);

  json += ",\"k\":[";

  for(int i = 0; i < r.count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.k[i]);
  }

  json += "],\"t\":[";

  for(int i = 0; i < r.count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.t[i]);
  }

  json += "],\"zpre\":";
  json += String((int)r.zpre);
  json += ",\"zk\":[";

  for(int i = 0; i < r.zn; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.zk[i]);
  }

  json += "],\"zt\":[";

  for(int i = 0; i < r.zn; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.zt[i]);
  }

  json += "]}";

  server.send(200, "application/json", json);
}

void handleWatch()
{
  noteWatchHere();
  server.send(200, "application/json", watchJson());
}

// The watch's LAST STRIKE page and the phone app's strike graphs.
// /laststrike: the newest strike; while it is being recorded (the 10 s
// after it) what is there so far, so the watch shows it at once.
// /laststrike?n=12: strike number 12 if the rod still has it (the last 5).
// "zk"/"zt": 2 s before + 2 s after at 50 a second.
void handleLastStrike()
{
  String want = server.arg("n");
  long n = want.length() > 0 ? want.toInt() : -1;

  bool live = wsRecording && (n < 0 || n == (long)wsNextId);
  WsRec* r = nullptr;

  if(!live)
  {
    for(int i = wsKeepN - 1; i >= 0 && r == nullptr; i--)
      if(n < 0 || wsKeep[i].id == (uint32_t)n)
        r = &wsKeep[i];
  }

  bool have = live || r != nullptr;
  int count = live ? wrecCount : (r != nullptr ? r->count : 0);
  int start = (wrecHead - wrecCount + WREC_N) % WREC_N;

  String json;
  json.reserve(WREC_N * 10 + ZOOM_EACH * 20 + 300);

  json += "{\"type\":\"";
  json += live ? wsNextType.c_str() : (r != nullptr ? r->type : "");
  json += "\"";

  addField(json, "have", have);
  addField(json, "recording", live);
  addField(json, "id", (long)(live ? wsNextId : (r != nullptr ? r->id : 0)));
  addField(json, "ago", have ? (long)((millis() - (live ? wsNextAt : r->at)) / 1000) : 0L);
  addField(json, "count", (long)strikeEventID);
  addField(json, "tiltpeak", live ? wsNextTiltPeak : (r != nullptr ? r->tiltPeak : 0), 2);
  addField(json, "pull", (long)(live ? wsNextPull : (r != nullptr ? r->pull : 0)));
  addField(json, "pre", (long)(live ? count - wsPost : (r != nullptr ? r->pre : 0)));

  json += ",\"k\":[";

  for(int i = 0; i < count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)(live ? wrecK[(start + i) % WREC_N] : r->k[i]));
  }

  json += "],\"t\":[";

  for(int i = 0; i < count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)(live ? wrecT[(start + i) % WREC_N] : r->t[i]));
  }

  // zoom: while recording from the dashboard's recording, else the kept copy
  int zs = 0, zn = 0, zpre = 0;

  if(live)
  {
    zs = savedPre - ZOOM_EACH;
    int ze = savedPre + ZOOM_EACH;

    if(zs < 0) zs = 0;
    if(ze > savedCount) ze = savedCount;

    zn = ze > zs ? ze - zs : 0;
    zpre = savedPre - zs;
  }
  else if(r != nullptr)
  {
    zn = r->zn;
    zpre = r->zpre;
  }

  float thr = savedThreshold > 0 ? savedThreshold : 1;

  json += "],\"zpre\":";
  json += String(zpre);
  json += ",\"zk\":[";

  for(int i = 0; i < zn; i++)
  {
    float k = live ? savedEnergy[zs + i] / thr * 100 : r->zk[i];
    if(i > 0) json += ",";
    json += String((int)(k > 300 ? 300 : k));
  }

  json += "],\"zt\":[";

  for(int i = 0; i < zn; i++)
  {
    float t = live ? savedTilt[zs + i] * 100 : r->zt[i];
    if(i > 0) json += ",";
    json += String((int)(t > 300 ? 300 : (t < -300 ? -300 : t)));
  }

  json += "]}";

  server.send(200, "application/json", json);
}

void handleData()
{
  unsigned long timeLeft = 0;

  if(rodState == ST_LEARNING)
  {
    unsigned long elapsed = millis() - learningStart;

    if(elapsed < LEARN_TIME)
      timeLeft = (LEARN_TIME - elapsed + 999) / 1000;
  }

  long dirAge = -1;

  if(dirLearnedAt != 0)
    dirAge = (millis() - dirLearnedAt) / 1000;

  String json;
  json.reserve(900);

  json += "{\"state\":\"";
  json += stateName();
  json += "\"";

  addField(json, "strike", strikeActive);

  json += ",\"type\":\"";
  json += strikeType;
  json += "\",\"lasttype\":\"";
  json += lastStrikeType;
  json += "\"";

  addField(json, "strikeid", (long)strikeEventID);
  addField(json, "strikeready", lastStrikeReady);
  addLand(json);
  json += ",\"rst\":\"";
  json += resetReason();
  json += "\"";
  addField(json, "knockid", (long)knockAlertID);

  // Highest knock energy since the last poll, so short knocks are never missed
  addField(json, "energy", energyPeakSincePoll, 4);
  energyPeakSincePoll = 0;

  addField(json, "knockthr", knockThreshold, 4);
  addField(json, "tilt", tiltDeg, 2);
  addField(json, "side", sideDeg, 2);
  addField(json, "allow", pullAllow, 2);
  addField(json, "bucket", (long)(pullBucket / bucketSize * 100.0));
  addField(json, "drop", (long)(dropBucket / bucketSize * 100.0));
  addField(json, "knocks", (long)recentKnockCount());
  addField(json, "dir", haveDir);
  addField(json, "dirage", dirAge);
  addField(json, "confidence", (long)settlingConfidence);
  addField(json, "timeleft", (long)timeLeft);
  addField(json, "grace", rodState == ST_SETTLING && millis() < settleGraceUntil);
  addField(json, "ptdeg", totalDeg, 2);
  addField(json, "ptneed", pullTestNeed(), 2);

  addField(json, "sea", (long)seaState);
  addField(json, "seaauto", seaAuto);
  addField(json, "wave", (long)waveLevel);
  addField(json, "waveswing", waveSwing, 2);
  addField(json, "period", wavePeriod, 1);
  addField(json, "wind", (long)windLevel);
  addField(json, "windswing", windSwing, 2);
  addField(json, "windvib", windVib, 4);
  addField(json, "noise", sensorFloor, 4);
  addField(json, "hr", sensorHighRes);

  json += ",\"sname\":\"";
  json += sensorName();
  json += "\"";

  addField(json, "serr", (long)sensorReadErrors);
  addField(json, "tide", (long)tideLevel);
  addField(json, "tiderate", tideRate, 2);

  addField(json, "up", (long)(millis() / 1000));
  addField(json, "armed", armedSince == 0 ? -1L : (long)((millis() - armedSince) / 1000));
  addField(json, "total", (long)totalSeconds());
  addField(json, "kbeep", settings.knockBeep > 0.5);

  addField(json, "peakenergy", strikePeakEnergy, 4);
  addField(json, "peaktilt", strikePeakTilt, 2);

  addField(json, "dropwatch", dropWatch);
  addField(json, "gustago", gustIgnoredAt == 0 ? -1L : (long)((millis() - gustIgnoredAt) / 1000));
  addField(json, "dropseen", dropSeenAt == 0 ? -1L : (long)((millis() - dropSeenAt) / 1000));
  addField(json, "dropid", (long)dropSeenID);

  addField(json, "bat", (long)batPercent);
  addField(json, "batv", batVolts, 2);

  addWatchLink(json);
  json += ",\"rodid\":\"";
  json += rodId;
  json += "\"";
  addField(json, "paired", (long)bondedDevices());
  addField(json, "pairopen", pairWindowOpen());
  addLinkMode(json);

  addVibrationSamples(json);

  json += "}";

  server.send(200, "application/json", json);
}

// Last strike replay: 4 s before + 4 s after
void handleStrike()
{
  String json;
  json.reserve(REC_TOTAL * 16 + 200);

  json += "{\"type\":\"";
  json += savedType;
  json += "\"";

  addField(json, "pre", (long)savedPre);
  addField(json, "thr", savedThreshold, 4);
  addField(json, "allow", savedAllow, 2);
  addField(json, "dir", haveDir);

  json += ",\"e\":[";

  for(int i = 0; i < savedCount; i++)
  {
    if(i > 0) json += ",";
    json += String(savedEnergy[i], 4);
  }

  json += "],\"t\":[";

  for(int i = 0; i < savedCount; i++)
  {
    if(i > 0) json += ",";
    json += String(savedTilt[i], 2);
  }

  json += "]}";

  server.send(200, "application/json", json);
}

// =====================================================
// SETUP
// =====================================================

void landNews()
{
#if defined(ESP32)
  server.pushLive();
#endif
#if defined(ENABLE_LORA) && ENABLE_LORA
  lastHeartbeatSent = 0;                // LoRa: the next heartbeat goes now
#endif
}

// A new sketch uploaded (not a restart): a fresh rod - settings back to
// default, paired phone and watch forgotten (pair again with the PIN), new
// LoRa key. Kept: the PIN, the strike log numbers, the total time and the
// sensor's noise calibration (that is the hardware, not a setting).
bool freshBonds = false;

// a fingerprint of the uploaded program (changes with every new upload)
String buildId()
{
#if defined(ESP32) && __has_include("esp_app_desc.h")
  char sha[17] = { 0 };
  esp_app_get_elf_sha256(sha, sizeof(sha));
  return String(sha);
#else
  return String(__DATE__ " " __TIME__);
#endif
}

void freshAfterUpload()
{
  String BUILD = buildId();

  Preferences p;
  p.begin("strikesense", false);

  if(p.getString("build", "") == BUILD)
  {
    p.end();
    return;
  }

  uint32_t pin = p.getULong("pin", 0);
  uint32_t lseq = p.getULong("lseq", 0);
  uint32_t lboot = p.getULong("lboot", 0);
  uint32_t total = p.getULong("total", 0);
  bool hasFloor = p.getBool("hasfloor", false), hasFloorA = p.getBool("hasfloorA", false);
  float floor0 = p.getFloat("floor", 0), floorA = p.getFloat("floorA", 0);

  p.clear();

  if(pin) p.putULong("pin", pin);
  if(lseq) p.putULong("lseq", lseq);
  if(lboot) p.putULong("lboot", lboot);
  if(total) p.putULong("total", total);
  if(hasFloor) { p.putBool("hasfloor", true); p.putFloat("floor", floor0); }
  if(hasFloorA) { p.putBool("hasfloorA", true); p.putFloat("floorA", floorA); }
  p.putString("build", BUILD);
  p.end();

  Preferences q;
  q.begin("presets", false);
  q.clear();
  q.end();

  freshBonds = true;
  Serial.println("NEW SKETCH UPLOADED: a fresh rod - settings back to default, phone and watch must pair again (PIN)");
}

void setup()
{
  Serial.begin(115200);

  delay(1000);

  freshAfterUpload();

  Serial.println();
  Serial.println("=== LAARIF STRIKESENSE ROD - version 3.4 (no LoRa; one phone + one watch) ===");

  Wire.begin();
  Wire.setClock(SENSOR_I2C_HZ);

  // ADXL355 first, then LIS3DH. Keep trying until one answers.
  while(!startSensor())
  {
    Serial.println("NO SENSOR FOUND (ADXL355 or LIS3DH)");
    Serial.println("  ADXL355: pin 1 -> D5, pin 2 -> D4, pin 5 -> GND, pin 6 -> 3V3,");
    Serial.println("           links on the sensor: pin 3 -> 12, pin 4 -> 11");
    Serial.println("  Trying again in 2 s...");

    delay(2000);
  }

  sensorFloor = SENSOR_FLOOR_DEFAULT;
  knockThreshold = KNOCK_FLOOR;

  loadStoredDirection();
  loadSensorFloor();
  loadSettings();
  loadTotalTime();

  if(haveSensorFloor)
  {
    Serial.print("Sensor noise loaded (g): ");
    Serial.println(sensorFloor, 4);
  }

  if(haveStoredDir)
    Serial.println("Stored pull direction loaded");

  Serial.println();
  Serial.println("Bluetooth: \"StrikeSense\" - open the StrikeSense app on the phone");

  server.on("/", handleRoot);
  server.on("/logo.webp", handleLogo);
  server.on("/data", handleData);
  server.on("/watch", handleWatch);
  server.on("/teachdir", handleTeachDir);
#if defined(ESP32)
  server.onLive(watchJson);
#endif
  server.on("/laststrike", handleLastStrike);
  server.on("/watching", handleWatching);
  server.on("/reelin", handleReelIn);
  server.on("/ready", handleReady);
  server.on("/iam", handleIam);
  server.on("/fishout", handleFishOut);
  server.on("/lostfish", handleLostFish);
  server.on("/nofish", handleNoFish);
  server.on("/relearn", handleRelearn);
  server.on("/skippull", handleSkipPull);
  server.on("/sea", handleSea);
  server.on("/strike", handleStrike);
  server.on("/strikes", handleStrikes);
  server.on("/log", handleLog);
  server.on("/logstrike", handleLogStrike);
  server.on("/wping", handleWatchPing);
  server.on("/forget", handleForget);
  server.on("/linkmode", handleLinkMode);
  server.on("/settings", handleSettings);
  server.on("/set", handleSet);
  server.on("/savesettings", handleSaveSettings);
  server.on("/defaultsettings", handleDefaultSettings);
  server.on("/settime", handleSetTime);
  server.on("/presets", handlePresets);
  server.on("/presetsave", handlePresetSave);
  server.on("/presetload", handlePresetLoad);
  server.on("/presetdel", handlePresetDelete);

  logBegin();
  server.begin();

  if(freshBonds)
    forgetPairedDevices();              // new sketch: phone and watch pair again

  startEnvironmentLearning();

  lastSampleTime = millis();

  Serial.printf("Started after: %s\n", resetReason());
  loadLinkMode();
  startWatchdog();                      // from here on: stuck 10 s = restart
}

// =====================================================
// LOOP
//
// The sensor is read every 2.5 ms, averaged, and processed at a
// fixed 100 steps per second. The web server is served in between.
// =====================================================

// ---------- Watchdog ----------

const uint32_t WATCHDOG_MS = 10000;      // stuck this long = restart

// loop() must check in at least every WATCHDOG_MS, or the chip restarts
void startWatchdog()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = { WATCHDOG_MS, 0, true };

  if(esp_task_wdt_reconfigure(&cfg) != ESP_OK)
    esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WATCHDOG_MS / 1000, true);
#endif

  esp_task_wdt_add(NULL);               // this task: setup() + loop()
}

// why the rod started this time
const char* resetReason()
{
  switch(esp_reset_reason())
  {
    case ESP_RST_POWERON:  return "power";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "low battery";
    case ESP_RST_DEEPSLEEP: return "sleep";
    default:               return "other";
  }
}

// ---------- Battery ----------

// Resting LiPo voltage -> percent
int lipoPercent(float v)
{
  static const float V[] = { 3.27, 3.61, 3.69, 3.71, 3.73, 3.75, 3.77, 3.79, 3.80, 3.82, 3.84,
                             3.85, 3.87, 3.91, 3.95, 3.98, 4.02, 4.08, 4.11, 4.15, 4.20 };

  if(v <= V[0])
    return 0;

  for(int i = 1; i < 21; i++)
  {
    if(v < V[i])
      return (int)((i - 1 + (v - V[i - 1]) / (V[i] - V[i - 1])) * 5 + 0.5);
  }

  return 100;
}

void updateBattery()
{
  if(lastBatRead != 0 && millis() - lastBatRead < BAT_READ_MS)
    return;

  lastBatRead = millis();

  // 16 readings: WiFi makes the ADC a little noisy
  float sum = 0, lo = 99, hi = 0;

  for(int i = 0; i < 16; i++)
  {
    float v = analogReadMilliVolts(BAT_PIN) * 2 / 1000.0;   // x2: the divider halves it

    sum += v;

    if(v < lo) lo = v;
    if(v > hi) hi = v;
  }

  float v = sum / 16;

  // Nothing wired: the pin floats (jumps around) or reads near 0
  if(v < 2.8 || v > 4.6 || hi - lo > 0.4)
  {
    batVolts = 0;
    batPercent = -1;
    return;
  }

  batVolts = batVolts == 0 ? v : batVolts * 0.85 + v * 0.15;
  batPercent = lipoPercent(batVolts);

  // time left: from the drop since the start (or since it was charged)
  if(batStartPct < 0 || batPercent > batStartPct + 3)
  {
    batStartPct = batPercent;
    batStartMs = millis();
    batLeftMin = -1;
  }
  else
  {
    float minutes = (millis() - batStartMs) / 60000.0;
    int drop = batStartPct - batPercent;

    if(minutes >= 15 && drop >= 2)
      batLeftMin = (long)(batPercent / (drop / minutes));
  }

  if(batPercent < 15 && !batLowSaid)
  {
    batLowSaid = true;
    Serial.printf("BATTERY LOW: %d %% (%.2f V)\n", batPercent, batVolts);
  }
  else if(batPercent > 25)
  {
    batLowSaid = false;
  }
}

void loop()
{
  esp_task_wdt_reset();                 // "still alive" for the watchdog

  server.handleClient();

  logTick();

  oversample();

  maybeSaveTotalTime();

  updateBattery();

  unsigned long now = millis();

  if(now - lastSampleTime < SAMPLE_MS)
    return;

  // If the web server held us up for long, do not try to catch up
  if(now - lastSampleTime > SAMPLE_MS * 10)
    lastSampleTime = now;
  else
    lastSampleTime += SAMPLE_MS;

  processSample();
}


// =====================================================
// LOGO (WebP, 17102 bytes) - served at /logo.webp
// =====================================================

const uint8_t LOGO_WEBP[] PROGMEM =
{
  0x52,0x49,0x46,0x46,0xc6,0x42,0x00,0x00,0x57,0x45,0x42,0x50,0x56,0x50,0x38,0x20,0xba,0x42,0x00,0x00,0xb0,0xc0,0x00,0x9d,
  0x01,0x2a,0x2c,0x01,0x2c,0x01,0x3e,0x79,0x34,0x95,0x47,0x24,0xa3,0x22,0x21,0x2b,0x17,0xcc,0x18,0x90,0x0f,0x09,0x68,0x6e,
  0xe0,0xc0,0x00,0x64,0x7d,0xa0,0x2e,0xcf,0xab,0x9d,0xf5,0xfb,0xff,0x39,0x5e,0x41,0xee,0x2b,0xde,0xff,0x74,0xf3,0x4f,0xdb,
  0x8f,0xce,0xff,0xcc,0xf2,0xe0,0xf6,0x6f,0xe4,0xfc,0xe5,0x7f,0xb8,0xf5,0x23,0xfa,0xd3,0xd8,0x0f,0xf5,0xcf,0xf6,0x33,0xdc,
  0xf7,0xfd,0x7f,0x56,0x7f,0xe1,0xff,0xea,0x7e,0x33,0x7c,0x0d,0xfd,0xb2,0xfd,0xb0,0xf7,0xa1,0xfc,0xb0,0xf7,0x6b,0xfe,0x3f,
  0xd4,0x0f,0xfa,0xb7,0xfb,0x8f,0xff,0xfd,0x85,0xbf,0xbb,0xbe,0xc1,0xbf,0xb4,0xbe,0x9c,0xbf,0xba,0xbf,0x09,0xff,0xde,0xbf,
  0xec,0x7e,0xe3,0x7b,0x55,0x7f,0xf3,0xf6,0x00,0xff,0xf7,0xed,0x93,0xfc,0x03,0xff,0xc7,0x58,0x7f,0x60,0x3f,0xcb,0x76,0xcd,
  0xfe,0x3b,0xc2,0xbf,0xc6,0xbe,0x81,0xfc,0x0f,0xf6,0x7f,0xdb,0xff,0xf0,0x5e,0xe0,0x19,0x73,0xec,0xb7,0xe9,0x3f,0x52,0xbf,
  0x98,0x7d,0xef,0xfd,0x5f,0xf7,0xef,0xdd,0xcf,0xf0,0x9e,0xe3,0xff,0xad,0xff,0x35,0xfb,0xab,0xe6,0x7f,0xc8,0xbf,0xf5,0x3d,
  0x42,0x3f,0x15,0xfe,0x63,0xfe,0x67,0xfb,0x9f,0xef,0x0f,0xf8,0xaf,0x87,0xff,0xb9,0xff,0x91,0xdd,0x43,0xb5,0xff,0xa1,0xfd,
  0xa9,0xf6,0x0b,0xf6,0x1b,0xe9,0xbf,0xed,0xff,0xc2,0x7e,0xf4,0x7f,0xb1,0xf8,0x7a,0xf8,0xaf,0xf6,0xdf,0x9a,0x9e,0xe7,0xfd,
  0x95,0xff,0x4f,0xf9,0xc5,0xf4,0x03,0xfd,0x03,0xfb,0x27,0xfb,0x1f,0xb8,0x5f,0x6a,0xcf,0x07,0x6f,0xc6,0xff,0xbf,0xfd,0xaf,
  0xf8,0x00,0xfe,0x63,0xfd,0xdf,0xfe,0x37,0xf8,0xbf,0xcb,0xbf,0xa6,0x9f,0xeb,0xbf,0xf5,0xff,0xad,0xff,0x61,0xfb,0xc1,0xed,
  0xbb,0xf3,0xff,0xf2,0xdf,0xf8,0x7f,0xcb,0x7f,0xaf,0xf9,0x0a,0xfe,0x61,0xfd,0x97,0xfe,0x77,0xf8,0x9f,0xf3,0xde,0xfb,0x1f,
  0xff,0xfd,0xd4,0x7e,0xe3,0x7f,0xf4,0xf7,0x57,0xfd,0xac,0xff,0xfe,0x8a,0xe0,0xd0,0xfb,0x5a,0x52,0x58,0x89,0xfa,0x56,0x04,
  0xe0,0x26,0x88,0xa1,0x85,0x78,0xca,0x6f,0x99,0xb0,0x9c,0x8a,0x6a,0x19,0x63,0x4e,0x69,0xcc,0x7c,0x4a,0x12,0xd4,0xd1,0x43,
  0xed,0xde,0x17,0x7a,0x36,0xbe,0xeb,0x5c,0xbc,0xed,0xdb,0xeb,0x9b,0x8e,0xb7,0x53,0x6b,0xfc,0x3e,0x24,0xae,0xe4,0x5a,0x24,
  0xaf,0x4e,0x84,0x79,0x90,0x50,0xfb,0x77,0xdc,0x0f,0x7f,0xb6,0x27,0xa5,0xee,0xf1,0x5a,0x00,0x13,0xff,0xff,0xf9,0x53,0xe1,
  0xb7,0x61,0x4d,0x88,0x79,0x6c,0x76,0x22,0xa8,0xe2,0x5c,0x1c,0xc1,0xdb,0x22,0x65,0xb2,0x5a,0x7d,0x08,0xd0,0xa8,0x55,0x06,
  0x07,0x62,0x76,0x2d,0x7b,0xd6,0x03,0x48,0xf4,0x23,0x27,0x6f,0x84,0xfc,0x91,0xc1,0x21,0x4e,0xdd,0xa3,0x3b,0x5b,0x61,0x5e,
  0x11,0xa6,0x18,0xab,0xff,0xfe,0x5d,0x4e,0x2c,0x52,0xd8,0xf4,0x44,0x95,0x7e,0x5b,0x48,0xc3,0x77,0x4e,0x5d,0xc6,0x41,0x39,
  0x5b,0x6c,0x6d,0x2e,0x61,0xfb,0xbc,0x45,0xd5,0xa6,0x63,0x26,0x3e,0x3e,0xa9,0x44,0x5d,0x80,0xd1,0xdd,0x1e,0xe8,0x1a,0x84,
  0xe8,0x20,0xb1,0xd6,0x9d,0xa9,0x8a,0xe0,0x75,0xba,0x9b,0x00,0x85,0x73,0x5d,0x7f,0xb2,0xfd,0x48,0xa1,0xaa,0x42,0x07,0xfd,
  0xda,0xce,0x8b,0xfa,0x88,0x35,0x18,0x75,0x71,0x51,0x37,0xc9,0x50,0xfa,0xd0,0x5d,0xa6,0x69,0x54,0x5a,0xdb,0x6a,0xf2,0x4a,
  0x68,0x31,0x5f,0xe1,0xa3,0x98,0x62,0x74,0x83,0x29,0xa8,0xa4,0x5a,0xef,0x59,0xff,0x79,0x9a,0x9f,0xc1,0xea,0x27,0x98,0xfb,
  0xf0,0xcf,0xea,0xae,0xd2,0x23,0x79,0xb2,0x1e,0x53,0x57,0x78,0xba,0xf0,0x10,0x02,0xad,0x84,0x82,0x15,0x47,0x74,0x5d,0x9f,
  0xb0,0x81,0x8e,0x26,0x5c,0x5c,0x08,0xbe,0x55,0x23,0x63,0x0e,0xe3,0x39,0x74,0xcf,0x51,0x7d,0x37,0xe4,0x4f,0xd4,0xae,0xb9,
  0x16,0x8e,0xe4,0x86,0x58,0xe2,0x37,0xda,0x2b,0x55,0x6d,0x7f,0x64,0x43,0x7f,0x98,0x5a,0xe6,0x82,0x4e,0x95,0x44,0xb8,0x4f,
  0x7f,0x86,0x09,0xc9,0xc3,0xfd,0x0b,0x3d,0x54,0x7a,0x80,0xa1,0x61,0x65,0x6d,0xfb,0x09,0xde,0x7f,0x21,0x68,0x45,0xab,0xae,
  0xe6,0x56,0x84,0x2b,0x28,0x68,0xc7,0x80,0x60,0xe6,0x8a,0x19,0x4e,0x44,0xdf,0x90,0x9f,0xa6,0x67,0x25,0xc4,0xd4,0x57,0x68,
  0x72,0xe0,0xc4,0x47,0x60,0x9a,0x80,0xd5,0x4f,0x56,0xcf,0x67,0x4f,0x3e,0xa5,0x7e,0x28,0x5f,0x77,0x48,0x66,0x53,0x45,0x51,
  0x96,0xbc,0x92,0x48,0xd3,0x21,0xf1,0xcd,0xa4,0xe8,0x69,0x3f,0x5f,0x04,0x32,0xe2,0x4b,0x40,0xa6,0xf2,0x9e,0x93,0x7f,0x14,
  0x54,0x02,0x5a,0xa0,0x6c,0x8d,0x88,0xe9,0xa8,0x4d,0x4a,0xad,0x4b,0x15,0x62,0xc7,0xfb,0x28,0x71,0x8d,0x74,0x3c,0xfe,0xc2,
  0xe9,0x8c,0x3c,0xfe,0x7d,0x86,0xcf,0xd5,0x70,0x9e,0x86,0x62,0x21,0xa2,0x29,0xb8,0xf1,0x21,0x6c,0xda,0xb5,0x38,0x5d,0x06,
  0x68,0x34,0x4c,0x76,0x40,0xbf,0x37,0x90,0x8b,0xa6,0xc6,0x59,0xb2,0xab,0x17,0x40,0x1f,0xf3,0x3c,0x69,0xaf,0x01,0x18,0x0e,
  0xb5,0x5c,0x43,0x60,0x55,0x9a,0x8d,0x4e,0x31,0x31,0x4e,0xcd,0x73,0x53,0x5a,0x47,0x05,0x31,0xd8,0x74,0xf7,0x44,0xea,0x1f,
  0x32,0xa4,0x79,0xdf,0xbc,0xa7,0x2a,0xb4,0x52,0xa9,0x92,0xf5,0x1f,0xfd,0xfd,0xe9,0x25,0x84,0x4d,0xe0,0x0d,0x68,0x5d,0x0d,
  0x58,0x7a,0x1a,0xda,0x95,0x09,0x51,0xf3,0xd4,0x89,0xaa,0x05,0xb5,0x02,0xe9,0xe7,0x39,0xa0,0xa8,0x7b,0x8b,0x50,0x4c,0x07,
  0x2c,0xcc,0x85,0xa2,0xeb,0x11,0xb8,0x4c,0x2e,0xe7,0xf9,0xb5,0x03,0x95,0xec,0x86,0xbb,0xe9,0x20,0x1c,0xda,0x04,0x7c,0x11,
  0xac,0x14,0x3b,0xbb,0xbb,0x8b,0x8c,0x14,0x78,0x96,0xa0,0x14,0xb7,0x62,0x41,0x2e,0xea,0x8f,0x2e,0x87,0x96,0x59,0x12,0xae,
  0x94,0x1f,0x19,0x7e,0xef,0x3c,0x64,0x69,0xea,0x6d,0x9d,0x4b,0x8f,0x30,0x98,0x89,0x8f,0x0d,0x8e,0x5e,0x5c,0xb8,0xa2,0x39,
  0xda,0x62,0xc4,0x4e,0x6b,0x87,0x95,0xd8,0x37,0xd6,0xa9,0xdb,0x6c,0x53,0xaf,0xb3,0x88,0xad,0x28,0xaf,0xec,0x02,0xb9,0x4a,
  0x69,0xbc,0x09,0x20,0xae,0x07,0x6c,0x4b,0xb7,0xc9,0x86,0xe2,0x94,0x46,0x9e,0x2f,0xcc,0x57,0x30,0x44,0xf5,0xff,0x2d,0x67,
  0x30,0x71,0xf5,0x64,0x65,0xb3,0x11,0xa8,0xb9,0x35,0xda,0x85,0x43,0xf1,0x81,0x89,0x0c,0x7e,0xf5,0x3f,0x94,0x7b,0x78,0xd4,
  0xd5,0x70,0x06,0x54,0x79,0x46,0xf2,0xc3,0xe8,0xac,0xaa,0xf2,0xec,0xc8,0x33,0xbe,0xc3,0xeb,0x64,0x70,0x7f,0x8e,0x35,0x67,
  0xb0,0x17,0x0b,0x87,0xd7,0xe5,0x37,0xa4,0x22,0xbf,0xf2,0x61,0x60,0x3c,0xab,0xaf,0x4a,0x9e,0x4d,0xf7,0xb9,0x0e,0x21,0xa6,
  0xf7,0xc5,0xac,0x5f,0x99,0xa2,0x9a,0xd5,0x01,0xfa,0x51,0xde,0x9e,0x46,0xa8,0x5f,0xb0,0x2f,0x62,0x91,0xc5,0xa7,0x26,0xc7,
  0x0e,0x18,0x46,0x72,0x93,0xc3,0xb4,0x2d,0x6e,0x6b,0x21,0x53,0x79,0xf1,0x4d,0x28,0x12,0xfa,0x71,0xa8,0x89,0xe7,0x09,0x96,
  0x36,0x6d,0xbf,0x18,0xe7,0x94,0x28,0xa2,0xa3,0x09,0x5a,0xae,0x28,0x41,0x18,0xc7,0x0e,0xe6,0xbd,0x8f,0x7e,0x8a,0xd0,0x39,
  0x74,0x84,0xc5,0x42,0x36,0xd9,0x3d,0xa9,0x38,0xd8,0x75,0xe1,0x8d,0xa3,0xfd,0xbf,0x40,0x6c,0x39,0x57,0xa3,0xb9,0x74,0x27,
  0x7f,0xf4,0x31,0x3a,0x63,0x2e,0xa1,0x7d,0x77,0xd7,0x7e,0x3c,0xf1,0x1f,0x17,0xb0,0x5c,0xd3,0x6c,0xd5,0x34,0xa8,0x58,0x38,
  0x1d,0xbf,0x22,0x14,0x4f,0xbb,0x7a,0x99,0xed,0x32,0x0b,0x1a,0x49,0xd1,0xd3,0x9d,0x3e,0x95,0x1d,0xa4,0x9e,0xe4,0x80,0x17,
  0x33,0x15,0xbb,0x1a,0xff,0x59,0xcf,0x86,0x79,0xc4,0xce,0x8d,0xa6,0x7a,0x65,0xec,0x93,0x03,0xda,0x9b,0xe1,0x8c,0x2a,0x0a,
  0x34,0xf8,0x8c,0x50,0x46,0x65,0x81,0x00,0xea,0xc8,0x1a,0x8a,0x33,0x4b,0xb0,0xbe,0x5d,0xe0,0xbf,0x10,0xe2,0xeb,0x78,0x23,
  0x3d,0x6c,0xf4,0x2d,0xb8,0x21,0x04,0x6d,0x50,0xe0,0x6a,0xa2,0x94,0xa2,0x58,0x84,0x2c,0x22,0xe4,0x17,0x97,0xdd,0xc3,0x78,
  0xe2,0x30,0x73,0x71,0xa7,0x2c,0x61,0x8c,0x90,0xff,0x55,0xd4,0xdb,0x10,0xab,0xd7,0x6b,0x0e,0x60,0x32,0x53,0x08,0x74,0x02,
  0x64,0xc6,0x63,0x4d,0x78,0xf4,0x70,0xa9,0x65,0x2a,0x88,0xa9,0x35,0x2d,0x10,0x5b,0x73,0x9f,0x46,0x7e,0x78,0x99,0x7f,0xc4,
  0xd3,0x9f,0x4b,0x87,0xf8,0x7b,0x88,0xa2,0x8b,0x6d,0x3d,0x71,0xb0,0x11,0x5e,0x88,0xed,0x79,0xbb,0xcb,0xe5,0x10,0x7e,0xd7,
  0xf0,0x46,0xe2,0x41,0x74,0x45,0xbc,0xf8,0x2b,0xbb,0xf3,0xc8,0xbe,0x7f,0x95,0x2b,0x0e,0x11,0x1e,0x09,0xdc,0x93,0xa2,0xf7,
  0xa5,0x06,0xd7,0x14,0x37,0xfd,0x97,0xdf,0xd5,0x1c,0xc2,0x1d,0x55,0xda,0x4f,0x98,0x25,0xbb,0xb2,0xbf,0xc3,0x77,0xb7,0xe4,
  0x89,0x66,0xd4,0x1b,0x34,0x51,0xfb,0xfc,0x74,0x12,0x70,0x6e,0x70,0xcf,0x5b,0xb7,0x81,0xbe,0x72,0x34,0xc7,0x8c,0xa2,0x28,
  0xac,0x2e,0xfe,0x61,0x86,0x6f,0x0b,0x03,0x69,0xb2,0x1e,0xc8,0x7e,0xc0,0xc4,0x85,0x1e,0xf0,0xe8,0x27,0xd4,0x82,0x04,0xc2,
  0x13,0xa5,0x7b,0x0e,0xae,0x67,0x50,0xee,0x12,0x1a,0x9f,0x99,0x35,0x71,0x1f,0x5f,0x29,0xbf,0x3f,0x7f,0x71,0xb7,0xd9,0x86,
  0x64,0xd8,0x2b,0x24,0xef,0x8e,0x24,0xea,0xb5,0x1d,0xe3,0x9c,0x47,0x03,0x43,0xa8,0x0b,0x1a,0x45,0xee,0x87,0xbb,0x1d,0xf3,
  0x24,0x24,0x5b,0x47,0x34,0xe9,0xb1,0x66,0x05,0x83,0x25,0xc4,0x03,0xbe,0xef,0xb2,0xd3,0x6a,0x68,0xa2,0x8b,0x66,0x46,0xce,
  0x1d,0x58,0x6c,0x65,0x7a,0xad,0x54,0x46,0x15,0x40,0x00,0xfe,0xd5,0xc9,0x80,0x40,0x0d,0x34,0xa7,0xf9,0x1d,0x74,0x1e,0x74,
  0x90,0x70,0x6c,0xd1,0xd8,0x01,0xba,0x44,0x09,0x9f,0xdc,0x96,0xfa,0x68,0xac,0xb3,0x59,0x4c,0xa3,0x54,0x26,0x6d,0xcb,0xd8,
  0xbe,0x4c,0x02,0x3d,0xaa,0xae,0x20,0x8c,0x3e,0xb2,0x1d,0x3d,0x3e,0xc3,0x51,0x92,0xd5,0x23,0x3f,0x29,0x1d,0x2c,0x0b,0xbb,
  0x3f,0x5d,0x46,0x42,0x41,0x0a,0xb3,0x79,0xf6,0x47,0x70,0xf0,0xe4,0x00,0x27,0x8a,0x12,0x7b,0xdb,0x8b,0x70,0x02,0x15,0xa0,
  0xb9,0x56,0xf1,0x86,0x3f,0x70,0x2b,0xad,0x7d,0x0c,0x45,0x1f,0xb6,0xe2,0xd6,0x5d,0x01,0x2c,0xe3,0xf1,0x8f,0xb8,0xf5,0x42,
  0xb5,0x68,0x00,0x06,0x1b,0x96,0x02,0xd3,0x37,0x7d,0xf0,0xde,0x43,0xa2,0xb9,0x2a,0x69,0x28,0xd2,0xc1,0x41,0xfd,0x19,0xe6,
  0x1b,0xb7,0x9d,0xf6,0x11,0x0e,0xf0,0x2c,0x8e,0x02,0xf9,0x26,0x55,0x49,0x9e,0x95,0xa7,0x8c,0x10,0xeb,0x27,0xe9,0xc6,0xc7,
  0xdd,0xb0,0xae,0x5f,0xaf,0xe1,0xee,0x10,0x06,0xed,0x87,0x7f,0x9d,0xbf,0xa3,0x6c,0x96,0xe2,0x3a,0xbd,0x5f,0x51,0x3a,0x56,
  0x26,0x4f,0xd6,0xae,0xaf,0x41,0x90,0xe4,0x1d,0x49,0x50,0xd8,0x28,0x84,0x58,0x0a,0x5b,0x94,0x91,0x97,0xe1,0x67,0x51,0xc5,
  0x34,0xdd,0x08,0x85,0xc4,0xdf,0x3a,0xff,0x5e,0x4f,0xe6,0x2c,0xb8,0x99,0x64,0x23,0x51,0x4d,0x9b,0x5c,0x00,0x00,0x30,0xc9,
  0x69,0x1e,0xc1,0x67,0x4e,0xba,0x8e,0xa7,0xa3,0xdf,0x32,0x00,0x50,0x82,0x6d,0xff,0x76,0x44,0xda,0xd4,0x51,0xaf,0x23,0xaa,
  0x87,0x1a,0xd1,0x6c,0x39,0x48,0x0e,0x9c,0x66,0x3b,0x25,0x06,0x11,0x2d,0xbd,0x25,0xf9,0x3f,0x22,0x2a,0x7c,0x69,0x39,0x14,
  0x60,0xaa,0xb8,0x66,0x70,0x03,0x03,0x42,0xe4,0x39,0x8e,0xc2,0x99,0x78,0xcd,0xf4,0xf0,0x68,0x13,0xf5,0x15,0xbd,0x82,0x84,
  0x2b,0xf9,0x32,0xf4,0x0d,0x32,0xbd,0xb9,0x6a,0x14,0x66,0xff,0xaf,0x3e,0xad,0x65,0x1f,0xf5,0x44,0x8c,0x4a,0x9d,0xca,0xa1,
  0xcd,0x5b,0x7c,0x0b,0x5e,0xfd,0xea,0xfd,0xe1,0x35,0x07,0x1e,0xee,0x31,0xae,0x84,0x12,0x95,0xc4,0xd0,0x52,0x14,0xc5,0x34,
  0x51,0x9f,0x19,0xfe,0xba,0x95,0x4b,0xb0,0xdd,0x6b,0x41,0x6f,0x2d,0x17,0x36,0xf9,0xbb,0xcb,0x4b,0x08,0x01,0x9d,0x33,0xd4,
  0x1b,0x52,0x11,0x65,0x03,0xdf,0xc9,0xa2,0xd6,0x16,0x3f,0x60,0xed,0x1d,0xb8,0xba,0xb2,0xfb,0xec,0xde,0xea,0x5a,0x53,0xfc,
  0xb5,0x45,0x6f,0xe3,0xc4,0x6f,0xba,0x55,0xc6,0x37,0x20,0x7c,0x28,0xd4,0x5c,0xb6,0x1d,0x66,0xa6,0x98,0x3f,0x12,0x8f,0x66,
  0x5b,0x5e,0x57,0x24,0xe2,0x65,0x68,0x72,0x4f,0xcc,0xa7,0x75,0x14,0x65,0xdd,0x19,0xd4,0x10,0x8f,0x29,0xe1,0x00,0xba,0x3d,
  0xee,0x84,0x9f,0xe8,0x62,0xe5,0x5b,0x99,0xd4,0xbd,0x43,0x65,0x6c,0x9c,0xdd,0x96,0x5c,0xb1,0xd0,0x93,0xb1,0x38,0x97,0xd7,
  0x19,0xe2,0x8b,0xd3,0xdc,0x78,0x81,0x22,0x9f,0x3c,0xb9,0xf8,0x74,0x3e,0xb3,0x10,0x2b,0xae,0x97,0x5d,0x1c,0xa0,0x15,0x14,
  0x04,0x8c,0x74,0x88,0x99,0x16,0x6c,0x2d,0x5d,0xa8,0xe7,0x6b,0x80,0x3a,0x5e,0x0c,0x2f,0x86,0x53,0xa7,0x9c,0x8e,0x8b,0xbb,
  0xa7,0x0b,0xc8,0xa5,0x5d,0xf5,0x2e,0xca,0x80,0x4a,0xad,0xa2,0x34,0x88,0xff,0xb5,0xdf,0x55,0x40,0x8b,0x2a,0x7d,0xcc,0xe8,
  0x6f,0xc4,0x00,0xa1,0xc7,0xaa,0x2a,0x73,0x5e,0xab,0x70,0xbb,0xb5,0x28,0xc2,0x88,0xa1,0x2c,0xc1,0xe0,0x0d,0x46,0x7e,0x84,
  0xb7,0xa0,0xde,0xc8,0xc3,0x24,0xfb,0xb9,0x3a,0x92,0x7e,0x63,0xd8,0xb2,0x49,0x16,0xaf,0x23,0xe6,0x7e,0x56,0x62,0x66,0x74,
  0x0c,0x45,0x8f,0xd5,0x79,0x1e,0x7e,0x40,0x7a,0xaa,0x5f,0x66,0x8e,0x99,0x63,0x2e,0x49,0xd8,0xf9,0xff,0xed,0x76,0x4f,0x2a,
  0xeb,0xdf,0xea,0xb7,0x23,0x0b,0xcb,0x90,0x22,0xc4,0x75,0x63,0xd0,0xe9,0x06,0x9f,0xeb,0x9e,0x2c,0x06,0x01,0x22,0xed,0x36,
  0xb2,0xe8,0xb5,0xc0,0x29,0x5b,0x0d,0x4f,0x10,0x5d,0x20,0x84,0x60,0xba,0xbc,0xd3,0xf3,0x3b,0x17,0x6c,0xc2,0x83,0xbc,0x48,
  0x03,0xa5,0x6a,0xdf,0xdb,0x1a,0x0f,0xb0,0x5c,0x4c,0x67,0xe8,0xdf,0x5f,0x60,0xcf,0xf1,0x3f,0x2c,0x0b,0xe5,0x89,0xc6,0x3e,
  0x46,0x79,0x4c,0xa7,0x97,0x16,0x8d,0x71,0x07,0x29,0x1c,0x88,0x53,0x51,0x71,0x4e,0xee,0xd3,0x2a,0xe8,0x57,0x20,0xb5,0x28,
  0x67,0x1f,0xe6,0x86,0xb5,0xe9,0xd2,0x0c,0xc2,0xc7,0xcb,0x07,0xae,0x0c,0x6f,0x4f,0x90,0x1e,0x2f,0x7d,0xdf,0xa7,0xa9,0x6e,
  0x72,0xba,0xe9,0xf7,0xe5,0x7d,0x80,0x31,0x09,0x56,0x29,0xad,0x48,0x8d,0xd4,0x9b,0x4e,0x78,0xa9,0x12,0xe3,0x99,0x8e,0x38,
  0x19,0x03,0x82,0x71,0x9e,0xa8,0xdd,0xe3,0x10,0xbf,0xec,0x2a,0x94,0xa6,0xa4,0x55,0x3a,0x5f,0xa0,0x49,0xf5,0xf1,0x31,0xd6,
  0x95,0xc1,0x38,0x04,0x0e,0x83,0x65,0xde,0x31,0xb6,0xaf,0x2f,0xdc,0xb0,0x99,0xa7,0xb3,0x93,0x79,0x5f,0x13,0x7f,0x5c,0x82,
  0xaa,0x73,0x46,0xfd,0x94,0xef,0x0f,0xa9,0x1e,0xc0,0xf6,0x0f,0xbd,0x2e,0x14,0x46,0xd8,0x73,0xc4,0xeb,0x86,0x04,0xba,0xf8,
  0x87,0xd1,0xd8,0xe5,0xdf,0x7e,0xda,0x43,0x97,0x04,0xc1,0x30,0xc1,0x1b,0x38,0x1e,0x08,0xaf,0xe5,0xe5,0x7b,0x38,0xd2,0xc3,
  0x3c,0xcc,0x74,0x0c,0x7c,0xde,0xe6,0xd5,0x46,0x59,0x3d,0xba,0x8c,0xa0,0x9b,0x1f,0x6b,0xde,0xa0,0x39,0x69,0xf3,0x03,0xcd,
  0xb1,0xd1,0x40,0xe0,0xe2,0xf7,0xc4,0x2e,0xd9,0x43,0x46,0x7e,0x4a,0x3b,0x47,0x1c,0x10,0x2f,0x89,0xbe,0x23,0x8e,0x1a,0xb9,
  0xad,0x96,0x0c,0x2e,0xc1,0x68,0x6a,0x2f,0x49,0x54,0x5a,0x77,0x01,0x42,0x36,0x18,0x51,0x17,0x21,0xd5,0x86,0x28,0x79,0x90,
  0xb0,0x93,0x65,0xcd,0xdf,0x68,0xbf,0x15,0x69,0x79,0xa9,0x63,0xea,0x87,0xaa,0xa9,0x4b,0xb3,0xbe,0x0f,0x6c,0xe2,0x70,0xad,
  0x55,0x14,0x3f,0xae,0xbe,0x92,0x19,0xb6,0x14,0x36,0xcf,0xbd,0xa7,0xb6,0x36,0xde,0xd3,0x5c,0xba,0xe5,0xfc,0x58,0xc2,0xb6,
  0xae,0xbd,0x7e,0x0f,0xee,0xfe,0x4d,0x86,0x34,0x3b,0x9d,0x0f,0x6c,0x5e,0x96,0x64,0xbf,0xb6,0x0b,0x41,0x34,0x34,0xd9,0x27,
  0x27,0x06,0x19,0x5b,0x6e,0x3b,0x4c,0x2a,0x9e,0x4b,0xc4,0x2a,0x77,0x94,0x75,0xde,0xb0,0x80,0x1f,0x2e,0xb2,0xf6,0xa5,0xd0,
  0xaa,0x89,0x4d,0xbe,0xdb,0x9e,0x84,0x0d,0x9f,0x5c,0xb0,0x6a,0xbc,0x51,0x42,0xdc,0x0c,0xd1,0x11,0x48,0x92,0x2f,0x9a,0x09,
  0xfa,0x5d,0x59,0xc0,0x69,0x86,0xf6,0xb7,0x3e,0xa6,0x6c,0xf5,0x57,0xe9,0x54,0x61,0x42,0xd8,0x86,0xa4,0xaf,0xb9,0x6e,0x19,
  0xe8,0x02,0x6a,0xf1,0x2f,0x4a,0x0a,0xef,0xe7,0xf5,0xc2,0x9d,0x24,0x9c,0x37,0xf2,0xaa,0xaa,0x20,0xc7,0x1d,0x58,0x1e,0x42,
  0x6b,0x7f,0xba,0xfd,0x45,0x83,0x5f,0xbf,0x8a,0x41,0x6b,0x90,0xea,0xea,0xd5,0x44,0xa3,0x7e,0x8a,0x5f,0x5a,0x38,0xe5,0xeb,
  0x7b,0xf1,0x98,0x51,0x69,0x37,0x8a,0x91,0x37,0xfb,0x87,0xfc,0x35,0x12,0xbe,0x31,0x7c,0xcb,0x00,0x1d,0xf9,0x09,0xbd,0x9b,
  0xde,0x4b,0xe6,0xee,0xdc,0xb7,0xbd,0x0b,0xa7,0xc5,0x91,0xe9,0x50,0x18,0x71,0x66,0xe6,0xcb,0x0e,0x30,0xf8,0x2d,0x9b,0x98,
  0x23,0x55,0x7a,0x81,0x40,0x24,0xc2,0xae,0x01,0xc1,0xce,0x10,0x47,0xdc,0x2b,0xa9,0x9f,0xc0,0xac,0x1a,0xa6,0xd8,0xab,0x75,
  0x30,0x28,0xe9,0x51,0x55,0xb1,0xd3,0x19,0x12,0x6a,0xd0,0x2f,0xf2,0xcd,0xb5,0x76,0xc7,0xe0,0xac,0x4d,0xc3,0x46,0x79,0xe8,
  0xa6,0x32,0x9a,0x09,0x61,0xe0,0xd0,0x38,0x30,0xc8,0x9a,0xc7,0xdb,0xd4,0x65,0x8e,0x93,0x9f,0xc0,0xac,0x4e,0x00,0x8c,0x07,
  0x75,0x12,0x7c,0x1d,0x88,0x64,0xf8,0x3f,0x7b,0x89,0xd1,0x97,0xfe,0x68,0x7d,0x9f,0x8c,0x1f,0x1e,0xaf,0x66,0x30,0x94,0x0c,
  0x56,0x45,0x91,0x51,0x21,0x62,0x2b,0xda,0xdc,0x3c,0x0e,0xb6,0x0e,0x39,0x9b,0xf7,0xda,0xe1,0x47,0xcb,0xad,0xba,0xb2,0x9c,
  0x24,0x9e,0xa6,0x2e,0x85,0x52,0x39,0x8d,0x60,0xe3,0xb2,0x59,0x90,0x95,0xa5,0x93,0x36,0xf5,0xe4,0x9b,0x2c,0xd0,0xe2,0x84,
  0x11,0x90,0x85,0x17,0x8a,0x6f,0x72,0xc4,0x1a,0xa1,0xc8,0x7a,0xa1,0x3c,0xc6,0x11,0xbd,0xc1,0x17,0xce,0x0d,0xf8,0x51,0x80,
  0x93,0xe9,0xee,0x48,0x89,0xa9,0xef,0xe5,0x07,0xe2,0xf5,0x0f,0x99,0x33,0x97,0x53,0x52,0xce,0x96,0x92,0x18,0xfb,0x7c,0xc7,
  0xa9,0x15,0x84,0x52,0x9e,0xa7,0x78,0xdd,0x92,0x6c,0x13,0xaf,0x68,0x5a,0x96,0x78,0x54,0x95,0x21,0x6d,0x36,0xc8,0x3f,0x2e,
  0x71,0x7c,0x96,0x67,0xcc,0x52,0x08,0x4d,0xe6,0xea,0x4f,0x09,0x0a,0xa8,0xa0,0x4d,0x10,0xd0,0x85,0xed,0x2d,0x32,0x05,0xbd,
  0xbe,0xe6,0xb9,0x0d,0xe4,0xcb,0x41,0x0c,0x34,0x20,0x3e,0xbf,0xd2,0x05,0x81,0xbe,0xaf,0xa6,0xc6,0x26,0x88,0xb1,0x57,0xfb,
  0x61,0xfa,0xb0,0x5b,0x87,0xd3,0xbe,0x65,0x4e,0x20,0x7f,0x07,0x4f,0x6e,0x7f,0x6f,0x77,0x40,0xa1,0x26,0x97,0x79,0x01,0x8a,
  0x68,0xe6,0x19,0x5c,0x46,0xf6,0x38,0xaf,0x8e,0x94,0xb2,0x50,0xa3,0x67,0x3b,0x55,0x2f,0x78,0x0f,0x8b,0xac,0x44,0x3b,0x6c,
  0x60,0xf1,0x47,0x86,0xd1,0x48,0x10,0x90,0x7c,0x19,0xde,0x15,0xd9,0x97,0x43,0x8d,0xee,0x15,0xf7,0xcc,0x16,0xd2,0xb9,0xbb,
  0xe9,0x85,0x78,0xe0,0x85,0xb6,0x9c,0x40,0x2c,0xbd,0x1f,0x5d,0x11,0x85,0x5b,0xcb,0x02,0xa3,0xe8,0x0e,0x41,0xbe,0xd5,0xc5,
  0x81,0x78,0xf7,0xa1,0x1e,0xe7,0xca,0x54,0xc8,0xe7,0xe0,0x1c,0x54,0x14,0xe9,0x07,0xd8,0x88,0x93,0xd1,0x95,0x9a,0xbe,0x7f,
  0x50,0xe4,0xac,0x4e,0xec,0xf8,0xaa,0x9b,0x23,0xc8,0xfb,0x9b,0xee,0x58,0x8e,0xdf,0x01,0x83,0x0f,0xe5,0xb1,0x0f,0x29,0xee,
  0x63,0x9c,0x84,0x0f,0x22,0x7b,0x4b,0xb6,0x25,0x18,0xcc,0x17,0x0b,0x41,0x8c,0x30,0x62,0x85,0xc3,0xbb,0xec,0xde,0xca,0xa8,
  0x1a,0x60,0x25,0x24,0x95,0xc0,0x43,0xff,0x3c,0xb3,0x1a,0xdb,0x03,0x24,0xc7,0x18,0x85,0xca,0xe0,0x63,0xe2,0x2a,0x8a,0xf3,
  0xa2,0x5d,0xbc,0x5d,0x04,0x35,0x00,0x99,0x99,0xd8,0x09,0x15,0x6c,0x63,0xb8,0x29,0xd3,0x49,0x22,0xfe,0xab,0xfc,0xdf,0x8f,
  0x6d,0x74,0xdb,0x50,0x85,0xee,0xd0,0x8c,0xf4,0xd6,0x48,0xb6,0x57,0xf7,0x7c,0x19,0xf0,0xd3,0x76,0x18,0xec,0x73,0x7a,0xb1,
  0x84,0x7d,0xc9,0x92,0xc3,0x54,0xd2,0x76,0xe8,0xc6,0xa5,0x30,0x96,0x0a,0xab,0x09,0x5a,0xf6,0x7a,0x5f,0x36,0xb1,0x0a,0x80,
  0xbe,0x26,0xe8,0xa5,0xbf,0xd7,0x20,0xcd,0xdb,0x70,0x17,0x83,0x7b,0x3e,0x65,0x31,0x4e,0xf7,0xe5,0x4b,0xc6,0xcc,0x63,0x7e,
  0x48,0xde,0x69,0xad,0x90,0xb7,0x51,0x6c,0xdc,0xf8,0x4a,0xdf,0x3b,0x4a,0xc3,0xdf,0xd5,0xb0,0x76,0xd0,0x05,0xf4,0x3a,0xd8,
  0x9c,0xc3,0xe5,0x28,0xe6,0x37,0x51,0xe1,0x51,0x8f,0x7d,0xe8,0xce,0xfd,0x9d,0xe4,0x08,0x00,0x83,0xc7,0x61,0x9d,0x5f,0x4d,
  0x96,0x2d,0x83,0x00,0x95,0xb7,0xc1,0xb2,0x75,0x7a,0x28,0xd7,0x21,0x01,0x2d,0xc8,0x71,0x4b,0xaa,0x95,0x86,0x4a,0x84,0x9d,
  0xe5,0xf4,0x62,0x20,0xfd,0xfb,0xf0,0x30,0x6b,0x8b,0x2c,0x1b,0x96,0x75,0xc1,0xe0,0x7d,0x5d,0xc5,0x4a,0x55,0xa0,0x0a,0x2b,
  0x7c,0x63,0x2e,0xdc,0xc0,0x0f,0xdc,0x9c,0xb2,0x48,0xb6,0xde,0x3f,0x5c,0x6a,0x2f,0x16,0xb1,0x74,0xea,0xf8,0xb4,0x0d,0x5d,
  0xf8,0x96,0x1a,0xe3,0xca,0x30,0xa6,0x45,0xd9,0x63,0xd0,0x91,0xab,0x7d,0x5b,0xf6,0x64,0x0c,0xc6,0xcb,0xd5,0x27,0x5e,0xa7,
  0xf4,0x51,0xcf,0x15,0x1a,0x74,0x4f,0xf7,0x02,0x71,0xed,0xe8,0x2e,0xad,0xda,0x3e,0xf0,0xe3,0x01,0x40,0xd3,0x66,0xe8,0xb0,
  0xe0,0x82,0x8f,0xf3,0xc8,0x52,0xce,0x43,0x93,0x3d,0xcd,0xea,0x5d,0x6b,0x86,0x7f,0xd4,0xb6,0xe0,0xde,0xf0,0x02,0x3f,0x8e,
  0x8b,0xde,0x07,0xa2,0x1a,0x9a,0xfd,0x8b,0x3b,0xb1,0x60,0x4e,0x94,0xfd,0x3a,0x62,0x9f,0xfc,0x57,0x98,0x41,0xdf,0x23,0x15,
  0x21,0xfd,0x63,0x9a,0x13,0x5b,0x61,0x3b,0x26,0x73,0x44,0x75,0xfd,0xaf,0xbf,0x4b,0x2d,0xb8,0x05,0x0f,0xdf,0x06,0x01,0xc7,
  0x90,0x69,0x3f,0x7b,0x74,0x31,0x9f,0x2e,0xcc,0xb5,0xc4,0xa4,0xe6,0xf8,0x67,0x7e,0xb5,0x41,0x1a,0x75,0x65,0xd0,0x72,0xd9,
  0x73,0x53,0xdb,0xf6,0x67,0xe7,0x69,0xc0,0x49,0xc6,0x42,0xae,0x0c,0xc1,0xf4,0x2f,0x00,0x35,0xf4,0x1e,0xa9,0x29,0x5e,0x5d,
  0xca,0x7a,0xe5,0x1d,0xf1,0x59,0x1e,0x39,0xa6,0xf3,0xda,0x37,0x79,0xbb,0x9f,0x03,0xe1,0xdf,0xf3,0xaf,0x1d,0x82,0x9c,0xc7,
  0x86,0x13,0xd2,0xfa,0x63,0x27,0xbd,0xc7,0xe8,0x8a,0x87,0xb6,0x60,0x6e,0x7d,0x52,0x3b,0x33,0x06,0x59,0x13,0x49,0xbe,0x2a,
  0x4e,0xe4,0x03,0x79,0x77,0x7b,0xf1,0xb9,0x2f,0x57,0xc5,0xa0,0x41,0x97,0x75,0xdb,0x4d,0x75,0x91,0xdb,0x0e,0xf7,0xf2,0xb5,
  0x6c,0x4f,0xdf,0x79,0x41,0x9b,0x8f,0xd5,0x88,0x2f,0xbe,0x25,0xf9,0x39,0xaa,0x2d,0x04,0xbb,0x35,0x34,0x1e,0x06,0xf1,0x43,
  0x18,0xfb,0x76,0x9e,0x8e,0x33,0xc7,0x23,0x87,0x4d,0x91,0xf6,0x8b,0x03,0x5e,0x75,0xfc,0x38,0xbd,0xf9,0x1a,0x70,0x37,0x93,
  0x26,0x5c,0x43,0xc4,0xf9,0x3e,0x0e,0x7c,0xad,0x99,0xad,0xca,0x4a,0xdc,0x4c,0xdc,0x9b,0xc7,0xd4,0x96,0x17,0x65,0xcb,0x73,
  0xe6,0x9c,0xf4,0xac,0x69,0xcf,0x7a,0xc4,0x01,0xe9,0x38,0xec,0x4f,0x60,0xaa,0xb4,0x7d,0x63,0x44,0xd9,0x4a,0x99,0x02,0x97,
  0xe6,0x8e,0xfd,0x0a,0x78,0xef,0xb4,0xbe,0xe5,0x4d,0xdd,0xe9,0x45,0x4c,0xeb,0x0e,0x0d,0x0d,0x55,0xff,0xe7,0xc9,0xef,0x16,
  0x8e,0x3b,0xb1,0x0d,0x6c,0xec,0xfb,0x78,0x6d,0x6a,0x9d,0xee,0xff,0x07,0xda,0x3e,0xb2,0xd4,0x2c,0xa0,0x3d,0xbb,0xc1,0xf6,
  0x6e,0x82,0x9d,0x9b,0x45,0xe8,0x61,0xaa,0xbd,0xf4,0x95,0x87,0x9d,0xdd,0x05,0x50,0x3e,0xa2,0x80,0x81,0x70,0x32,0x67,0xf3,
  0x25,0x92,0x76,0x09,0x2a,0xb9,0x0c,0xc1,0xf9,0x87,0x0b,0xd8,0xde,0x6a,0x3f,0x47,0xb0,0xa8,0x07,0xcf,0x98,0xb5,0x96,0x3f,
  0x15,0xee,0xc9,0xca,0xb7,0xc1,0xb6,0x67,0xfb,0x2b,0x7e,0x69,0x1f,0x22,0x09,0x9e,0xf4,0x95,0xd9,0x48,0x06,0xa2,0xcf,0x03,
  0xe1,0x1b,0x0a,0x2b,0x29,0xc1,0x14,0x6b,0x35,0xb2,0x94,0x2a,0x51,0xec,0x69,0x9b,0xcf,0x27,0xb8,0x00,0xef,0xa7,0xc0,0xc1,
  0x44,0x83,0x6f,0x49,0x2d,0xd1,0x83,0xfb,0x71,0xdf,0x8f,0x30,0x33,0x3d,0x8e,0x9d,0xc0,0x76,0x23,0xa8,0xd6,0x15,0x22,0x7e,
  0x80,0x07,0x40,0xa0,0x11,0xc7,0x8e,0x8a,0xff,0x85,0xc8,0xae,0xed,0xcf,0x0a,0xaf,0x4c,0x3a,0x93,0x07,0x27,0xab,0x92,0xec,
  0xda,0x3f,0xe2,0x26,0xa8,0x79,0x7d,0x1d,0x21,0x69,0x15,0xa1,0x03,0x46,0x33,0xda,0x7a,0xb0,0x48,0x66,0x35,0x01,0x69,0xee,
  0x2a,0x4d,0x01,0x58,0x3c,0x8a,0x02,0x44,0xfc,0xe6,0x1a,0x48,0x90,0x56,0x32,0xd9,0xb1,0x89,0xf6,0x81,0xf3,0xdc,0xb4,0xf1,
  0x34,0x96,0x3c,0x8c,0x20,0x45,0xf5,0xa4,0x4d,0xf6,0xe2,0xb4,0xfa,0xec,0xcc,0x18,0x3a,0x54,0x8e,0x97,0x8e,0x67,0x97,0xe3,
  0x04,0xf4,0x01,0x72,0xc6,0x81,0x42,0xad,0xf4,0xcf,0x97,0xae,0x9e,0x98,0xed,0xde,0x24,0x48,0xdc,0x9e,0xd3,0xdc,0x5d,0x3a,
  0x50,0x60,0x5e,0x88,0x39,0x3a,0x92,0xc1,0xf7,0x61,0x52,0x35,0xd7,0xdc,0x8c,0x93,0xea,0x54,0x9f,0xbf,0x3f,0xe4,0x55,0xaa,
  0x3d,0x4f,0x06,0xc1,0xad,0x84,0x16,0x4f,0x04,0x6b,0x7e,0x14,0x82,0x71,0xc3,0x9e,0x69,0x96,0xe9,0x8a,0xbe,0x37,0xc8,0xf0,
  0x3a,0x8a,0xa5,0x59,0x02,0x7e,0xdd,0x3d,0xcc,0x23,0x21,0x0d,0x88,0xb2,0x18,0x4f,0x1b,0x7b,0x39,0x2c,0x8e,0x12,0xbe,0x66,
  0x64,0x6f,0x77,0x90,0x60,0xf3,0x6b,0x7d,0x21,0x77,0xc9,0xbf,0x72,0x4a,0xbf,0x5c,0x8c,0x9d,0xc4,0x44,0x9d,0xfa,0x77,0xe1,
  0x73,0x8c,0x34,0x16,0x1f,0x83,0x75,0xd0,0xc2,0xf2,0xa8,0x89,0xad,0x4a,0xd7,0x02,0xbf,0xbc,0xd3,0x9b,0x61,0x8d,0xc2,0x22,
  0xe7,0x2c,0x14,0x86,0x7c,0x50,0xaf,0xda,0xdc,0xba,0xf2,0x7d,0x8d,0xaf,0x5f,0x7c,0x86,0xfa,0x26,0x08,0x7b,0x6b,0xf0,0x1f,
  0xf3,0x15,0xc2,0x5f,0x37,0x6a,0x1c,0x37,0xb5,0x94,0xaa,0x13,0x9b,0x8d,0x35,0xe0,0xed,0x65,0x80,0x9e,0xd6,0x56,0x11,0xf1,
  0xa6,0xcc,0xe7,0x21,0x87,0xbd,0x36,0xa8,0x38,0x60,0xc1,0xbc,0x4c,0x5d,0xfe,0x5b,0x7c,0xcc,0x3a,0x9b,0x4a,0x60,0x0f,0xa5,
  0x96,0x9e,0xba,0xac,0x18,0xf9,0x7b,0x1b,0x06,0xa2,0xd2,0xf5,0x00,0x4e,0x18,0x9b,0xed,0xac,0x22,0x6a,0xb3,0x97,0x49,0x93,
  0x47,0x15,0x3b,0x22,0x97,0x44,0x36,0xd2,0x95,0x1d,0xf8,0xd3,0x11,0x8f,0x98,0x8b,0x31,0x92,0x90,0xb1,0x41,0x44,0x75,0x3f,
  0xec,0xf0,0x39,0xde,0xf5,0x1c,0xec,0x84,0xc2,0xea,0xf6,0x49,0xd0,0x2d,0x3b,0xef,0x6d,0xdf,0x3e,0x77,0xfc,0x32,0x7d,0xdf,
  0x70,0x40,0x23,0x34,0xbf,0x7d,0x7f,0xd8,0x26,0x43,0xf9,0xf7,0xb1,0xb2,0x20,0x4c,0x5e,0x45,0xaa,0x83,0x5c,0x97,0x5a,0x7b,
  0x69,0xa9,0xdc,0xce,0xff,0xdd,0x33,0xe6,0xca,0x99,0xe2,0xc4,0xab,0x58,0x66,0x8f,0xde,0xa8,0xa1,0x1d,0xbe,0x51,0x6c,0xec,
  0x7e,0xe3,0x90,0x26,0x82,0x41,0xd1,0x6b,0x5a,0xb3,0x10,0x26,0xdc,0xf2,0xbb,0x97,0xa2,0xe1,0x37,0x4d,0xff,0xb6,0x82,0x4b,
  0xb2,0x05,0x3b,0xb5,0x59,0xcb,0x9a,0x68,0x2b,0xa6,0x34,0x68,0x2a,0x7a,0x44,0x1a,0xc0,0x9b,0x94,0x28,0x34,0xca,0xe0,0x47,
  0x08,0x44,0x33,0x02,0x4b,0xe1,0x27,0x9c,0x18,0xb1,0x3a,0x7c,0xd8,0xea,0x4a,0xd2,0xb8,0xea,0x35,0x6a,0xc2,0x39,0xcb,0xa3,
  0x0c,0xca,0xee,0x26,0x94,0x5b,0xdd,0xe8,0x12,0x20,0x0a,0xe7,0x90,0xca,0xe3,0x8e,0x1c,0xa7,0xde,0x7e,0xe9,0xb2,0x5a,0xd2,
  0xe0,0x74,0xff,0xaf,0xef,0xd8,0xff,0x9b,0xbd,0xf5,0x53,0x13,0x2d,0xd7,0x11,0x2d,0x2c,0x5d,0xa7,0xe0,0x6b,0x24,0x0f,0x97,
  0xba,0x12,0x68,0xca,0x52,0x5b,0x6e,0x9c,0x45,0xde,0x77,0x23,0x5f,0x7e,0x8e,0x1f,0xbb,0xad,0xd8,0xed,0xb6,0x7b,0x39,0xcc,
  0x32,0x6e,0xaa,0x1e,0x8b,0x91,0xbd,0x8f,0x90,0xf4,0x70,0x11,0x7b,0xa8,0xc7,0xa8,0x94,0x1b,0xdd,0xe5,0x20,0x2a,0x35,0x43,
  0xd9,0xc8,0x37,0xd4,0x9c,0x93,0xd8,0x9a,0xed,0xb4,0xe0,0x5d,0x36,0xc8,0xfc,0x14,0x1a,0xd5,0xae,0xca,0x3a,0x40,0xc8,0xfc,
  0x7a,0xe7,0x33,0x65,0x57,0xe2,0x67,0x27,0x86,0x77,0xa6,0x25,0x4e,0xae,0x0e,0x68,0x90,0x07,0xec,0x9e,0xa5,0x99,0xd7,0x54,
  0xca,0xd6,0xe8,0x7f,0x4c,0xab,0x67,0xff,0xec,0x0c,0x30,0x97,0x25,0x64,0xc0,0xc2,0x52,0xe4,0x56,0x8d,0x45,0x07,0x61,0x29,
  0xda,0x1d,0x69,0x77,0xb3,0xf0,0x18,0xec,0x22,0x24,0xcc,0x93,0xc8,0x48,0x07,0x38,0x25,0xfc,0xf1,0xb7,0x87,0x41,0xcf,0x11,
  0x33,0x75,0xbe,0xae,0x72,0x8d,0x84,0xcb,0x0a,0x0a,0x0a,0x41,0xa1,0x5a,0xdd,0xe7,0xec,0x52,0xfd,0x21,0xc9,0xbc,0xd9,0x70,
  0xc9,0xaa,0xa3,0xc6,0x73,0xce,0x43,0x3b,0x82,0x4e,0x11,0x5b,0x3f,0x07,0x51,0xed,0x57,0x44,0xd5,0x5a,0x71,0x9b,0x87,0x14,
  0x63,0x20,0xbe,0xcc,0xc1,0xdc,0x8d,0xe0,0xd0,0x87,0xbd,0x09,0xb2,0xac,0x7d,0xcf,0x01,0x81,0x25,0x34,0x37,0x43,0x59,0x5a,
  0x06,0x80,0x61,0x6f,0xc5,0xd9,0x86,0x40,0xa7,0x17,0xc7,0x52,0x38,0xf1,0x9e,0xe7,0x62,0xb6,0x57,0x41,0xc2,0xbb,0xe8,0x06,
  0xab,0xb7,0x72,0x5c,0xe7,0x98,0x5d,0x08,0x15,0x23,0xb7,0x3a,0xae,0xed,0xb3,0x14,0x8d,0x56,0xc1,0xec,0x33,0x4d,0x6b,0x5f,
  0x9a,0xc8,0x51,0x4e,0xc3,0x45,0x28,0x4d,0x5d,0xa6,0xc6,0xfc,0xce,0xb5,0x99,0x3a,0xfe,0xd8,0xc1,0xd3,0x26,0x5a,0x9a,0x68,
  0x6f,0x6c,0xec,0xc3,0x0f,0xf2,0xf8,0x9c,0x59,0x51,0xf3,0xa3,0x1a,0x4a,0xc3,0xc3,0x48,0xdf,0x6b,0xe4,0xb5,0xe1,0x98,0xa3,
  0x60,0xa2,0x5a,0xbb,0x90,0x03,0x4a,0xb2,0x7b,0x69,0x11,0x45,0x33,0x62,0x74,0x68,0x9e,0x08,0x3a,0xf9,0x5e,0x3c,0xc1,0x02,
  0xc5,0x54,0xca,0xab,0x60,0xe1,0xc1,0xb4,0x9a,0x48,0x20,0xb0,0x11,0xe4,0xb5,0xac,0x9e,0x8e,0xa4,0xc6,0xe3,0xa5,0xdf,0x4a,
  0x81,0xae,0xef,0xd5,0x49,0x27,0x0a,0x1f,0x6d,0xaa,0xbe,0xe7,0x31,0xef,0x52,0xf1,0x9c,0xaf,0x0a,0xf0,0x90,0xd9,0xb0,0x6b,
  0x56,0x27,0x59,0xfd,0x40,0x22,0x2b,0xad,0xe7,0x02,0x26,0x40,0x1b,0x28,0x34,0xa3,0xd9,0x43,0x36,0x82,0xfa,0x34,0x83,0xa5,
  0x68,0x73,0xe6,0x63,0xe5,0x30,0x6e,0xd6,0xc0,0x69,0x1e,0xe4,0xe3,0xa7,0xd5,0x02,0x68,0x17,0x38,0x25,0x7c,0xe0,0x6e,0xf6,
  0xb7,0xbe,0xed,0xed,0xbd,0x24,0x62,0xc8,0x7d,0x76,0x04,0x3b,0xfc,0xf7,0x41,0x97,0xe3,0x72,0x6f,0x45,0x33,0x96,0xbf,0xc7,
  0x0b,0x3b,0xb2,0xf1,0x3d,0x6d,0x40,0x85,0x98,0x21,0x8b,0xe0,0x31,0x47,0xa0,0x2b,0x79,0xa3,0xff,0xd1,0x72,0x19,0x52,0xda,
  0xb6,0xba,0x0e,0xd3,0x95,0x8e,0x7a,0xb3,0xce,0x19,0x9c,0x4c,0x47,0xec,0x1d,0x45,0x6f,0x4d,0x66,0x93,0xa6,0x6c,0x9c,0xa2,
  0x59,0xca,0xf3,0xd0,0x72,0x39,0xa2,0x14,0x4a,0xc4,0xc5,0x07,0xa7,0xf8,0xf8,0xfb,0x39,0x5b,0xda,0xe8,0xea,0x63,0x33,0x54,
  0xf5,0x60,0xe3,0x32,0xea,0x00,0x2b,0xb7,0xa1,0x08,0x29,0x42,0x8d,0xc8,0x6b,0xa8,0x65,0x66,0xfd,0x76,0xf7,0x29,0xc8,0x4a,
  0xcf,0x21,0xe6,0x67,0x41,0xee,0x78,0x94,0x3d,0xfd,0x56,0xc8,0xd6,0x76,0x3a,0x8b,0x01,0x73,0xfd,0x20,0x97,0xb3,0x33,0x57,
  0xa5,0xaf,0x84,0x36,0x36,0xa0,0xdc,0x7c,0x45,0x24,0xc3,0x5d,0x14,0xe4,0xe8,0x06,0x92,0x99,0x08,0x8f,0x18,0x4e,0xa4,0x62,
  0x64,0xa1,0x96,0xdb,0xfb,0x1d,0x36,0xd4,0xa2,0xee,0xd8,0x63,0xf9,0xab,0x47,0xd4,0x81,0x0e,0xdb,0xe7,0x73,0x1c,0xe1,0xc9,
  0x54,0xac,0x69,0x27,0x32,0xdf,0x90,0x90,0x47,0x83,0x41,0x7e,0xd8,0xee,0x68,0x06,0x94,0x5d,0xd4,0x14,0x38,0xe8,0x84,0x7b,
  0xe5,0xd5,0x67,0x26,0x80,0x79,0xd4,0xf0,0x71,0x43,0xae,0x41,0xcd,0x34,0x6e,0x04,0x47,0x10,0x51,0x43,0xf0,0xd4,0x9b,0xaf,
  0xad,0xa1,0xdc,0xab,0x92,0x00,0xef,0xae,0x4c,0x8a,0xfa,0x0b,0xcb,0xef,0x01,0x62,0xf2,0x52,0xdf,0x9c,0x22,0x58,0x89,0xcb,
  0x48,0x0d,0x49,0x7d,0x39,0x7a,0xfa,0x5b,0x67,0x49,0xd2,0xe5,0x67,0xaf,0x9b,0x1d,0xac,0x2e,0x6c,0x46,0x70,0xba,0x84,0x8b,
  0xc2,0xe9,0x72,0x40,0x43,0x2a,0x14,0x9f,0xa4,0x28,0x32,0x3f,0x74,0x84,0xa7,0x35,0xac,0x1c,0xea,0x56,0x7e,0xca,0x3e,0xfc,
  0xcd,0x81,0xdf,0x49,0x25,0x44,0x8a,0x62,0x9c,0x66,0xc4,0x93,0x9c,0x87,0xd7,0x62,0xee,0xf8,0x28,0xde,0xfd,0xd8,0x85,0x9d,
  0xdf,0x90,0x8b,0x9e,0x80,0x15,0x46,0x02,0xfb,0x6d,0xdc,0x6f,0xcf,0x4b,0xcb,0x70,0xfc,0xa1,0x36,0xd7,0xa2,0xb1,0xbd,0xa1,
  0xa8,0xef,0x08,0x0e,0xcb,0xe2,0xf9,0x8b,0xd6,0x92,0x77,0xe4,0x13,0x47,0x57,0x9f,0x18,0x30,0xc2,0x40,0xac,0x35,0xa1,0xb3,
  0x37,0xda,0xe5,0xfe,0xf6,0x29,0x85,0x62,0x46,0x92,0x9f,0x9d,0x02,0x60,0x94,0x1e,0xce,0x94,0x45,0x5e,0x25,0xc0,0x75,0x7e,
  0xe3,0xb2,0xa4,0xbc,0x1b,0x34,0x78,0x0a,0x1b,0xd8,0xaa,0x4e,0x6e,0x6f,0x2d,0x9e,0x25,0x6c,0x72,0x4a,0x52,0x82,0x6f,0x2d,
  0x34,0xb5,0x1f,0xa2,0x1c,0x2f,0xeb,0xed,0x24,0x8b,0x93,0x58,0xa2,0xd5,0xa6,0xc7,0x44,0x57,0xcb,0x29,0xd9,0x84,0x45,0xc0,
  0x61,0x86,0xab,0xa1,0xc6,0x89,0xec,0xfd,0x8f,0xd5,0xe8,0x75,0x45,0x36,0xb1,0x8e,0xaa,0xb7,0x9a,0x79,0x8d,0x7c,0x63,0x72,
  0x19,0xee,0x87,0x6e,0x45,0xbb,0xec,0x66,0xbd,0xf3,0x75,0x2d,0xc9,0x18,0x04,0xce,0xfb,0xff,0xd6,0xf1,0x2a,0x58,0x20,0xe7,
  0x85,0x71,0xcd,0xce,0x3f,0xc5,0xdf,0x35,0xf9,0x23,0xb5,0x56,0xaf,0xa1,0xc5,0x1a,0x81,0x34,0xe8,0x43,0xa2,0x64,0xff,0x70,
  0xb9,0x2b,0x35,0xd8,0x29,0x57,0x6a,0xe4,0x3f,0xd5,0x24,0xe8,0x1f,0x20,0xed,0x18,0xf9,0xb8,0x4d,0xd4,0x47,0x0e,0xa1,0xbf,
  0xa7,0xfe,0x55,0xc7,0xce,0xf6,0x52,0xc0,0xcc,0x6b,0x77,0x80,0x5f,0xb2,0x27,0xaf,0x19,0x93,0xf9,0x51,0x24,0x60,0x85,0xa8,
  0x83,0x10,0xfd,0x46,0xba,0x18,0x34,0x63,0xba,0x1d,0xe2,0xcd,0xb0,0xd2,0x5f,0xd2,0x15,0x46,0x9e,0xf4,0x74,0xe2,0xad,0xa3,
  0xf9,0x58,0xe3,0xcf,0x60,0xf1,0xc1,0x94,0x7c,0x2b,0x43,0xe0,0x56,0x40,0x05,0x34,0xdf,0x9d,0xfa,0x09,0xf2,0x02,0xb6,0x3d,
  0x7b,0xfb,0xfe,0x17,0x6e,0x47,0x31,0x5e,0x0f,0x8f,0xfd,0xab,0x6a,0x31,0xe3,0x92,0x67,0x29,0x05,0x93,0x27,0x89,0x9d,0x12,
  0x74,0x51,0xf9,0x5d,0x60,0xdb,0xcb,0x77,0xb3,0x98,0xbd,0x63,0x97,0xc4,0xdc,0x11,0xf0,0x0c,0x46,0xc1,0x7c,0xf8,0x00,0x3c,
  0x4d,0x54,0x64,0x3e,0x9d,0x86,0x6b,0x17,0xc3,0x70,0x43,0x2e,0xd1,0x22,0x28,0xc3,0x86,0x89,0xfb,0xba,0x28,0x75,0x16,0x23,
  0x33,0x76,0xb4,0x9c,0xe5,0xe7,0x8a,0x3b,0xc8,0x36,0x53,0x80,0x5b,0x42,0x79,0x18,0x2e,0x84,0xb6,0xbe,0xf1,0x2b,0xa4,0xb1,
  0x05,0xe4,0x08,0x40,0x7b,0x99,0xa4,0x76,0xcb,0x36,0x90,0x1d,0xec,0xbd,0x0a,0x4f,0x4b,0x11,0x5b,0x8c,0x9a,0x60,0xe1,0x28,
  0x00,0x60,0xa3,0xdb,0x34,0x28,0xfa,0x86,0x39,0x88,0x1d,0x23,0xe5,0x9c,0x26,0x66,0xf0,0x77,0xf8,0x6b,0xc0,0xbc,0x40,0xbb,
  0xf5,0x31,0x83,0x93,0x5d,0x4f,0x35,0x6e,0x99,0x61,0xa0,0x73,0x88,0x9d,0x1b,0x7b,0x6f,0x4b,0xe8,0x6c,0x39,0x31,0xe1,0x4d,
  0x08,0x91,0x0c,0xd2,0x1f,0xa2,0xf7,0x95,0x2a,0x00,0xcf,0xed,0x62,0x91,0xa6,0x87,0xdc,0x3c,0x3d,0x8c,0xe8,0x3c,0x49,0x11,
  0x6e,0x78,0x56,0x5f,0xa6,0xe7,0xc0,0xf2,0x90,0xfb,0xb8,0x20,0xf7,0xff,0xce,0x55,0x43,0xa3,0x8d,0xa2,0xa3,0x74,0xf0,0x20,
  0xbd,0xe9,0xb4,0x89,0xf7,0x9c,0xdd,0xbe,0xe1,0x6c,0xd7,0xec,0x9e,0x6c,0xb4,0x83,0x35,0xc8,0x16,0x80,0xc3,0x92,0x0c,0xe3,
  0xef,0x7f,0x26,0x11,0xbf,0xa5,0xf0,0x0f,0x47,0x3a,0x35,0x91,0xfe,0xeb,0x34,0x85,0xc5,0xca,0x87,0x2d,0xf7,0x9c,0x80,0x85,
  0x98,0x37,0x48,0xc5,0xf3,0x06,0x00,0x7d,0x27,0xe0,0x34,0x45,0xc6,0x62,0x2e,0x13,0xce,0x5c,0xc1,0x97,0xc0,0xc9,0x4d,0xa7,
  0x90,0x5a,0x2d,0x43,0x88,0x4b,0x97,0xa4,0xe1,0x1e,0x9c,0xa0,0x7e,0xba,0x6c,0xed,0xf1,0x1b,0xd7,0x50,0xb0,0xb6,0x0d,0x20,
  0x3a,0x91,0xf1,0xce,0x71,0xe2,0x69,0xee,0xed,0xd8,0x59,0xb5,0x6b,0x79,0x2b,0x52,0x73,0xbd,0x51,0xd2,0x13,0xbb,0xa7,0x61,
  0x0e,0x02,0xc1,0xee,0x5f,0x9d,0xb5,0xf1,0xeb,0x40,0x81,0x21,0xca,0x8b,0x2a,0xd6,0x23,0xde,0x91,0x93,0xbd,0x9f,0xc1,0x87,
  0x3e,0xcd,0x89,0xcf,0xd2,0x48,0x0d,0x6b,0x38,0x5f,0x4e,0x4b,0xdf,0x08,0x8b,0x63,0x19,0xd4,0x4a,0x95,0x59,0x0f,0xb4,0x0c,
  0x90,0x8f,0x76,0x55,0x4e,0x61,0x38,0x84,0x08,0x6a,0xc5,0xad,0xe4,0xd9,0x14,0x62,0xbd,0xf9,0x55,0x04,0x73,0x27,0xc4,0x83,
  0x84,0xc7,0x32,0xa8,0xcd,0x1b,0xa0,0xae,0x0d,0xd1,0xc1,0x8e,0x4a,0xb8,0x3c,0x13,0xab,0x5c,0x96,0xda,0x23,0x73,0x67,0x73,
  0x91,0x94,0x60,0x29,0xc0,0xed,0x78,0x89,0x39,0x8d,0xc4,0x2a,0x0a,0x30,0x31,0xf0,0x96,0x17,0xa2,0x35,0x97,0xb3,0x6b,0x29,
  0x0d,0xf6,0x2a,0x1f,0xc8,0x8c,0xe0,0x83,0x76,0x43,0xfe,0x2b,0x02,0xab,0xa3,0x1e,0x92,0xf2,0xb8,0x95,0x14,0x0e,0xeb,0x9e,
  0x5a,0x53,0x48,0x15,0x45,0x23,0xeb,0x9f,0xe0,0x6b,0xba,0x64,0x75,0x7f,0xf1,0x88,0xba,0x88,0xc8,0x16,0x3a,0x17,0xdf,0x84,
  0x80,0x0b,0x29,0x3b,0x7d,0x7d,0xbd,0xdf,0xe1,0xea,0x53,0xf1,0x69,0x4d,0x49,0x8a,0x8e,0x46,0x3e,0x25,0x6c,0x40,0x79,0x2e,
  0x3f,0x31,0xf3,0x25,0x66,0xf8,0x22,0x43,0xf2,0x41,0xfb,0x73,0xd2,0xd5,0xf1,0xf1,0x15,0x92,0xf5,0x47,0xaf,0x08,0x67,0xe1,
  0xbd,0x7f,0x7e,0x73,0xa1,0xde,0xd1,0xf1,0xc1,0x62,0xc0,0xd7,0x09,0x40,0xd7,0xfa,0xc5,0xfa,0x73,0x9e,0xc4,0x00,0x9e,0x78,
  0x33,0x43,0x25,0x93,0x2b,0xa4,0x4b,0x21,0xbc,0x5b,0xab,0x71,0x0b,0x91,0xc6,0x21,0xef,0xcf,0x9e,0xae,0x5e,0xf1,0xe7,0x66,
  0x32,0x95,0x35,0x60,0xd8,0x36,0x47,0xb0,0xe9,0xa7,0xd2,0xd4,0x22,0xe3,0x8a,0x27,0x8a,0x58,0xe0,0x34,0x2d,0xe0,0x8c,0x83,
  0xba,0xa6,0x96,0xff,0x27,0x69,0xeb,0xeb,0x93,0xf6,0xa3,0xfc,0xe6,0x6b,0x4f,0x0d,0x40,0xc8,0x01,0x12,0xea,0x0d,0x64,0x72,
  0x00,0xec,0xe0,0x11,0xb4,0x89,0xda,0x19,0xe7,0xb8,0xc8,0xd1,0xb4,0xfc,0x77,0x7a,0x47,0xef,0x8b,0x0e,0x55,0x60,0x4d,0x4c,
  0xd9,0x4a,0xde,0xa4,0xd3,0x0e,0x61,0xe1,0xf3,0x7b,0x1b,0xf1,0xca,0xeb,0x55,0x81,0xdc,0xbb,0xe0,0xbe,0x78,0x0f,0xe3,0x0f,
  0xbf,0x84,0xf9,0x7d,0xff,0x9e,0xd3,0x9e,0x08,0xfd,0x35,0x8c,0xd7,0xae,0x4d,0xa0,0x17,0xb6,0xda,0x51,0xf4,0x2c,0xc5,0x61,
  0x92,0xa7,0x13,0x7e,0x33,0x49,0x2d,0x9d,0x30,0x91,0x57,0x38,0xe4,0xa3,0x95,0x75,0x05,0x84,0xa8,0xee,0xd6,0xbf,0x1f,0xb0,
  0x12,0x24,0xc8,0x42,0x7a,0xc1,0xf2,0xdd,0x67,0x57,0x33,0x80,0x60,0x4d,0xc3,0xd9,0xae,0x49,0x04,0x53,0x4c,0x0f,0xe1,0x5a,
  0x9f,0x0a,0xd0,0x79,0x0b,0x07,0x65,0x66,0x0d,0x18,0x09,0xb9,0xba,0xb2,0x46,0xc5,0x5a,0x0e,0x27,0x1d,0xd5,0xcd,0xaa,0x6b,
  0x3e,0xd0,0x18,0x55,0xf2,0xfb,0x8f,0xaa,0x8e,0x46,0x7c,0x9e,0x43,0xd9,0xb5,0x5f,0x34,0x20,0x8d,0xfe,0x68,0x68,0xa2,0x9f,
  0x4a,0x9c,0x55,0x11,0x73,0x74,0x48,0xa4,0xe7,0xe2,0x03,0xf0,0xdf,0x8e,0xd4,0x64,0x48,0xea,0x7e,0xde,0x7f,0x1f,0xb9,0x26,
  0x59,0x61,0x23,0xa1,0x14,0x2a,0x35,0x30,0x11,0x33,0x23,0x3b,0x63,0x21,0x80,0xa5,0x4d,0xad,0x68,0x77,0xa5,0x38,0xcc,0xa3,
  0x00,0xb8,0x18,0x65,0x10,0xc8,0xed,0x5d,0x7a,0xdb,0x3f,0xd8,0x51,0x49,0x59,0x69,0x7a,0x35,0x9d,0xd5,0xca,0xf7,0x8a,0xbc,
  0x30,0x9f,0x72,0x29,0xdb,0xbb,0x73,0x24,0xa7,0xbc,0x88,0xad,0x93,0x3d,0x36,0xf0,0x81,0xe6,0xde,0xe3,0xbe,0x96,0x8e,0x39,
  0x13,0x18,0x49,0x53,0x37,0x51,0x76,0x5e,0xe4,0xc3,0x80,0xa3,0xfb,0x08,0x56,0x80,0x8d,0xac,0x83,0x02,0x60,0x2e,0x57,0x06,
  0xf0,0x07,0x96,0xcc,0x03,0x5a,0x7f,0xd6,0x9a,0x00,0xcd,0x23,0x94,0x87,0x0c,0xb8,0x53,0x36,0x6d,0xad,0x78,0x64,0x2b,0xf9,
  0x47,0xac,0x4c,0x90,0x80,0x66,0xea,0xf6,0x54,0x3c,0x5c,0x0a,0x5a,0x9d,0xd2,0x6d,0xd2,0xfd,0x8b,0x6e,0xd4,0x38,0x01,0x9c,
  0xe9,0x03,0x75,0x7c,0x05,0x4e,0x7e,0xf2,0xfa,0xbe,0xb2,0x2b,0x4f,0x10,0xe3,0x98,0x50,0x7a,0xa2,0x47,0xfd,0xb4,0x5c,0x87,
  0x18,0x0b,0x79,0x31,0x4b,0xb3,0xe5,0x08,0xa6,0xb5,0xb0,0xbf,0xc5,0xf3,0x09,0x55,0xfa,0x36,0x0a,0xe0,0x37,0x64,0x4d,0xd7,
  0xd8,0x0a,0xeb,0x30,0xa1,0x0b,0xad,0x5a,0xb0,0x7b,0x1a,0x3c,0x1d,0x9f,0x7f,0x1b,0xe8,0x3d,0x6d,0x32,0x86,0x38,0xb6,0x63,
  0xae,0x96,0xe0,0xa1,0xab,0x5b,0x09,0x52,0x03,0xcd,0x3f,0xfa,0x1b,0x80,0xcf,0x3c,0x34,0x74,0x95,0xc3,0xdc,0xaf,0x0c,0xfa,
  0xbe,0x5e,0x3b,0xa6,0xbb,0x21,0xa9,0xe1,0xa2,0x09,0x87,0xf9,0x0e,0x17,0x72,0x88,0x46,0xbe,0xe1,0x4a,0x3c,0xda,0x4e,0x1b,
  0xa6,0x9c,0x03,0x0b,0xb6,0x30,0x05,0x1b,0x6e,0x65,0x39,0x7e,0xc2,0xdb,0x61,0x72,0xd7,0xaf,0x57,0x07,0xf0,0xd8,0x3e,0x7a,
  0x27,0x2e,0x5e,0x5a,0xcd,0xba,0x76,0xcd,0xf4,0xe3,0x10,0x9a,0x22,0x14,0xc6,0x93,0x21,0x02,0xad,0xac,0x4d,0x22,0xdf,0x63,
  0x02,0x42,0xd3,0x11,0xb5,0x81,0x1f,0x41,0x40,0x51,0x6d,0x7e,0x0d,0xcf,0xd8,0x33,0xf0,0xb2,0x0f,0x6f,0x4c,0x20,0x71,0x8a,
  0x6a,0xff,0x6b,0xf9,0x23,0x34,0x37,0x71,0x2a,0x58,0x85,0x99,0x50,0x47,0x69,0x12,0x34,0x88,0x62,0x20,0x9a,0x6d,0x4a,0x59,
  0x10,0x46,0x45,0x5e,0xdc,0xd5,0x32,0x50,0x7f,0x9f,0x29,0xcd,0x5c,0x05,0x77,0xda,0xc3,0x6e,0xe0,0x8c,0x8a,0x37,0x10,0x78,
  0x36,0x97,0xc8,0x0f,0x27,0xd4,0x47,0xfb,0x8f,0xea,0xfa,0xf3,0xef,0xf0,0xef,0x36,0x7e,0x59,0xfa,0xe8,0x6c,0xaa,0x43,0x0c,
  0xf2,0x1f,0x02,0x6c,0x26,0x97,0x74,0xde,0xf5,0x67,0x9c,0xe6,0xb1,0xe2,0xac,0x13,0xa7,0x9a,0xc3,0xbe,0x1b,0x02,0xcb,0xc0,
  0x8d,0x76,0xb3,0x4d,0x58,0x6f,0xc7,0xa0,0xf1,0x49,0x36,0x62,0xaa,0x46,0x1a,0x62,0xfd,0xec,0x9c,0x21,0x6d,0x29,0x62,0x04,
  0xa8,0x59,0x76,0x36,0xb1,0x19,0x9b,0xf1,0x21,0x1c,0x0f,0x3c,0xa7,0x32,0x23,0x33,0xf8,0x62,0xce,0x79,0x80,0x3f,0x15,0x0e,
  0x3b,0x2c,0x68,0x6f,0x0d,0x17,0x16,0x32,0x8a,0x60,0x62,0xa7,0x0e,0xb8,0x17,0x73,0x8e,0xcd,0x66,0x7e,0x30,0xc9,0x4f,0xac,
  0x24,0x9e,0xf2,0x94,0xa5,0x67,0x61,0xec,0x7d,0x57,0x45,0xf5,0xea,0x89,0x37,0xda,0x3e,0x63,0x56,0xd5,0x88,0x51,0xe4,0x18,
  0xe8,0x38,0x45,0x43,0x6a,0xb1,0x9b,0xf4,0x1c,0x43,0x8c,0xcb,0x35,0x57,0xb9,0xba,0xec,0x75,0x1b,0xe7,0xab,0x82,0xcf,0x12,
  0x85,0xb4,0x0a,0x06,0xb1,0x6a,0x4b,0x75,0x88,0x7d,0xad,0xe8,0xa4,0x05,0x30,0x59,0xf1,0x03,0xbf,0x55,0x4a,0xdb,0xf2,0x79,
  0xe5,0xce,0xa7,0x6b,0xa1,0x46,0xcf,0x87,0x36,0x71,0x88,0x7c,0x46,0x88,0xd3,0x46,0x1c,0x2e,0x98,0xc6,0x14,0x56,0x27,0x2f,
  0xb0,0xb6,0x55,0x7c,0x5b,0xb3,0x9f,0xed,0xff,0xca,0x22,0xd6,0xff,0x1d,0xb6,0x96,0x1a,0x9a,0xd1,0xe3,0xbc,0xa9,0x29,0xb8,
  0x37,0xce,0xfb,0xb1,0x15,0x9b,0x89,0x7e,0x86,0x4b,0xc8,0x9d,0x3a,0xff,0xe9,0xd1,0x07,0x9b,0x01,0xb7,0x94,0x09,0xdd,0x82,
  0x56,0x0f,0x6f,0xaf,0x01,0x0b,0x8b,0x74,0xc3,0xc2,0x46,0x2c,0x25,0x7b,0x53,0x62,0x7c,0x76,0xae,0x8f,0x7d,0x5f,0x85,0xfa,
  0x99,0x79,0x6d,0xa6,0x99,0xc9,0x37,0xa7,0x99,0xd1,0x27,0x2b,0x22,0x92,0x18,0x88,0xdf,0x30,0xc1,0x9c,0x34,0xde,0x6f,0x73,
  0xd9,0x22,0x59,0x97,0x0b,0x5c,0x18,0x2a,0x06,0x75,0x63,0x8e,0xed,0x5d,0xe2,0x5a,0xcf,0x2b,0x08,0x4a,0xf1,0x2e,0x22,0xb4,
  0xa8,0x13,0x5b,0x56,0xdd,0x09,0x74,0x56,0x98,0x05,0x3f,0x6a,0xae,0xb6,0xb0,0x78,0x10,0x52,0x09,0x7f,0x93,0x78,0x61,0x9f,
  0x63,0x8c,0x25,0xc5,0x4e,0xa5,0x24,0xba,0xe1,0x85,0x82,0x5d,0x96,0x9d,0xa9,0x77,0xdb,0x15,0xc1,0xb0,0x96,0x47,0xc1,0x7d,
  0xdd,0x32,0xe2,0x2e,0xc9,0x2f,0x3f,0xbd,0x91,0xd8,0xbb,0x50,0x13,0x45,0x94,0x84,0xa0,0x91,0x7c,0x18,0x38,0xd8,0x87,0xee,
  0x0c,0x84,0x14,0xa6,0xad,0xe2,0xca,0x48,0x78,0x1a,0x32,0xb8,0x8d,0xc4,0xa2,0xad,0xce,0x04,0x73,0xfb,0x2a,0xd4,0xe8,0x9d,
  0x5c,0xa0,0x82,0xd0,0x5d,0x7e,0x34,0x6b,0x18,0xa1,0x78,0xbf,0x8b,0x68,0x0c,0x6b,0xda,0xfb,0xc6,0xc8,0xdf,0xea,0x6e,0x95,
  0x4d,0x20,0x7e,0xd3,0xfe,0x91,0xac,0xa0,0xbc,0x0f,0x34,0x9a,0x39,0x83,0xda,0xda,0x7f,0xc8,0x4b,0xb3,0xaf,0xd2,0x93,0xaf,
  0xd7,0x4d,0xc4,0x22,0xb6,0x1c,0xa0,0x5e,0x77,0x83,0xa7,0xef,0x88,0x27,0x84,0xd4,0x80,0xe3,0x5d,0xeb,0x3a,0xc1,0xce,0x10,
  0xb2,0xf0,0x1f,0x53,0x55,0xbf,0xa9,0x83,0xe7,0xe3,0xee,0xbf,0xc0,0x34,0x00,0x52,0x50,0x32,0x97,0x14,0x3f,0xdc,0x1e,0x97,
  0xa1,0x06,0xd9,0x5a,0xde,0x45,0x97,0x2d,0xa0,0x6d,0xcb,0xd4,0x79,0xf1,0xf5,0xb0,0x4e,0x7a,0xdb,0x6b,0xdb,0xbe,0xcc,0xd7,
  0x44,0xb5,0x33,0x5e,0xf6,0x6b,0xe1,0x96,0x25,0xed,0x26,0xf1,0xd5,0x74,0xa8,0x77,0x22,0x6a,0x16,0x48,0x35,0xbc,0xcc,0x03,
  0xb6,0x56,0xa6,0xac,0x2e,0xc1,0x40,0x3a,0x82,0xbc,0x3d,0x56,0xde,0x0f,0xbf,0xd4,0xbe,0xda,0x42,0x19,0x45,0x5c,0x04,0x05,
  0x33,0xa8,0xb0,0x34,0x44,0xd0,0xb5,0x01,0x00,0xca,0x70,0x3b,0x18,0xc1,0xf1,0x5b,0xf6,0x3a,0x90,0xbf,0x0a,0xb1,0x3f,0xfd,
  0xfa,0x6c,0xbf,0xdf,0x89,0x79,0xa2,0x82,0x5c,0x05,0x18,0xab,0xb3,0xe9,0x24,0x40,0x9a,0x78,0xb1,0xcb,0x6f,0x16,0xe0,0xea,
  0x67,0xdc,0x27,0xb9,0x06,0xda,0x59,0x9b,0xf2,0xd1,0x3d,0x24,0xf1,0xb8,0xad,0x51,0x49,0xcf,0x41,0x50,0xfd,0x35,0xfb,0x95,
  0xeb,0xa1,0x86,0xe3,0x68,0x25,0x90,0x10,0xd9,0x3d,0x25,0x64,0x89,0x12,0xee,0x52,0xdc,0x15,0xd7,0xbd,0xe5,0x09,0x72,0x67,
  0x25,0x8e,0xcb,0x6b,0x16,0x3e,0x16,0x85,0x9a,0x0e,0x2f,0x5b,0x3d,0x62,0xa3,0x69,0x48,0x7b,0x48,0xc0,0x5a,0x4d,0x45,0x9e,
  0x0b,0xe2,0x7f,0x35,0x61,0xd9,0x20,0x49,0xbe,0x68,0x6e,0x19,0x40,0x17,0x61,0x11,0x10,0xd8,0x2f,0xf1,0x2a,0x1d,0xfd,0x9d,
  0xf2,0xf6,0xc2,0x4b,0x70,0xda,0xd1,0x36,0x3b,0x45,0x10,0x0a,0x85,0x8a,0xfd,0xbc,0x60,0x76,0x33,0xfb,0xfc,0x97,0x9f,0xe6,
  0x72,0x98,0xe8,0x9b,0x59,0xd6,0x09,0xa4,0x41,0x8b,0xf8,0xfb,0x84,0x9c,0x34,0xb3,0x06,0xee,0xb5,0xc5,0xee,0x66,0x32,0x10,
  0xf9,0xef,0x9d,0x93,0x2a,0x18,0x55,0xe7,0xdb,0x15,0xb0,0xa2,0xe3,0xa0,0x8c,0x3c,0x7e,0x81,0x8d,0x2a,0xbe,0x57,0x0a,0x77,
  0xc2,0xfc,0x75,0x1b,0x26,0x27,0xf5,0x30,0xef,0x0a,0xd0,0x59,0x11,0xa4,0xc4,0x97,0xae,0xf5,0xa4,0x27,0xb5,0x62,0x05,0xb1,
  0xfe,0xf1,0xf9,0x9c,0x42,0x9f,0x50,0xe7,0x78,0xc7,0xaf,0xb5,0xf6,0x69,0x9d,0x3f,0xa2,0xd3,0xac,0x21,0x0e,0xd2,0xa4,0x09,
  0x9f,0x11,0xfa,0xfa,0x65,0x4a,0x88,0xa8,0x6a,0xe7,0xb1,0xb3,0x50,0xa8,0x88,0xf8,0x3c,0x9c,0xa5,0x61,0x9b,0x0b,0xa5,0x5b,
  0x88,0x3e,0x38,0x31,0x65,0x59,0x02,0x9f,0x22,0x77,0x3c,0x86,0xdf,0x07,0xc8,0x3a,0x61,0x44,0x04,0x5d,0x8f,0x44,0x6b,0x9c,
  0x25,0x0e,0x6c,0x2c,0x99,0xff,0x7c,0x5d,0xe0,0x6e,0xd6,0x13,0xce,0xb3,0x1b,0x97,0x00,0xc6,0xeb,0x54,0x46,0x43,0xa6,0xda,
  0xcf,0x54,0x0e,0xe9,0x75,0x63,0xfe,0x80,0xb3,0x34,0xa8,0x4b,0xc2,0x6e,0x61,0xa7,0x16,0x6e,0x8b,0xd8,0x19,0x61,0xfd,0x6a,
  0x0e,0x39,0xcb,0xd5,0xe8,0xc5,0x2a,0xf3,0x61,0x1d,0xd1,0x8c,0x1b,0x1c,0xc4,0x51,0xba,0xc8,0x34,0x40,0x22,0x41,0x71,0xf0,
  0xd8,0x40,0x13,0xc1,0x69,0x42,0xa6,0xa6,0xb0,0x36,0xdb,0x8d,0x19,0x24,0x92,0x0c,0x7f,0x41,0x68,0x1d,0xc6,0x31,0xd0,0xcc,
  0xab,0xb6,0x17,0xa7,0x7b,0xca,0x82,0xb5,0xd5,0xde,0x0a,0x96,0x52,0x76,0x94,0x47,0x03,0x5a,0xac,0xc9,0x26,0x4d,0x27,0xe8,
  0x27,0xcf,0x1c,0xa3,0x5e,0x98,0xa2,0xa5,0x36,0xaf,0x92,0x82,0x9e,0x30,0x87,0x95,0x7d,0x57,0x92,0x94,0xae,0xa3,0xc3,0x41,
  0xb1,0x7e,0xb0,0x95,0x6d,0xc8,0x4a,0x9d,0xe6,0x20,0xd7,0x03,0x7a,0xf2,0xc6,0xf1,0xd7,0x0f,0xb7,0xa5,0xe0,0x4a,0x27,0x16,
  0xb6,0x0b,0x9a,0x1e,0xb0,0x84,0x1c,0x77,0xb5,0xea,0xab,0xaf,0x49,0x76,0x16,0x46,0x5e,0xa5,0x30,0xba,0x8d,0xe6,0x24,0x51,
  0xa7,0xe1,0x64,0xcf,0x70,0xba,0x36,0x6d,0xac,0x5f,0x57,0xd1,0x54,0x20,0xdb,0x11,0x8a,0xb8,0x0e,0xa0,0x08,0x57,0x7e,0xfe,
  0x79,0x8a,0x8c,0xfe,0xef,0x03,0x03,0xe5,0x8c,0x93,0xfc,0x89,0x16,0x97,0xa9,0xe8,0xd7,0x96,0x43,0xa3,0x51,0x50,0xa8,0x7a,
  0xc0,0xfa,0x30,0xaa,0xef,0xb5,0x99,0x1d,0x6b,0x9c,0xce,0xe7,0x20,0x21,0x1d,0x29,0x80,0xcb,0x46,0xd6,0x37,0x84,0xc5,0x86,
  0xa9,0x32,0x06,0x26,0x62,0xd0,0x5b,0xce,0x5f,0xdd,0xc6,0x04,0x65,0x2a,0xb5,0x61,0x44,0xf9,0x70,0x23,0xa9,0x57,0x35,0xae,
  0xf5,0xe5,0xeb,0xa4,0x60,0x40,0xce,0xdb,0x7f,0x56,0x53,0xd3,0xb6,0xea,0x4f,0xdd,0xbc,0xc5,0xee,0x53,0xa2,0xfb,0x14,0x0d,
  0xbe,0x02,0x3c,0x00,0xb4,0x35,0x3a,0x23,0xce,0xbb,0x11,0x20,0xf0,0x60,0x93,0x52,0xf8,0xf1,0xc3,0xae,0x67,0xd3,0x18,0x28,
  0x4d,0xf1,0x43,0x13,0x93,0xfa,0x06,0xd7,0xdc,0xe3,0x14,0x2e,0xde,0xbc,0x34,0x45,0x27,0x75,0x19,0x56,0xd9,0x26,0xc0,0xca,
  0x06,0x3a,0x20,0x75,0xf0,0x5d,0xb3,0x8c,0x5f,0xba,0xe9,0x84,0xdd,0xcf,0xe0,0x73,0x77,0x15,0x25,0x92,0xa8,0x3d,0xde,0x22,
  0xb2,0xb5,0xf9,0x28,0x89,0x73,0x72,0xe3,0x89,0x43,0x11,0x7a,0x55,0xf2,0xbe,0x12,0x06,0x6e,0x80,0x14,0xc9,0xc7,0xfe,0xa0,
  0x39,0x28,0x94,0xbc,0xfa,0xab,0x5c,0xd5,0xae,0xb1,0xb1,0x18,0x45,0x52,0xe6,0x9b,0xb3,0x16,0x74,0x3a,0xc8,0x41,0x84,0x36,
  0xe1,0xc6,0xc4,0xd5,0x5b,0x57,0x3f,0x53,0x2a,0x2d,0x40,0xb3,0x13,0xf4,0x29,0xf5,0xd9,0xf4,0x48,0x29,0x47,0xb1,0x7a,0x65,
  0x7d,0x8b,0xb1,0x00,0x24,0x89,0xcf,0x56,0x11,0x8c,0xb8,0x89,0x2b,0xc6,0x1a,0xd5,0x63,0x18,0x9c,0x4e,0x3a,0xee,0x02,0x76,
  0x3e,0x9f,0xb9,0xec,0xc2,0x48,0xbe,0xc1,0x7f,0x3b,0x99,0x71,0x1d,0xbd,0x77,0x69,0x83,0x6b,0x68,0x60,0xec,0xec,0x4a,0x2d,
  0x7d,0x14,0xa0,0x96,0xda,0x78,0x11,0x5a,0xc5,0xbd,0x26,0xe7,0xb9,0x58,0xd5,0xaa,0xd8,0xc9,0x17,0x26,0x2a,0x0b,0x67,0x1a,
  0x48,0xe7,0xa2,0xed,0x13,0x34,0x00,0x43,0x68,0xd7,0xa8,0xf7,0x84,0x49,0x0d,0x5c,0x6f,0xe1,0xea,0x6f,0x0f,0xfd,0x6f,0x9d,
  0x5c,0x9e,0x34,0x80,0x78,0xaa,0x43,0xa9,0x61,0xdf,0x19,0x1d,0x7e,0x8e,0xf0,0x80,0xad,0x2d,0x68,0x5c,0x39,0xff,0xb9,0x93,
  0x44,0xf7,0xb9,0xc7,0xe2,0x8d,0x50,0xf3,0xb2,0x34,0x2a,0x6f,0x14,0x04,0xe4,0xa9,0x58,0x37,0xf4,0xe7,0xb0,0x28,0x03,0x19,
  0x38,0x25,0x90,0x75,0xac,0x65,0xc6,0xd6,0x66,0xe3,0xad,0x4b,0xc0,0x9a,0x37,0x3f,0x8c,0xf8,0x38,0x39,0xc5,0x96,0x78,0xa3,
  0xa5,0x85,0xbc,0x6a,0x64,0xbf,0x40,0xdf,0x90,0x1a,0x06,0x38,0xe0,0x56,0x9a,0x4c,0xee,0x43,0xe1,0x91,0x75,0xda,0x1c,0xb0,
  0xae,0xc2,0x9b,0xe1,0x12,0xb8,0x70,0xdb,0xd7,0x68,0xb1,0xdc,0x42,0xf9,0x3d,0xe0,0xa9,0x05,0x9b,0xa8,0xfc,0xa7,0xe0,0x82,
  0xe9,0xde,0xff,0xc2,0xb4,0xf4,0x9a,0x91,0xc0,0x11,0x40,0xd6,0xe6,0x4f,0x00,0x0c,0xbf,0x02,0x49,0x21,0xc1,0xc0,0xf7,0xe0,
  0xa1,0x9e,0xcf,0xd0,0xa8,0x77,0x24,0x0a,0x40,0x35,0x9d,0xea,0x02,0x6b,0xd9,0x62,0x5d,0xca,0xba,0xd5,0x72,0x19,0x8a,0x42,
  0x4a,0x8c,0x41,0x8f,0x79,0xcb,0xae,0x1d,0x8e,0x80,0xab,0x65,0x90,0x2b,0x2c,0xab,0x21,0x62,0xa6,0x17,0x12,0xf0,0x47,0x78,
  0xdd,0x49,0xef,0x7b,0x6d,0xb9,0x2d,0xab,0x7d,0x4f,0x0b,0x3d,0x66,0xfc,0x32,0x83,0x5d,0x36,0xf9,0xcf,0x29,0x04,0xf6,0x74,
  0x7e,0x2a,0xd4,0xe5,0x16,0xac,0xd0,0x19,0xd8,0x0d,0x50,0xdc,0x61,0x8d,0x5e,0xb7,0xe4,0x86,0x1d,0x20,0xc9,0x20,0x87,0x99,
  0x83,0xaf,0x3b,0xd9,0xb7,0xa5,0x19,0x74,0xfe,0x69,0xc5,0x10,0x38,0xb4,0x47,0x65,0x07,0x85,0x4f,0xce,0x54,0xfc,0x10,0x3d,
  0x4e,0xbb,0x5d,0x0a,0xc7,0x2e,0xbf,0xf2,0x79,0xab,0xe4,0x57,0x69,0x67,0x08,0xe4,0xd2,0xfa,0xc3,0x3c,0xb4,0xdf,0x12,0x52,
  0xab,0x8b,0x42,0xa6,0xd4,0xc8,0x4a,0xe8,0xa2,0x46,0x07,0xaa,0x5a,0x9b,0x80,0xd0,0xc8,0xe9,0x80,0xdb,0x0e,0x20,0x9f,0xa4,
  0x0c,0xf2,0xee,0x86,0x62,0x0b,0x88,0x9d,0xfa,0xf9,0xb3,0xd7,0xe5,0xa6,0x86,0x31,0xac,0xe6,0x40,0x35,0x5e,0x26,0x76,0xce,
  0xc1,0xf0,0xdd,0xfb,0x36,0xe7,0xe5,0xef,0x83,0x1f,0x71,0xa9,0x6a,0xa2,0x63,0xb1,0x47,0x77,0x30,0x2a,0xce,0x2a,0x5e,0xb5,
  0x65,0x40,0x95,0xce,0xd3,0x7d,0x0d,0x66,0x45,0xfb,0x32,0x86,0xaa,0x00,0xc4,0xed,0xc1,0xe7,0xf2,0x62,0xa3,0x26,0x62,0x3c,
  0x4f,0x64,0x05,0x5e,0x63,0xdc,0x9c,0x70,0x35,0x95,0x11,0xe3,0x6b,0x93,0x0b,0x16,0xc7,0x33,0x70,0x34,0xb2,0x47,0x41,0x45,
  0xab,0xfa,0xa6,0x25,0x00,0x8f,0xf3,0x5b,0x28,0x3c,0x3d,0x03,0xe6,0x4c,0x23,0x5b,0x5e,0x5b,0x00,0xe0,0x91,0x5c,0x2d,0x4e,
  0x8d,0x9f,0xab,0x3b,0xe9,0xe6,0x78,0x51,0xe5,0xfc,0xb9,0xe2,0x81,0xcd,0x8b,0xcd,0x2b,0xda,0xeb,0x8d,0xb2,0xee,0xfb,0x87,
  0xea,0x00,0x36,0xcf,0x6b,0xca,0xe2,0xf8,0xa0,0x24,0x97,0x82,0x55,0x2e,0xa5,0xce,0x56,0x40,0x1b,0x58,0x42,0x50,0x17,0xb6,
  0xea,0x34,0xbc,0x47,0x0a,0x12,0x77,0xc4,0x8a,0x04,0xc3,0xbc,0x37,0x1f,0x4b,0xe2,0xa5,0x29,0x8c,0x70,0x14,0xe6,0x43,0xfa,
  0xc7,0xcb,0xcc,0x86,0xdb,0xe5,0xa5,0x5a,0xbe,0x59,0x08,0x33,0x64,0xd9,0x29,0x5c,0x08,0x3d,0x11,0x83,0xf5,0xc3,0x96,0xb1,
  0x2b,0x2d,0x50,0xef,0x39,0xca,0xca,0x5f,0x66,0x5b,0xf5,0x2c,0x9b,0x74,0x6e,0xde,0x30,0x56,0x3b,0xdd,0x9a,0x91,0x20,0x30,
  0xa2,0xd3,0xaf,0xe2,0x8e,0xa1,0xca,0x5f,0x12,0xaa,0x95,0x34,0x2a,0xe0,0x10,0xe7,0xe9,0x96,0x78,0x16,0x7e,0xaf,0x75,0x5c,
  0xde,0x8f,0xe5,0x9b,0xd3,0x09,0x03,0xc2,0x84,0xb3,0x3c,0x15,0x1f,0x74,0x66,0x44,0x09,0xf7,0x0f,0x08,0x24,0x37,0x66,0xb4,
  0x1b,0xd0,0x48,0x4a,0x37,0xe6,0x1d,0x8a,0x6d,0x6d,0x2d,0xb4,0x3d,0xf7,0xe9,0x3e,0x39,0xa7,0x1d,0x5a,0x5e,0xa7,0xa5,0x2d,
  0xef,0xca,0xcb,0x9a,0x75,0x94,0x7d,0xc6,0x0e,0x15,0x8b,0xb2,0x68,0x52,0x59,0xcc,0x0c,0x40,0x6d,0xee,0xba,0xa3,0x8f,0x95,
  0xa0,0xbb,0xa2,0x14,0x1c,0x28,0xbe,0xa1,0x37,0x86,0x44,0xaa,0x15,0x16,0xae,0xa0,0x2f,0xe8,0x73,0x63,0x0d,0x21,0x3e,0x97,
  0x11,0x10,0x27,0xfe,0x0d,0xc7,0x0b,0x5a,0x12,0xae,0xf0,0xb0,0x9e,0x3c,0x58,0xd4,0x1b,0x30,0xee,0xa0,0x99,0xe0,0x88,0x6e,
  0xab,0x2a,0x5d,0x9c,0x2c,0x19,0x9d,0xcd,0x87,0x5f,0xd9,0x16,0x13,0xe0,0x43,0x59,0x4d,0x20,0xd6,0x48,0x51,0x43,0x5a,0x57,
  0xaa,0xd9,0x70,0x67,0x96,0x6e,0x8b,0xeb,0x8b,0xf8,0xd9,0x38,0xdb,0x66,0x9a,0x5a,0xf4,0x0f,0x7f,0x57,0xd5,0x5f,0xa3,0x7b,
  0x98,0xa7,0x33,0xc0,0x7c,0x6c,0x64,0x29,0xd8,0x6b,0xd7,0x91,0xc3,0x7b,0x32,0xb4,0xf2,0x14,0x1e,0x6f,0x38,0xba,0xcd,0x17,
  0xb9,0xdc,0x97,0xa5,0x49,0x32,0xd6,0x11,0x3e,0x22,0x86,0x1c,0x7f,0x83,0xac,0x45,0x2e,0x80,0x78,0xd1,0x4a,0x8a,0xa5,0x3c,
  0x14,0x58,0x8c,0x85,0xb6,0xd3,0x71,0xb2,0x6e,0xce,0x17,0xce,0x91,0xef,0x0f,0xaa,0x9d,0x25,0x44,0x30,0x0c,0x6d,0x5e,0xe7,
  0x14,0xf2,0x62,0x9c,0x81,0x11,0x84,0xf5,0x31,0x0c,0xf1,0x2c,0x57,0xaf,0x9a,0x3d,0xe0,0x2a,0x3a,0x36,0xf2,0x51,0x06,0x31,
  0xae,0xa6,0x39,0x6f,0x3e,0x1a,0x2b,0xce,0x12,0x7b,0x53,0xb8,0x1d,0x31,0xd9,0x2c,0x8f,0xee,0x05,0xc8,0x79,0x1b,0x3d,0xb0,
  0x59,0x10,0x4f,0x2f,0xe9,0xf2,0x5a,0x8b,0x23,0x77,0xa8,0xbe,0x78,0x80,0xa0,0x33,0x37,0xa1,0x7a,0x03,0x39,0x03,0x67,0xf3,
  0x39,0x0d,0x9a,0xa8,0x2e,0xc7,0xcc,0xdb,0x94,0x77,0x9b,0x1e,0x34,0xc2,0x8e,0xb4,0x66,0x45,0xc4,0x93,0xf5,0x1f,0xed,0xaa,
  0xa6,0xcd,0x0a,0x89,0x9f,0x79,0x54,0xc1,0x5d,0xb1,0xb1,0xe1,0x88,0xf6,0x17,0xf7,0x4e,0x16,0x44,0x45,0x6b,0x96,0x24,0x11,
  0xe3,0x19,0xaf,0xe7,0x96,0xb4,0xc4,0x50,0x43,0xf7,0x8f,0x49,0xb0,0x84,0x39,0x25,0x8d,0x69,0x51,0x32,0x61,0x24,0xce,0x9e,
  0x8d,0x8a,0x9c,0xf6,0x0d,0x25,0xdc,0xb2,0xa1,0x4a,0x01,0xab,0xd8,0x63,0x58,0xf0,0x32,0x06,0xef,0x9d,0x2e,0x1e,0xcf,0x32,
  0xd0,0xb5,0xaf,0x95,0x46,0x90,0xd5,0xc8,0x16,0x7b,0x6c,0x01,0x33,0xce,0xc0,0x0c,0x13,0x57,0x1e,0xc8,0x12,0x4f,0xeb,0x8a,
  0xda,0x44,0xb5,0x1e,0x39,0xcb,0x2f,0x87,0x71,0xbf,0x91,0xd3,0x44,0xe5,0x74,0x49,0xa0,0xa5,0xc4,0xbd,0x31,0x74,0xff,0xbc,
  0x9c,0xac,0xff,0x76,0x72,0xfe,0x7a,0x10,0xf7,0x35,0x7d,0x8f,0x8d,0x5a,0x5b,0x2a,0x85,0xad,0x6e,0x3f,0x69,0x3e,0x52,0x7e,
  0x54,0xd8,0x50,0x89,0x8d,0xd1,0x06,0x74,0xa1,0x36,0xac,0xb9,0x7f,0xd4,0xad,0xe1,0x8d,0x6b,0x34,0x7e,0xa7,0xd6,0x74,0x22,
  0xbd,0x2a,0xb5,0xa0,0x55,0x22,0x7a,0xf9,0xdf,0x08,0x10,0x4e,0xc1,0x80,0xf7,0x3d,0x36,0x54,0x70,0x3e,0x4c,0x0a,0x31,0xc1,
  0x0d,0x52,0x4c,0xd2,0x66,0x74,0x93,0x3f,0xcb,0xa4,0x09,0x91,0xec,0x75,0x5c,0xf9,0x00,0x8e,0x62,0x31,0xea,0x40,0x78,0xe2,
  0x48,0xc9,0x08,0xb4,0xc7,0x86,0xd2,0xa4,0x0b,0xfb,0x46,0x7b,0x5d,0xda,0x30,0x37,0x3b,0xc3,0x53,0xe2,0xaf,0xdc,0xf7,0x29,
  0x6f,0x58,0x6a,0x97,0xb9,0xfe,0x4b,0xd3,0x73,0x60,0x29,0xe3,0x88,0x3b,0xb6,0x74,0xe3,0x9d,0x31,0x90,0xd3,0x96,0x01,0x35,
  0xd9,0xc8,0xff,0x3a,0xb4,0x38,0xd6,0x92,0x29,0x85,0xfa,0xf0,0x94,0x9e,0xfe,0xb5,0xa6,0xf5,0x45,0x35,0x54,0x6f,0xda,0x9d,
  0x34,0xb4,0x83,0x98,0x6e,0xbc,0x53,0xcb,0x23,0xda,0x2f,0x84,0x94,0x2e,0x01,0x03,0x2e,0x71,0xef,0xcd,0x03,0x24,0xdf,0x6c,
  0xfe,0xbf,0xa9,0xa3,0x4e,0xf6,0x68,0x72,0x78,0x1c,0xe3,0x30,0xb8,0xa4,0x72,0xdc,0x4e,0x65,0x43,0x3f,0x54,0xdb,0x78,0xc0,
  0xc4,0x05,0x41,0x95,0x5e,0xa0,0xee,0x38,0x03,0xa2,0x3b,0x76,0xc7,0xaa,0x81,0x90,0x77,0xa4,0xad,0x56,0x87,0x83,0xc8,0xd7,
  0x26,0x4c,0x3b,0x9d,0x8d,0xb3,0x4a,0x21,0x2d,0xc0,0xd0,0x19,0x5c,0xcd,0x72,0x55,0xeb,0xd0,0xc2,0x82,0xcc,0x29,0x44,0xe1,
  0xdb,0xc0,0x1a,0x67,0xdf,0x87,0x38,0x02,0x5c,0xfc,0xb1,0x20,0xee,0x0a,0xe6,0x5b,0x7b,0x29,0x5b,0x2b,0xb4,0x91,0x33,0xd4,
  0x42,0xa8,0xf8,0x36,0x17,0x34,0x69,0xab,0xa7,0xc8,0x7c,0x48,0x3d,0x74,0x4e,0xca,0x24,0x58,0x04,0x5e,0x7e,0x10,0xb3,0xab,
  0xc4,0x46,0x07,0xac,0x9f,0xf4,0xe0,0xfb,0x87,0xf3,0x92,0xd1,0x66,0x1c,0xa2,0x84,0xc6,0xab,0x96,0x41,0x80,0x39,0x86,0x40,
  0x5d,0x47,0x81,0x45,0x68,0xd8,0x30,0x4d,0x6d,0xf9,0x26,0xca,0xcc,0x6c,0x74,0x36,0xbe,0x1a,0x10,0xc3,0x9a,0xab,0x96,0x01,
  0xb0,0x1d,0x15,0xac,0x2d,0x11,0xe6,0x75,0xfd,0xb2,0x5e,0xe6,0x02,0x48,0x1e,0x0b,0x23,0xd1,0xf6,0x24,0x57,0x62,0x98,0x47,
  0xca,0x08,0xa8,0xb9,0xa2,0x95,0x90,0xe9,0xf0,0x0a,0xa0,0xab,0xfe,0x47,0x04,0x55,0x26,0x90,0xf9,0x7d,0xe7,0xc1,0x7c,0xf7,
  0x26,0x11,0x7c,0x28,0x3d,0xa5,0xf5,0x5b,0x8b,0xa4,0x9b,0x71,0xea,0x47,0xa6,0xc9,0xfb,0x4a,0xdb,0x23,0x6b,0x0d,0xf4,0x80,
  0x21,0x7d,0xb2,0xd8,0xa0,0xfb,0x29,0xb0,0x2f,0x64,0x6e,0xc4,0x8d,0xf6,0x59,0xc7,0xda,0xc4,0x9f,0xa7,0x48,0x2e,0xe2,0xfa,
  0xc8,0xdf,0xfb,0x9e,0x8b,0x41,0x22,0x5d,0x86,0xf1,0x94,0xd3,0xf2,0xf7,0x57,0x09,0xef,0x7c,0x96,0x0c,0x3d,0x22,0xd7,0x1c,
  0x06,0x85,0x5e,0xf5,0x97,0xb1,0xf9,0x74,0xa0,0x93,0x96,0x43,0xad,0x4c,0x1f,0x4a,0x11,0x8e,0x50,0x41,0xf6,0x00,0x23,0x67,
  0xeb,0x01,0x31,0x91,0x0c,0x20,0x7a,0xcc,0xae,0x86,0x13,0x95,0x0f,0x15,0xb7,0xbb,0x72,0x24,0xcc,0xf8,0x98,0x55,0x81,0x7a,
  0x1e,0x75,0x59,0x05,0xfe,0x52,0x68,0xd2,0x66,0x1a,0x88,0x87,0x9c,0x90,0x58,0xb4,0x3e,0xea,0x20,0x41,0x56,0x0c,0x44,0x2e,
  0xd3,0x63,0x5f,0xaf,0x66,0x08,0x31,0x26,0x6f,0x2f,0x21,0xa3,0xee,0xdc,0xcd,0xae,0x84,0xd2,0x9b,0x63,0x5c,0x6b,0x8e,0x21,
  0x2c,0xa7,0x67,0x2f,0x1c,0x22,0xd1,0x7f,0xb6,0x74,0x43,0x42,0x32,0x44,0xad,0x35,0x9a,0x41,0x4c,0xba,0x74,0x2d,0x63,0x99,
  0xf9,0x47,0x0a,0x33,0x57,0x68,0xf0,0xfb,0x06,0x03,0x7a,0xb1,0x0b,0x9d,0x9b,0xe7,0x45,0xd4,0xc9,0x64,0x70,0x2d,0x41,0x67,
  0x58,0x68,0x67,0xf5,0x84,0x40,0xd3,0xf3,0x80,0x49,0xc6,0xf1,0x05,0xfc,0xac,0xc8,0x76,0x9b,0x8c,0x65,0xa6,0x63,0x18,0x4d,
  0x2b,0x74,0x9a,0xc8,0xaa,0x16,0x1e,0x94,0x4c,0xeb,0x51,0xf1,0xcf,0x38,0x84,0x81,0x68,0x54,0x69,0x8f,0xae,0xc9,0x03,0xaa,
  0xe2,0x43,0xe1,0xe5,0x72,0x09,0xb6,0x1a,0xc3,0x60,0x6c,0x0d,0x24,0x61,0x75,0x6c,0xff,0xec,0xfd,0x8b,0xac,0xf9,0x35,0xb2,
  0xe8,0xa8,0x41,0xca,0xdd,0x9e,0xe4,0xb2,0x8c,0x48,0xee,0x4a,0x67,0x3b,0x7c,0xbb,0x12,0x96,0x35,0x80,0x1b,0xcc,0xc1,0x38,
  0x13,0xd3,0xf5,0xdf,0x39,0xe9,0x69,0x85,0xf4,0x03,0x20,0x61,0x21,0xdb,0xd5,0x98,0x09,0x78,0xcd,0x28,0xbe,0x0a,0xfc,0xe5,
  0x2a,0x3e,0x74,0x87,0xd5,0xd5,0xe7,0x02,0x72,0xe6,0x16,0x64,0xa6,0xc6,0xce,0xe3,0x02,0xf4,0x5b,0x8f,0x59,0x10,0xe3,0x8c,
  0x38,0x86,0x88,0xf0,0xbd,0xae,0xa8,0xda,0x04,0x34,0xf7,0x80,0x91,0xae,0x16,0x19,0x15,0xc3,0x81,0x76,0x9e,0xf2,0xaa,0xc5,
  0xc9,0x09,0xd1,0x37,0x20,0x4a,0xff,0xeb,0x27,0xe3,0xc5,0x8f,0xe1,0x6d,0xe0,0x09,0xf7,0xd6,0xb8,0xc5,0xfe,0xa4,0x31,0xfd,
  0xed,0xe7,0xe4,0x47,0x97,0x7a,0xaa,0xf5,0xca,0x4b,0x50,0x8d,0x64,0xc4,0x36,0x59,0x6d,0xd4,0x4d,0xf3,0xd5,0x35,0xf8,0x75,
  0xac,0xde,0xae,0xd4,0xaa,0xfb,0x7f,0x3c,0xa8,0x7b,0x4d,0x48,0x0a,0x8b,0xe8,0xe0,0xf7,0x6d,0xef,0x44,0x01,0xa7,0xe0,0x92,
  0x16,0x75,0x4e,0x43,0xe1,0xab,0x26,0x63,0xe1,0x3c,0x92,0xc5,0x60,0xd4,0x35,0xa7,0x59,0x65,0xe2,0xa4,0xe2,0x55,0x1f,0xa3,
  0x33,0xb1,0xbf,0x07,0x83,0x45,0x0f,0xed,0xc4,0x98,0x1a,0xaa,0x54,0xd1,0x87,0x7a,0xf6,0x3d,0xe1,0xc8,0xc3,0xf6,0x7a,0x08,
  0xf3,0xab,0xc6,0x85,0x6f,0x4f,0xe9,0x15,0x06,0xbc,0x4c,0xe8,0xf2,0xcc,0x92,0x51,0x33,0x26,0x36,0x1c,0x32,0xcc,0xed,0x13,
  0x4d,0x04,0x9a,0x4c,0x1d,0xc9,0xd8,0xe9,0x6c,0xeb,0x65,0x49,0x58,0x44,0x3c,0x72,0x2f,0x7e,0xcc,0xd6,0xbf,0xd3,0x1d,0x50,
  0x2e,0xd0,0x5e,0xa6,0xe3,0xe9,0xb8,0xb9,0xa2,0x0b,0xb5,0xe8,0x56,0x35,0x29,0xf5,0xb9,0x85,0x65,0xfb,0xeb,0x34,0xeb,0xca,
  0xe9,0xdd,0x70,0xf3,0x3b,0x2f,0x0a,0x63,0x17,0x75,0x72,0x64,0x58,0xb9,0x15,0x8f,0x22,0xc2,0x00,0xef,0xa0,0x84,0x6b,0x3c,
  0x1f,0x8b,0xaa,0x69,0xb7,0x1f,0x43,0xf3,0x9c,0xf8,0x99,0xe3,0xc6,0x71,0xe0,0xe5,0x90,0x38,0xd4,0x84,0xf7,0x5c,0x92,0x18,
  0x60,0x59,0xee,0xbb,0x2f,0x4b,0xf9,0x4e,0xfa,0x2a,0x8f,0x2c,0xfe,0x00,0xb5,0x73,0x5e,0x13,0x25,0x44,0x23,0x5a,0x6b,0x25,
  0x15,0x41,0x5d,0xcc,0xd7,0x18,0xee,0xc2,0xf7,0xe6,0xe9,0x18,0x7c,0xfb,0xa9,0xc3,0xec,0x05,0x79,0xff,0x88,0x25,0x74,0x58,
  0xae,0xbc,0xa2,0xea,0x37,0x3b,0xcf,0xfc,0x3e,0x88,0x48,0xc7,0xab,0x4d,0x8f,0xc4,0xec,0xef,0x5a,0xde,0x1b,0xce,0x72,0x55,
  0xd5,0xf8,0x64,0x7c,0x8d,0xfe,0xb3,0x19,0x19,0x3a,0xb0,0xfe,0xde,0x69,0x14,0xf2,0x74,0x2f,0x2d,0xa3,0x93,0x4a,0xb9,0x05,
  0x07,0x00,0xbf,0x69,0xa5,0x37,0x7c,0xc3,0x67,0x50,0x68,0xe0,0xed,0x00,0xfc,0x4e,0xd0,0xa7,0x16,0xc7,0x65,0xb0,0x89,0xad,
  0xaa,0x5e,0xd1,0xf5,0xd1,0xa4,0xe6,0x73,0xc1,0x3f,0xf0,0x59,0xc5,0x8d,0xd0,0x42,0xac,0xe3,0x8a,0x0f,0xed,0xe3,0xf7,0x84,
  0x5a,0xdb,0x62,0xb1,0x85,0x54,0x89,0x97,0x8a,0x58,0x58,0x50,0xe8,0x45,0x38,0x9d,0xff,0xc0,0xe8,0xed,0xac,0x39,0xa8,0xae,
  0xe6,0x26,0x39,0x01,0x39,0x09,0x3b,0xd4,0x78,0xa5,0x62,0x03,0x47,0x27,0xc3,0x29,0x32,0xad,0x84,0xb3,0xf5,0xf3,0x1d,0x8b,
  0x9e,0x2b,0x9f,0x7c,0x2f,0x6c,0x72,0x30,0xfc,0x40,0x5d,0x64,0x3a,0x95,0x4e,0x17,0xa7,0x70,0xb5,0x72,0xd8,0xad,0xf2,0xa7,
  0xb8,0xf9,0x44,0x5c,0x6f,0xb6,0x0f,0x40,0x42,0x83,0x43,0xce,0x24,0x7d,0x9a,0xf9,0xe5,0xba,0xba,0x60,0xa2,0xff,0xf7,0x8c,
  0xa6,0x72,0xcd,0xed,0x4a,0x01,0xd1,0xaf,0xd8,0x4d,0x4d,0x20,0x78,0xf2,0x0b,0x60,0xc8,0xff,0x05,0x00,0x1d,0x62,0x6b,0xb4,
  0x67,0xa9,0x5e,0x74,0xcf,0xe3,0x4a,0xbe,0x57,0xff,0xe9,0x29,0xcf,0x04,0xfc,0x03,0x3b,0x31,0xcc,0xcb,0x92,0xa3,0x46,0xc3,
  0xd1,0x15,0xb4,0x51,0x90,0x2d,0x7d,0x22,0xca,0x08,0x18,0xf3,0xc2,0x51,0x61,0x4f,0xfa,0x40,0x7b,0x93,0x63,0xc1,0x3f,0xd0,
  0xdb,0xe6,0x61,0xd6,0x37,0x88,0xb3,0x8a,0x28,0x9d,0x6f,0xd0,0xc4,0x37,0xfa,0x62,0x75,0x3f,0x67,0x38,0x24,0xb6,0xff,0xe5,
  0x0d,0x1c,0x45,0x11,0x94,0xf1,0x02,0x2f,0x63,0xd6,0x0f,0x75,0x71,0xe3,0xda,0xb1,0x6c,0xd5,0xf2,0xaa,0x6b,0x7c,0x0a,0xd8,
  0x5c,0x95,0xf3,0x07,0xfa,0x0c,0x65,0xb3,0x47,0xd5,0x54,0xd9,0x99,0x29,0x1e,0xec,0x4f,0x73,0xc4,0x56,0x8a,0x70,0xcf,0x0d,
  0x72,0x7c,0x57,0x99,0x09,0xa5,0x43,0x05,0x4c,0x6d,0x02,0x7e,0xb7,0x65,0xfa,0xa5,0x47,0xe7,0x2d,0x32,0xd2,0x10,0xe2,0x39,
  0x3f,0xeb,0x43,0x3b,0x56,0x47,0x8a,0xcd,0x7c,0xe2,0xc3,0xad,0x6a,0xd1,0xf0,0xc4,0x8a,0x68,0xf6,0xb7,0x73,0x40,0x86,0xd7,
  0xcb,0x14,0x8c,0x84,0x48,0xd0,0x8e,0x6d,0xed,0x11,0x7e,0x9b,0xcc,0x6b,0x5a,0x4b,0x90,0x11,0xf4,0xd0,0xea,0x18,0xc0,0x9a,
  0xf2,0x71,0xe5,0x5a,0x29,0x07,0x7d,0x35,0x7c,0x28,0x41,0xea,0x95,0x81,0x06,0x5e,0xde,0xdb,0x79,0x7a,0x02,0x42,0x28,0x17,
  0xbf,0xd3,0xc2,0x7b,0xa0,0x0d,0xdd,0xc6,0x51,0x4c,0x12,0x14,0x79,0xac,0x51,0xfe,0x19,0x65,0x50,0xa5,0xf7,0x5e,0x12,0xd1,
  0xeb,0x84,0x01,0x74,0xa5,0x6c,0x32,0x54,0xf5,0x85,0x3e,0x34,0xab,0x70,0x31,0x26,0xf4,0x88,0x11,0xcd,0x0a,0x3b,0x77,0x83,
  0xd6,0xd6,0xd3,0xee,0xc2,0x12,0xe4,0x37,0x2a,0x3b,0x1a,0xe7,0x0d,0xb3,0x70,0x44,0xca,0xd6,0xce,0x72,0xdd,0x3e,0x1a,0x24,
  0xbd,0xb4,0xe3,0xdd,0xea,0xcc,0xc6,0x0c,0x31,0x5c,0x36,0x77,0xc8,0x43,0xb6,0x55,0x07,0xd2,0x5e,0x71,0x8d,0x25,0x18,0x2e,
  0x66,0xd9,0xce,0xc0,0x5b,0x23,0x94,0x1f,0x70,0x8f,0x9e,0xe2,0xf5,0x33,0x93,0x89,0xeb,0x90,0x83,0x82,0xbd,0x1f,0x5b,0x89,
  0x9d,0xd7,0xeb,0x69,0xd9,0xf6,0xc0,0x30,0xb7,0x21,0x80,0x5f,0xd1,0x01,0x1b,0x63,0x44,0x9b,0xb4,0x93,0xf6,0x0a,0xa6,0xc0,
  0x51,0xdf,0x14,0xdf,0xc1,0xb5,0x21,0xa1,0x3c,0xac,0x4d,0x73,0x5a,0x25,0xe4,0x90,0xbb,0x25,0x1e,0xfa,0x08,0x9c,0xb5,0x9d,
  0xb8,0x32,0x7a,0xbf,0x1a,0xcc,0x54,0xef,0xa7,0xa5,0xeb,0x0a,0x3f,0x7a,0xb6,0xd0,0x29,0xee,0xd0,0xb5,0x8c,0xef,0x52,0xe5,
  0x9c,0x2f,0xff,0x3e,0x50,0xb1,0xfb,0x81,0xf5,0x5c,0xbf,0xc4,0xd5,0xd8,0x04,0xfb,0x7b,0xc7,0x5f,0x78,0xe5,0xe4,0x10,0x95,
  0xd5,0x43,0xbc,0xaa,0x96,0xb9,0x1a,0xe8,0x59,0x22,0x8a,0x66,0xae,0xa0,0x25,0x3e,0x57,0xb8,0x0d,0x4a,0x2b,0x81,0x37,0x82,
  0xb5,0xcf,0xba,0x78,0x77,0xbe,0x6f,0x29,0x14,0x6a,0x4b,0x1f,0x02,0x33,0xcb,0x0c,0xec,0x83,0xb3,0x33,0x43,0xde,0x9e,0xb1,
  0xf1,0x86,0xea,0xad,0x8a,0x68,0x85,0x37,0xc6,0x65,0x11,0x3f,0x6d,0xc3,0x05,0xcf,0x8a,0x33,0xd2,0x1b,0xa6,0xcc,0x0f,0xd6,
  0x7f,0x98,0x22,0x95,0x4b,0xbf,0x97,0x3f,0xc3,0x1f,0x6c,0xf1,0x59,0x93,0xe2,0x84,0xed,0x0c,0xb3,0x97,0x10,0xf6,0x11,0xd3,
  0x73,0xdf,0xbe,0x75,0x5e,0x23,0x0f,0x44,0xaa,0x8a,0xb2,0x34,0x61,0x85,0x9a,0xe5,0x87,0x0e,0xd6,0xef,0x9b,0x8e,0xb1,0xf1,
  0x7c,0x33,0x1d,0xc9,0xc9,0x82,0xb6,0xd9,0xc4,0x34,0x4b,0x7f,0x43,0xb9,0xe5,0xf5,0x71,0xe3,0x59,0x45,0xed,0x1e,0xeb,0xb8,
  0xcc,0x11,0x1c,0x0d,0x38,0x3c,0x80,0xc2,0xa1,0x16,0xa7,0xd2,0x70,0x9e,0x37,0xdb,0xa7,0x8a,0xe9,0x4d,0xf9,0x76,0xfa,0xc3,
  0x2e,0x94,0x27,0xdf,0x0f,0x57,0x45,0xfc,0xb4,0x67,0xab,0x9e,0x9e,0x94,0x13,0xd4,0xa6,0x07,0x52,0xc0,0x2a,0xb7,0xf8,0xf2,
  0x16,0x8a,0x5a,0x8e,0x10,0xf1,0x5c,0x76,0x68,0x80,0xbb,0x58,0xe3,0x66,0x01,0x82,0x8b,0xd8,0xbb,0x87,0x95,0x34,0x90,0x2c,
  0x27,0xa2,0xce,0x0e,0x07,0x83,0x4b,0x3b,0xf1,0x9b,0x30,0xa6,0x3c,0x55,0x97,0xd8,0x4d,0xd4,0xdd,0x98,0x27,0x47,0xeb,0xf3,
  0xc0,0x08,0x2d,0xc9,0xe5,0xb5,0x08,0xc3,0xff,0xa0,0x85,0xf7,0x26,0x60,0xfb,0x26,0x74,0xe3,0xcc,0x18,0x1d,0x8a,0x3b,0x92,
  0x86,0xcc,0x40,0x76,0x04,0x6e,0xc9,0x08,0x3e,0xd4,0x50,0x72,0x75,0x35,0x59,0xfa,0xbf,0x7f,0x4f,0x4b,0x00,0xc9,0xba,0x40,
  0x86,0xab,0x73,0x32,0x9b,0x8d,0x08,0x8a,0xb5,0x7f,0x24,0x30,0xfe,0x73,0xed,0x4f,0x0a,0xff,0xa2,0xbd,0x9a,0x40,0xca,0x5f,
  0x8e,0xcd,0x26,0xa4,0x08,0xad,0xbd,0x9e,0x59,0x53,0xa5,0x24,0x2a,0x9e,0xf8,0x74,0x39,0x51,0x7c,0x81,0xd4,0x98,0x61,0x49,
  0xc2,0xe8,0x93,0xd9,0x5c,0x70,0x57,0x3a,0x50,0xab,0xfb,0x21,0xbf,0x8a,0x56,0x07,0xac,0x07,0xa8,0x37,0x2f,0x9f,0x13,0x92,
  0x8f,0x7e,0x00,0x16,0xfd,0x0f,0x6c,0x9f,0xa3,0xc1,0x40,0x67,0xd9,0xa1,0x4d,0x00,0x42,0x81,0x33,0xa3,0xa6,0xb8,0xd6,0xb9,
  0x4a,0xe0,0xb3,0xfa,0x8c,0x2b,0x52,0x55,0x52,0x75,0x75,0xa7,0x17,0xd2,0xd8,0xbc,0xa2,0xa2,0xc8,0x77,0xb1,0x19,0xdf,0x18,
  0x4c,0x51,0xeb,0xdc,0x98,0xd6,0x02,0x9c,0xe2,0x61,0xbf,0xb3,0x10,0x84,0xcc,0x2e,0x49,0x6d,0x61,0x72,0xb2,0xb7,0xf4,0xa7,
  0x7f,0x53,0xaa,0xd6,0x56,0x8e,0xce,0x84,0x2a,0xee,0x07,0x07,0xe9,0x0f,0x1a,0xb7,0x2a,0xee,0x80,0xb9,0x31,0xe4,0x1e,0x78,
  0x5b,0x00,0xa6,0x20,0x7c,0x89,0xba,0xbb,0x70,0x2e,0x7e,0xbe,0x98,0x6b,0xa4,0x27,0xa7,0x69,0xdd,0x13,0x7a,0xa6,0x10,0xe4,
  0xf2,0x16,0x78,0x71,0x43,0x0b,0x40,0x83,0xc7,0xb8,0xd9,0x55,0xa6,0x82,0x17,0xc9,0x6b,0x1b,0x9a,0x7d,0xcf,0xac,0x79,0xef,
  0xe9,0x5a,0x9c,0xc7,0x41,0xb6,0x8e,0x89,0x03,0x6c,0x54,0xc7,0x73,0xb4,0x96,0x71,0xd4,0xda,0xd0,0x98,0x13,0xb3,0xd0,0xcd,
  0x21,0xb8,0xd6,0x67,0xe2,0xb9,0x02,0x60,0xa1,0xdd,0x1f,0x0d,0x4a,0xd8,0x15,0xe4,0x04,0x9f,0x43,0x06,0xa8,0xd4,0xbc,0x88,
  0x76,0x43,0x94,0xfc,0x1c,0x4f,0x1f,0x96,0xa9,0x97,0x8e,0x0a,0x9a,0xfd,0x01,0xea,0xaf,0x83,0x21,0x9d,0x2e,0x81,0x2e,0x92,
  0x64,0x27,0x4b,0x50,0x24,0x1b,0x53,0xde,0x24,0x26,0x7d,0x7d,0xe3,0xd1,0x3f,0x4a,0x19,0x5d,0xff,0x8c,0x4a,0xcd,0xe2,0x44,
  0x44,0x27,0x47,0x2e,0x6a,0x34,0xdb,0xbc,0x8b,0xc5,0xd3,0x56,0x0b,0x15,0x75,0xee,0xdf,0x72,0x02,0xc5,0x96,0x01,0xd6,0x5b,
  0xd2,0xee,0xd3,0xf5,0xeb,0x4c,0xe7,0x7b,0xdc,0xce,0x22,0xbc,0xa0,0xdb,0x18,0xc6,0xe7,0xe6,0xe0,0x13,0xc1,0xeb,0x25,0x07,
  0x7e,0xc3,0x0d,0xc7,0xa0,0x4a,0xb8,0x5f,0x8e,0x85,0xa5,0x48,0xe6,0xa3,0x73,0x78,0x13,0x0c,0xdb,0x9b,0x74,0xbb,0x51,0x05,
  0x71,0xbb,0x38,0x80,0x09,0xf3,0x4c,0x2f,0x6d,0x18,0x58,0x2c,0x27,0x9c,0x96,0x69,0x56,0xf9,0x64,0x2f,0x07,0xbb,0xaf,0xda,
  0xed,0xe7,0x83,0x45,0x0c,0xf2,0x1c,0xff,0x6d,0xbd,0x8d,0x7f,0xda,0x89,0xd2,0xf0,0xda,0xcd,0x8b,0x4a,0xe1,0xaa,0xe8,0xc6,
  0xde,0x2c,0x73,0x29,0xcb,0xc3,0xfd,0x17,0x3f,0xd9,0x28,0xaa,0x80,0xb4,0x51,0x3f,0x78,0x72,0xd8,0x19,0x03,0x6b,0x3d,0x0c,
  0x29,0xce,0x14,0xbf,0xdf,0xff,0x2e,0x43,0xfa,0x86,0x99,0xe9,0xe5,0x80,0xf0,0xa5,0xc2,0xde,0x22,0xaa,0xb7,0x67,0x08,0xdb,
  0x1a,0x62,0xbd,0x17,0xb9,0x83,0xc9,0x3f,0xe9,0x2f,0x03,0x4a,0x99,0x29,0x0b,0xdb,0xab,0xab,0x8c,0x6c,0x45,0x1d,0xf7,0x76,
  0x7e,0xd4,0x27,0xfd,0x18,0x85,0x6b,0xc9,0xd8,0x26,0xb6,0x7b,0xda,0xf8,0x90,0x51,0x5d,0xdf,0xfb,0xfc,0xaf,0xf7,0x25,0x05,
  0x44,0xdc,0x4f,0xb4,0xa7,0x82,0x4b,0x7d,0x6d,0x8f,0x28,0x75,0x85,0xfb,0x18,0xbb,0xe9,0x93,0x67,0x1d,0x3a,0x6a,0x96,0xc2,
  0x16,0xc1,0xba,0xfb,0x18,0x26,0x9a,0x27,0x2a,0x78,0x77,0xd3,0x30,0x41,0x2a,0x31,0x03,0x9d,0xb7,0x29,0x23,0xad,0x02,0xfb,
  0xf2,0x18,0x60,0x8a,0xe7,0xec,0xb2,0x0f,0x76,0x05,0xa2,0xf0,0x29,0x3f,0x52,0xcb,0x53,0x39,0xba,0xed,0x1f,0xd4,0x9b,0x1d,
  0x6a,0x79,0xd1,0xb8,0xd8,0x58,0x14,0x38,0x3d,0xe7,0x24,0xe8,0x30,0x41,0xdf,0x23,0x7c,0xc5,0x11,0x45,0x93,0xa2,0x3c,0xfb,
  0xd8,0xca,0x20,0xfd,0x8a,0x38,0x4a,0x58,0xfc,0xad,0x3e,0xfc,0x1f,0xe6,0x17,0x4d,0x11,0xef,0x9e,0x59,0x96,0x35,0xd6,0x3f,
  0xc0,0xe3,0x2b,0x2f,0xb3,0x67,0xff,0x97,0xdf,0xe0,0xdb,0x64,0x23,0x20,0x88,0x88,0x3a,0x1d,0x94,0x70,0x6a,0x14,0x88,0x9d,
  0xa7,0x1e,0xf9,0x61,0x66,0xcc,0x40,0xb5,0xdf,0xb8,0x36,0x29,0xfa,0xc1,0xb7,0x63,0x4e,0xf6,0xef,0x9f,0xcb,0x01,0xb7,0xf3,
  0x67,0xdf,0xec,0x0e,0x9a,0xfe,0xbf,0xc7,0xf0,0xfa,0xbe,0xbf,0x04,0x88,0x7c,0x48,0x88,0x49,0x93,0x90,0x5c,0x8d,0xd3,0x44,
  0x02,0x1a,0x7c,0x3c,0x7a,0x76,0x4b,0x26,0x7b,0x06,0x23,0x7d,0xf4,0x72,0xf8,0x35,0x3e,0x74,0xd5,0xc6,0x19,0xcc,0xde,0x07,
  0xd7,0xba,0x4d,0x24,0x93,0xb1,0x91,0x9a,0x47,0x4a,0x0e,0x74,0x9e,0x77,0x51,0xc9,0xe9,0xe1,0xa1,0x31,0xc3,0x8e,0x3c,0x8c,
  0x80,0xdf,0xc2,0x65,0xaa,0x1d,0x9b,0x9c,0xea,0xbb,0x2e,0xca,0xee,0xa9,0x9c,0xd9,0x48,0x93,0x94,0x58,0x3b,0xf6,0x23,0x53,
  0xb0,0x8d,0x40,0x7f,0x38,0x30,0x63,0x8a,0xcd,0x3b,0x6d,0x82,0xc9,0xd4,0x0f,0x99,0xf0,0x62,0x85,0x15,0x9f,0xc9,0x0c,0xb1,
  0x32,0x23,0x6a,0x33,0x94,0x97,0x5b,0x03,0x69,0x84,0xb7,0x77,0x28,0xdb,0x2c,0xda,0xac,0xfa,0xee,0xb9,0xe7,0xf6,0x45,0xf2,
  0x77,0x2c,0x06,0xf6,0x08,0xb1,0xe6,0x24,0x4c,0xac,0xf9,0xa0,0x03,0x03,0x34,0x51,0x1b,0xbe,0x6d,0x4e,0xf8,0x9e,0x3a,0xf3,
  0x82,0x99,0xd5,0x75,0x9c,0xd6,0xfc,0xb2,0xf1,0x72,0x5d,0x80,0x77,0x2c,0xbc,0x3c,0xf6,0xf5,0x12,0x6c,0x2e,0xf0,0x6a,0x53,
  0x64,0x92,0xf8,0xa0,0xa4,0xf4,0x15,0x28,0xc2,0xdb,0xd3,0x29,0x5c,0x7c,0xfa,0xbf,0xa4,0x3e,0x3e,0xbc,0x7c,0x8a,0xcb,0x03,
  0x30,0x1c,0xdc,0x64,0x69,0x42,0xfb,0xcb,0xfa,0x54,0x99,0xaa,0xc6,0xc4,0xd0,0xa7,0xf1,0x0e,0x35,0xa1,0x0b,0x3b,0x06,0x14,
  0x3d,0x5d,0xd5,0xdd,0x3c,0xb2,0xaa,0x6c,0x77,0xe6,0x95,0xd4,0xf1,0x6a,0x07,0x16,0xc9,0xbe,0xe6,0x9a,0x7c,0x7b,0x0c,0x49,
  0xeb,0x1d,0x5f,0xb6,0x6e,0xd1,0xec,0x05,0xa5,0xaa,0x7c,0x80,0x31,0x7e,0x4b,0x79,0x11,0xdf,0x98,0xad,0x4b,0x8c,0x4d,0x14,
  0x63,0x26,0x1f,0xbc,0xaa,0x3d,0x5d,0x07,0x78,0x9e,0xc0,0xd7,0xbc,0x04,0x66,0xf2,0x6e,0x2f,0x6f,0x90,0x35,0x09,0x2e,0xc5,
  0x64,0xdb,0x70,0xa8,0x45,0xfb,0xf1,0x07,0x21,0x5c,0x7a,0x9a,0xaa,0xf7,0x4e,0xdd,0x2b,0xcf,0xed,0xe5,0xa9,0xb0,0x8d,0xe6,
  0x05,0xeb,0x06,0xd7,0x82,0x8c,0xf8,0xab,0x5a,0x15,0xfe,0x02,0x8f,0x44,0x30,0x30,0x64,0xf0,0x25,0xc6,0xf9,0xbf,0x33,0x51,
  0x1f,0x77,0x85,0x22,0x6d,0x0a,0xe0,0x5f,0x5a,0xd7,0xff,0xbe,0x69,0xa8,0xb3,0x61,0x31,0x5f,0x4a,0xd3,0x57,0x01,0x33,0xf4,
  0xdc,0x38,0xf6,0xea,0xb7,0x21,0xaf,0xf8,0x9c,0x77,0x2c,0x5f,0x26,0x21,0xef,0x8b,0x7f,0xc9,0xa0,0x1e,0x0f,0x3e,0x46,0xcf,
  0xf5,0x6d,0x7b,0x17,0xf0,0x79,0xc8,0x80,0x5f,0x9b,0xfd,0x86,0xed,0x8b,0x8c,0x7c,0xf6,0x73,0x04,0xf3,0x88,0xc3,0x8c,0x5d,
  0x7a,0x81,0x43,0x44,0xec,0xe3,0x20,0xe5,0x21,0xd7,0xa6,0x2e,0xa0,0x12,0x4e,0xd4,0xae,0xc1,0xed,0x3b,0x5e,0xf7,0x04,0x07,
  0x57,0x54,0xe3,0x44,0xf6,0xdd,0x19,0x7a,0x81,0x6c,0x1b,0x1c,0xc3,0x9b,0xe1,0xdc,0x2a,0x19,0xe1,0x2c,0xf1,0xec,0xea,0x77,
  0x81,0x0b,0x51,0xd9,0x38,0x05,0x3e,0x1b,0x6e,0x58,0x56,0xb3,0xb1,0x37,0x48,0x29,0x84,0x26,0x99,0xb2,0x86,0x71,0x36,0x3a,
  0xb1,0x27,0x57,0xb9,0xa4,0x7a,0xd7,0xb7,0xfa,0xa2,0xa2,0x5e,0x7a,0x7e,0x07,0x60,0x8d,0xb2,0x37,0xe9,0x3d,0xdd,0x50,0x1a,
  0x5b,0x7e,0xba,0x06,0x8f,0xc2,0xb9,0xdc,0x23,0x88,0xfe,0x78,0x67,0x76,0x67,0xdf,0x78,0xd7,0x16,0x24,0xb7,0x95,0xfd,0x6c,
  0x4d,0xb5,0xe8,0xa0,0xfd,0xf8,0x51,0xd6,0xe5,0x36,0x8e,0xd2,0xf1,0xf2,0x59,0x91,0x92,0x27,0x0d,0x90,0xb5,0xd9,0x66,0x1a,
  0xa4,0xa2,0xbb,0x91,0x7c,0x1b,0xdc,0xd9,0xeb,0x99,0x8a,0x68,0xf6,0xd5,0x63,0x78,0x4a,0x66,0x2a,0x40,0x38,0xd9,0x33,0xe2,
  0xc5,0x68,0x6b,0xbd,0xa1,0xcd,0x40,0x2e,0xe6,0x1b,0x5b,0x1f,0xe8,0xb9,0x0a,0xdc,0x2f,0x1e,0x23,0x5d,0x4b,0x3a,0x7e,0x4a,
  0x66,0xa6,0x46,0x42,0xd2,0x09,0x48,0x9f,0x1f,0x6d,0xf2,0xf9,0x22,0x51,0x32,0x8b,0x06,0x8e,0x6e,0xb1,0x71,0xd6,0xda,0x45,
  0xb9,0xc7,0xb9,0xef,0xb3,0xcc,0x20,0xf7,0xe8,0xed,0x3c,0x44,0x2f,0xc6,0x5e,0x21,0x38,0x06,0x60,0x1e,0x9d,0xac,0x9d,0xef,
  0x26,0xdd,0xd8,0x0e,0xdb,0x27,0x04,0xdb,0xa6,0x02,0x97,0xdd,0x94,0x20,0xc7,0x33,0x58,0xf6,0x96,0xa8,0x69,0xfd,0x1b,0xd2,
  0x08,0x2b,0x5c,0x34,0x1c,0xf7,0x42,0x75,0xed,0x99,0xf3,0xd1,0xa1,0x56,0xa2,0x32,0xc7,0xed,0x0f,0xda,0x0a,0xb2,0x40,0xe5,
  0x09,0x3c,0xf6,0x6a,0x55,0xbb,0x7b,0xd1,0x32,0x4a,0x70,0x54,0x63,0x9f,0xad,0x48,0x39,0x72,0x31,0xc7,0xa6,0xf2,0x21,0x39,
  0xee,0x05,0x46,0x55,0x09,0x5d,0xbd,0x2e,0x28,0xc5,0x52,0xf4,0x31,0xce,0x99,0x55,0x67,0xf6,0xd9,0x34,0x53,0xcd,0x3f,0x22,
  0xff,0xf0,0x90,0xed,0xcc,0xc8,0x4c,0x86,0x43,0xd3,0x98,0xf1,0x84,0xd3,0x30,0x7a,0x57,0xa1,0x9a,0xa8,0x15,0x22,0x86,0x3a,
  0x5e,0xef,0xba,0xf9,0x0c,0xd2,0x04,0x07,0x8b,0xfb,0x4b,0x10,0xfa,0x93,0x6e,0x70,0x32,0x19,0x49,0xce,0xc7,0x85,0x69,0x38,
  0x2d,0xc3,0x09,0x41,0x27,0x8f,0x3e,0x0c,0x20,0xc5,0x49,0x1c,0xdc,0xcd,0x68,0xb6,0x11,0xd7,0xeb,0x89,0x3d,0x83,0xa7,0x6f,
  0x9a,0xf1,0xf7,0xf9,0x3e,0xbd,0x0f,0x52,0xfa,0x84,0x21,0xf5,0xac,0x49,0xfc,0x41,0x6a,0x3d,0x8a,0x8b,0x45,0xe1,0xb1,0x70,
  0x29,0x16,0xe6,0xf8,0x08,0x7f,0xaf,0xf1,0x02,0xff,0xc8,0x75,0x2a,0x0d,0x77,0xcc,0x16,0x3f,0x19,0xc9,0x3f,0x07,0x7c,0xd2,
  0xe7,0x1b,0x70,0xcc,0x81,0xf1,0xed,0x18,0x2d,0x5d,0xaa,0xcd,0xff,0x8d,0x90,0xc5,0x11,0x3a,0x3b,0xd1,0xf4,0xd5,0xc1,0x77,
  0x2a,0x93,0xeb,0xb2,0x54,0xa4,0xf0,0x29,0x23,0x60,0x51,0xcc,0xdc,0xfa,0x3e,0x1c,0xb4,0x54,0x8b,0xb0,0x43,0x89,0x1d,0x0f,
  0xdd,0xf1,0x61,0xa3,0x0b,0x08,0xed,0x5c,0xef,0xe1,0x0b,0xa9,0xf0,0xbf,0xc2,0xb9,0xfc,0x4a,0x26,0x6d,0x1b,0x10,0x14,0x90,
  0x27,0x76,0x9f,0x11,0x59,0x3c,0xfe,0xc6,0x2c,0x4c,0x0b,0x56,0x87,0x11,0x9d,0x59,0xc8,0xa7,0xab,0xab,0x59,0x4e,0x4c,0x15,
  0x58,0xfb,0x56,0xa8,0x91,0x2f,0x03,0xff,0x29,0x32,0x38,0xaf,0xd7,0x2f,0x47,0xeb,0xcf,0xfe,0x05,0xac,0xb9,0xbc,0x9e,0xb8,
  0xcd,0xa0,0x1f,0x74,0x20,0x35,0xa1,0x0d,0xdc,0xdf,0xe0,0xc7,0x9b,0x42,0x60,0x08,0xbe,0x68,0xf2,0x88,0xba,0xdc,0x45,0x9d,
  0x11,0x99,0x57,0xfd,0xef,0xa8,0xc0,0x7f,0x22,0x73,0x68,0x2f,0xc8,0xc2,0x57,0x17,0x18,0x9f,0x64,0xf8,0x74,0x7f,0x98,0x41,
  0xef,0xce,0x64,0x32,0x6b,0xe9,0xe9,0x4b,0xb7,0xd6,0x16,0x67,0x28,0xd1,0xfc,0x96,0x10,0x2e,0x75,0x76,0xe9,0x24,0x8d,0x39,
  0xcf,0x1a,0xb3,0x2d,0xbf,0x29,0x96,0xa2,0xa4,0x16,0xd9,0x12,0xeb,0xa6,0x69,0x34,0xff,0x1d,0xfd,0xae,0x2f,0x19,0xc9,0xf2,
  0xc0,0x8b,0x01,0x0a,0x63,0x62,0xac,0xc5,0xca,0x3d,0x9b,0xff,0x83,0x98,0x57,0xc8,0xba,0xec,0xd0,0x57,0xdf,0x75,0x0a,0xb3,
  0x40,0x69,0x46,0xa4,0x46,0x52,0xdb,0xab,0x20,0x53,0x20,0x87,0xe5,0x78,0xa8,0xfe,0x19,0xbd,0x15,0x4a,0x15,0x05,0x53,0x13,
  0xdc,0x26,0xc4,0xee,0xaf,0x39,0xfe,0xe9,0x95,0x12,0x91,0xe2,0xca,0xd7,0x18,0x95,0xb3,0xf0,0x12,0xe8,0xae,0x34,0x74,0x7f,
  0xb3,0xa3,0x01,0xce,0xa5,0x79,0xba,0x80,0x91,0xa0,0x42,0xc8,0x8c,0xdf,0xbd,0x09,0x46,0x12,0xca,0x35,0xde,0x21,0x69,0x59,
  0xa7,0x90,0x95,0xe0,0x17,0x05,0xa9,0xc9,0x7a,0x4e,0xfd,0x52,0xa2,0x16,0x82,0x69,0xfd,0xfd,0xe3,0x2c,0x8d,0x0d,0x17,0x1a,
  0x22,0x8a,0x4f,0xd3,0x33,0xf6,0x95,0x6d,0x23,0x74,0xb8,0x88,0xb7,0x8b,0x98,0x19,0xfd,0x72,0xdd,0xa8,0x7f,0x42,0x7b,0x1b,
  0xa2,0x84,0x6d,0x81,0xfb,0x07,0xbd,0x49,0x3a,0x27,0x8e,0x89,0x80,0xeb,0xcc,0x43,0xab,0xae,0x79,0xbe,0x96,0xb8,0x1e,0x9a,
  0x8e,0x02,0xc1,0x52,0x1c,0x00,0x4d,0x77,0x6b,0xfc,0x0b,0x4a,0xa9,0x4e,0x8b,0x23,0x51,0xb7,0xde,0xd9,0x2c,0xd9,0x7d,0x8b,
  0xfd,0x02,0xe0,0x13,0xa7,0x3c,0x3b,0xae,0x64,0x84,0xda,0x79,0x63,0x4c,0x1c,0x78,0xeb,0x2b,0x31,0x0c,0x8e,0xe1,0xd8,0x18,
  0x2c,0xef,0x98,0xcf,0xc4,0x48,0x92,0xfd,0xbc,0x84,0x00,0xae,0x3c,0x95,0x6f,0xcb,0xd9,0x93,0xa6,0x73,0xba,0x5e,0x40,0xbf,
  0x36,0x0a,0xd9,0x7a,0x4b,0xdb,0xd4,0x4d,0x7d,0xd1,0xb5,0xa0,0x6c,0x27,0xc4,0x33,0x9c,0x9f,0x7c,0x79,0xb7,0x8c,0x6c,0x4c,
  0x44,0x7a,0xb9,0xb5,0x42,0xbc,0xbf,0xcf,0xe6,0xf3,0xfc,0xd7,0x68,0xf8,0x10,0xd2,0xe4,0x58,0xaa,0xea,0x48,0x6b,0xc7,0xf6,
  0x41,0xeb,0x7b,0x7d,0x02,0x63,0x74,0x4e,0xc0,0xd9,0x53,0x02,0x0e,0xd9,0x97,0xbd,0x8c,0x7c,0xe8,0x20,0x01,0x01,0x78,0x24,
  0x70,0xa7,0x37,0x10,0x5c,0x04,0x7c,0x44,0xbc,0x42,0x83,0x20,0x78,0x00,0x7c,0xaa,0x9f,0xe5,0x38,0x2d,0xf7,0x6c,0x0b,0x30,
  0x9c,0xc5,0xf8,0xb9,0x96,0xa5,0xcd,0xab,0x14,0x3d,0xb1,0x3d,0x41,0x7a,0xc2,0x82,0x7a,0x27,0x95,0x89,0xee,0x9f,0xe9,0x55,
  0x01,0x3c,0xe0,0x3b,0x03,0xac,0x32,0x76,0x07,0x03,0x37,0xd9,0x70,0xc5,0x6c,0x85,0x07,0xa6,0xa9,0x94,0x76,0xe2,0x00,0x74,
  0xdb,0x0a,0x18,0xbd,0x80,0x59,0x4b,0x52,0xd4,0x39,0x20,0xc5,0x8f,0x1b,0x0a,0x7d,0x75,0x3e,0xd5,0xdb,0x62,0xfa,0x03,0xd1,
  0x99,0xa6,0x7f,0x7f,0x27,0x7f,0x95,0xa5,0xc7,0x19,0xe9,0xf8,0x6d,0xe5,0x53,0x18,0xd2,0x91,0x59,0xad,0xef,0xfb,0x17,0x90,
  0xc4,0xd4,0x61,0x96,0x15,0x3f,0x48,0x42,0x65,0x68,0xac,0x32,0xcc,0xf7,0x22,0x02,0x6c,0x9c,0xf0,0x75,0x3b,0x10,0x66,0xdc,
  0x69,0x39,0xd5,0x30,0x5b,0xeb,0x37,0xbf,0xd7,0x6c,0x91,0x16,0x7b,0xdc,0xff,0x60,0xad,0x01,0xa4,0x84,0x92,0x4e,0xe0,0x49,
  0x13,0xca,0xce,0x99,0x98,0xc9,0x40,0xf6,0xfa,0x21,0x5d,0x20,0xc8,0x3c,0xad,0x20,0x09,0xd1,0x6f,0xbd,0x81,0x76,0xc4,0xf7,
  0xea,0xd5,0x4f,0x7b,0x9d,0x10,0x73,0xcc,0x92,0x16,0x00,0xda,0xb0,0x2a,0xb7,0xa7,0x42,0xa4,0x04,0x03,0xa6,0x9c,0xf4,0x30,
  0xb5,0x9c,0x1d,0x63,0x19,0x2c,0xe8,0x45,0x00,0x86,0xfd,0x9d,0x19,0xab,0xd9,0x3e,0x56,0x1d,0x22,0xb9,0xd1,0x12,0x6f,0x37,
  0xab,0xa1,0x69,0xc4,0xed,0x9f,0xe8,0xeb,0x1d,0x3f,0xa7,0xd0,0x65,0x45,0xca,0x6e,0x7c,0xfc,0x6a,0xb1,0xf9,0x1b,0x90,0xaa,
  0x5a,0xbd,0x91,0xc5,0x21,0x72,0x7f,0xe0,0x92,0x96,0xe3,0x52,0x87,0x72,0x59,0xcc,0xa6,0xea,0xc8,0x7f,0xbc,0xb8,0x6e,0x2b,
  0x3f,0xd0,0x81,0x25,0x7f,0x4b,0xc7,0x54,0x52,0x2e,0x1f,0x3f,0xaa,0x2b,0x3a,0xc2,0x7b,0xb1,0x94,0xd7,0x7e,0x8a,0x16,0x56,
  0xfa,0xdf,0x46,0x2d,0x16,0x14,0x42,0xb0,0x22,0xa5,0x63,0x84,0xee,0x8c,0xb7,0xde,0x2c,0xbe,0xdb,0x91,0xf1,0x16,0x03,0xba,
  0xef,0xf8,0x4d,0x2d,0x21,0x84,0x16,0xaf,0xa6,0xc8,0x70,0x90,0xd7,0x6c,0x5e,0x80,0x50,0xb7,0xd0,0x04,0xfa,0xbd,0xc1,0x7a,
  0xe6,0xf9,0x5f,0xff,0x64,0x5b,0x8f,0x31,0xc3,0xe2,0x7e,0x5f,0xe8,0xb9,0xab,0x1e,0xfe,0x69,0x2c,0xa5,0x4d,0xe2,0xe8,0x9b,
  0x34,0xab,0xba,0x68,0xa0,0xbb,0x95,0xb2,0x58,0x23,0xa8,0xca,0xf6,0x9f,0x72,0x48,0xd1,0x9c,0x9d,0x3d,0xd1,0xef,0xbb,0x67,
  0x47,0x44,0xde,0xe0,0x1c,0x18,0x75,0xd9,0xa0,0x91,0xdc,0x03,0x7f,0x9c,0xcd,0xef,0x2e,0x1b,0xe3,0xab,0x77,0xe8,0x05,0xb6,
  0xed,0x81,0x37,0xea,0xb2,0x29,0x06,0x8c,0xbd,0x51,0xd5,0x1f,0x3e,0xf1,0x18,0x18,0xcc,0x00,0x3a,0x8a,0x9c,0x49,0xdd,0xda,
  0x02,0x38,0x54,0x8d,0x2e,0x8a,0x39,0x6b,0xdc,0x1c,0x8c,0x44,0x4c,0xb8,0x9c,0x36,0x75,0x9d,0xef,0x2b,0x3c,0x12,0x17,0x56,
  0x7a,0x5e,0xa1,0x48,0x46,0x38,0x54,0x10,0x73,0x39,0x69,0xe5,0xe8,0x28,0x06,0x4a,0x01,0x49,0xb2,0xe3,0x71,0x21,0xc3,0xe6,
  0x56,0x8b,0xbd,0x91,0x8e,0x4e,0x5d,0x0e,0xee,0x8f,0xf7,0xa1,0x66,0x16,0x37,0x09,0x94,0x1d,0xe6,0x67,0x05,0xd8,0x6c,0x53,
  0x4c,0xf8,0xbe,0x4e,0xed,0x72,0x71,0x5d,0xb7,0x67,0x7a,0x28,0xab,0xf7,0x89,0x36,0x2e,0xa2,0xd7,0xc3,0xc5,0x14,0xd5,0x57,
  0x32,0x23,0xcc,0x66,0x93,0x6a,0x40,0x4b,0xf2,0x33,0x95,0x1a,0xef,0xe4,0x8f,0x45,0x53,0x22,0xa7,0xd8,0x33,0xa8,0xdb,0x03,
  0x6b,0xfc,0x56,0xe1,0x31,0xd8,0x3c,0x54,0x6b,0x38,0x6e,0x3e,0xfc,0xb4,0x57,0x32,0xb8,0xc9,0xc5,0x79,0x70,0x03,0x92,0x5b,
  0x53,0xae,0x78,0x4c,0x87,0xbb,0x6d,0x9a,0x55,0x17,0xd7,0x32,0xf5,0x4f,0xf7,0x4b,0x8c,0x79,0x08,0x2d,0xe0,0x42,0x93,0x17,
  0x7f,0x1c,0x95,0x3c,0xdf,0xb5,0x08,0x25,0xce,0x02,0xd2,0x05,0x0b,0x61,0xc2,0xcf,0x6b,0xf4,0xf9,0x2c,0x15,0xc1,0xcf,0x95,
  0x02,0xef,0xca,0x35,0x7b,0xa5,0x72,0x78,0x56,0x8e,0x7e,0x25,0x58,0x2b,0xaa,0x60,0x7d,0x6e,0xbe,0x7e,0x66,0xca,0x60,0x0b,
  0x9b,0x49,0x76,0x33,0x74,0x14,0x1f,0x96,0x07,0xf3,0x65,0xbc,0x97,0xa5,0xb6,0xdc,0x67,0xa4,0x2f,0x29,0xdd,0x4d,0x6d,0x25,
  0x61,0x25,0x02,0x75,0xf4,0xd5,0xdf,0xf6,0x29,0x89,0x28,0xc6,0x11,0x4b,0x60,0x3f,0xbc,0x7a,0xc0,0x49,0xcf,0xb0,0x61,0x57,
  0x27,0xf9,0x45,0xe7,0x27,0x76,0x8b,0x8a,0xd4,0xb8,0x15,0xcd,0x76,0x4a,0xc7,0xb6,0xe1,0x1b,0x66,0x99,0x95,0x7f,0x9a,0xb2,
  0x13,0x4c,0x3e,0x95,0x60,0x84,0x54,0x65,0x16,0x5e,0x7e,0xae,0xfe,0x7b,0xcc,0x69,0x45,0x3e,0xa4,0x21,0x87,0xf5,0x84,0x4a,
  0x5a,0x2b,0x76,0xdc,0xbd,0xf1,0x11,0x02,0xd7,0x78,0xbf,0xe1,0xe7,0x16,0xd3,0xdb,0x2d,0x23,0x98,0x7c,0xec,0xd4,0xaf,0xf3,
  0xb9,0x78,0x94,0xec,0x5c,0xeb,0xf8,0x62,0x7e,0x08,0x6d,0x6c,0x88,0xbc,0x6c,0xc2,0x25,0xf8,0xdc,0xfb,0xa8,0x5c,0xb1,0xc4,
  0xc4,0x9d,0x0e,0x30,0x51,0x19,0xf4,0xdc,0xeb,0xc2,0x03,0xd9,0x61,0x43,0x09,0xdf,0xcb,0xc4,0x59,0xed,0xdb,0x38,0x75,0x74,
  0x4b,0x57,0x79,0xad,0x49,0xe6,0x86,0xa7,0x61,0xb1,0x6e,0x03,0xfa,0xa7,0xef,0x6c,0x89,0x8a,0xb1,0x55,0x28,0xc8,0xf0,0x43,
  0x23,0x7d,0x92,0xf4,0x3d,0x44,0x07,0xf8,0x33,0x17,0xa9,0x9d,0x79,0x06,0xa6,0xdd,0xd1,0x43,0x05,0x81,0x46,0x5f,0x97,0xe1,
  0xee,0x33,0x48,0x6c,0xa4,0x2c,0xf0,0x0d,0xa8,0x5a,0xd9,0x66,0x04,0xbd,0x1c,0x29,0xf8,0xe6,0x23,0x25,0xff,0x84,0x74,0x37,
  0xbe,0x20,0xfa,0xdf,0xd6,0x09,0x44,0x73,0x13,0x2b,0x55,0x8c,0x9a,0xdc,0xe6,0xbb,0x15,0xe1,0x3b,0xdb,0xd2,0xa6,0x53,0xa0,
  0x68,0x63,0x5d,0x1d,0x02,0xeb,0x02,0xc0,0xd4,0xac,0x6d,0x58,0x51,0x4d,0x39,0x20,0x6a,0x72,0x26,0x09,0x1d,0x92,0x8c,0xce,
  0x2f,0x0e,0xa7,0x56,0x1c,0x48,0xa3,0xf8,0xbf,0xe1,0xf2,0x1e,0xd3,0xda,0x3c,0xac,0x64,0x85,0x16,0x4b,0xf4,0x91,0xa5,0xc4,
  0x92,0xa9,0x75,0xd8,0xd2,0xb5,0x64,0x45,0xec,0xfa,0x99,0x34,0xb7,0xe3,0x91,0x3e,0x6f,0x2d,0x51,0xee,0x99,0xbc,0x16,0x9e,
  0xea,0x77,0xd2,0xfa,0x8e,0x4b,0x72,0x04,0x3e,0x92,0xbf,0xe3,0xea,0x17,0x21,0x20,0x7f,0xc5,0xc8,0x5c,0xf4,0x54,0xac,0x66,
  0xc9,0x35,0x42,0xd0,0xa8,0x7b,0x03,0x3d,0x8f,0x47,0xb4,0x21,0x34,0xb6,0x85,0xb5,0xc7,0xdc,0xfa,0xea,0xbb,0x81,0xfd,0x5b,
  0x5e,0x9f,0xcb,0xea,0xac,0xdb,0xe8,0x1f,0x10,0x44,0x99,0x7a,0x32,0x78,0xb0,0xb0,0x5c,0xab,0x14,0x6e,0x64,0xf7,0x8c,0x11,
  0x2c,0x9f,0x13,0x8a,0x12,0x92,0x71,0x20,0x76,0xb4,0xcb,0xa1,0x4d,0xc5,0x92,0x7d,0x0c,0x5f,0xb3,0x47,0xf5,0xb1,0xb2,0x49,
  0xf6,0xa4,0xdc,0x0c,0xf5,0x19,0x3c,0x9b,0x5e,0xb4,0xa2,0x82,0x9e,0x67,0xb8,0x13,0xdd,0x33,0xc2,0x6c,0x69,0x9f,0x62,0x41,
  0xd7,0xd4,0xd8,0xfa,0x07,0x15,0xe3,0x3a,0x1d,0x7a,0x54,0xbd,0x53,0x4d,0xcb,0xad,0x74,0x76,0x15,0x07,0xe9,0x9d,0x58,0xc2,
  0x55,0x0c,0x20,0x27,0x25,0x77,0xac,0xec,0x46,0x6e,0xf9,0xd6,0x05,0x79,0xe3,0x54,0x17,0xbf,0x5f,0x01,0xdd,0x85,0x66,0xb8,
  0x35,0x44,0x5f,0x46,0x1f,0xdd,0x11,0xed,0x3e,0x5a,0xf6,0x25,0x37,0x2d,0xdb,0x11,0xf0,0x21,0x15,0x05,0xcf,0x94,0x46,0xf8,
  0xb8,0xc7,0x7f,0xe9,0xe1,0x28,0x03,0x5a,0x1b,0x75,0xfa,0x40,0x16,0xe9,0x43,0x19,0xe0,0xbb,0xb7,0x56,0xc9,0x99,0x35,0x1c,
  0xb0,0xfd,0x60,0x43,0xe0,0x28,0x00,0x4c,0x1d,0x69,0x31,0xa3,0x05,0x44,0x5b,0xf6,0x95,0x31,0x06,0x00,0xc4,0x02,0xba,0xd1,
  0xce,0x48,0x27,0xf0,0x5c,0x95,0x05,0x00,0x35,0x53,0xb2,0x36,0x59,0xd2,0xf7,0xc6,0x45,0xd8,0xe6,0xf1,0xf8,0x3d,0x5d,0x38,
  0x86,0xf9,0x1d,0x2f,0xd1,0x89,0x74,0xae,0x6d,0xcb,0xf4,0x9d,0x95,0x4b,0xdd,0x0c,0x77,0x1e,0xb9,0x00,0xfe,0x7a,0xed,0xf8,
  0x8e,0xec,0x80,0x47,0xf6,0x2d,0xa4,0x21,0x90,0xfa,0xe5,0x9c,0xb6,0x58,0xb3,0xa8,0xf8,0x92,0x81,0x80,0x55,0x0e,0x7b,0xf8,
  0x58,0xd7,0x5d,0x45,0x77,0xf8,0x16,0x61,0x1e,0x70,0x8d,0xeb,0x05,0xac,0xae,0x5d,0x28,0xb6,0x48,0xf0,0x41,0x58,0x4a,0x11,
  0x99,0x81,0x96,0xdb,0xb7,0x02,0x2e,0x22,0x25,0xc0,0xa3,0xbe,0x43,0x8c,0x0d,0xa6,0x53,0xa5,0x50,0x57,0x7b,0x41,0x54,0xf3,
  0x37,0x12,0x6f,0x12,0xc9,0x9d,0xe8,0xb8,0x83,0xf0,0xf3,0xb9,0x22,0x0a,0x86,0xfb,0x10,0xcb,0x54,0x6d,0x66,0x78,0xc2,0xda,
  0x74,0xfe,0x73,0x10,0x42,0x74,0x02,0x9f,0xbd,0x20,0x49,0x97,0x26,0xdd,0x8c,0xa1,0xed,0x43,0x59,0x26,0xef,0x81,0xe0,0x35,
  0xf1,0x53,0x49,0xdc,0x42,0x7e,0xca,0x7d,0x9f,0xa5,0x36,0x6b,0x8e,0x2f,0x71,0x09,0xf2,0x3e,0x5d,0x74,0xec,0xd1,0x51,0x6b,
  0x71,0x57,0x25,0x5c,0xb1,0x2f,0x36,0x78,0x87,0x0c,0x69,0x95,0x84,0x6a,0x56,0x6d,0x41,0xc4,0x10,0x9e,0x05,0x91,0xc5,0xb3,
  0x45,0x05,0x9c,0x07,0x4c,0x00,0xb0,0xe4,0xc7,0xa3,0x84,0x2c,0xf1,0x23,0xc8,0xc6,0xc9,0xbc,0x0a,0x09,0xd1,0xa9,0x6f,0x5c,
  0x92,0xac,0x79,0x36,0xd8,0x88,0x47,0xf9,0x51,0xfb,0x5c,0xf0,0x84,0xf2,0x45,0x67,0x30,0x23,0x91,0x87,0x63,0x6f,0x20,0xe1,
  0xbe,0x98,0x7c,0x2a,0x75,0x74,0x66,0xbe,0xda,0x0e,0xb1,0xcd,0x9d,0x1d,0x8f,0x5c,0x22,0x0a,0x3a,0xf3,0x0a,0x67,0x46,0x64,
  0x87,0x46,0xf3,0x5c,0xf8,0x68,0x39,0xb6,0x28,0xfe,0x5e,0xa9,0x5d,0x34,0xb4,0x0d,0x7c,0x08,0x63,0xb1,0xb1,0x05,0x00,0x5b,
  0x01,0xc0,0x70,0x17,0x54,0x1d,0x0c,0x28,0xe4,0x2d,0x5f,0xf5,0xf0,0x09,0x3f,0xa9,0x1c,0x59,0x33,0x9f,0x28,0xf0,0x31,0xa2,
  0xeb,0x93,0x74,0x96,0x58,0xae,0x11,0x38,0x51,0x63,0xa4,0x52,0xb8,0x48,0xca,0xf0,0x94,0x4b,0xff,0x70,0x81,0xc0,0x46,0x20,
  0x00,0x14,0x65,0xf3,0x87,0x48,0x06,0x38,0x93,0xd1,0x73,0xea,0xfe,0x0c,0xe7,0x51,0x7a,0x30,0x98,0x2e,0x4f,0xfd,0x96,0xa2,
  0xa8,0x54,0x2f,0xbb,0xb4,0x47,0xc6,0xf7,0xcf,0xed,0x2f,0x39,0xb4,0x22,0xfc,0x5f,0xc6,0x1b,0xf2,0x3a,0x3e,0xf1,0x9e,0x90,
  0x1e,0xd1,0x0e,0x49,0x81,0x11,0x1e,0x08,0xbf,0x06,0x63,0xd8,0xc7,0xb0,0x9b,0x76,0x6a,0x5d,0xaa,0x77,0xff,0xea,0x4a,0xc4,
  0x10,0xf8,0xb4,0xc2,0x7b,0x96,0x41,0x43,0x86,0x5e,0x74,0xfb,0x61,0x17,0x82,0xbe,0x79,0x03,0x3a,0x9d,0x39,0x9a,0x94,0xa6,
  0x6d,0xc3,0x70,0xe7,0xc0,0xec,0x2f,0xf9,0x8a,0x8d,0xf1,0xe3,0xce,0xa3,0xb3,0x49,0x59,0x71,0x4f,0xa0,0x38,0xef,0x64,0x16,
  0x27,0x62,0xf1,0x59,0x79,0x1f,0x69,0xd7,0x61,0x53,0x32,0xf1,0x95,0xd2,0x12,0x05,0x84,0x99,0x38,0xff,0x2c,0x4e,0x17,0x8b,
  0x68,0xc5,0xbe,0x24,0x98,0x02,0x0d,0xab,0xfd,0x6c,0x20,0x31,0x7f,0x48,0xf9,0x0d,0x43,0xb7,0x9a,0x65,0x82,0x95,0xf8,0xf4,
  0xc3,0xdf,0x2d,0xf5,0xe3,0xdf,0xe9,0x22,0x68,0x87,0xea,0x56,0x95,0x28,0x6c,0x7d,0x86,0x72,0xc0,0xdd,0xde,0xfc,0x96,0x02,
  0x6d,0x87,0x8d,0x41,0x7f,0x0d,0x8f,0xeb,0xbb,0x99,0x06,0x0d,0x45,0xce,0xd3,0xfa,0x08,0x3b,0x57,0xd1,0x85,0x79,0xa0,0x50,
  0x26,0x86,0x4d,0x1e,0x53,0xe2,0x33,0x37,0x54,0xb0,0x1d,0x57,0xc5,0x2c,0x3a,0xe3,0xba,0xef,0x19,0x2e,0x7c,0xac,0x4e,0xbc,
  0xd1,0x3c,0x6f,0x6e,0x17,0x61,0x78,0x77,0x0b,0x57,0xf0,0x03,0x87,0x74,0xb5,0x17,0xa5,0x30,0xc6,0xcf,0xe2,0xce,0x41,0x5f,
  0x82,0xd1,0x1d,0xac,0x44,0x20,0x56,0x1e,0x56,0x29,0xe8,0xab,0x40,0xd1,0x6d,0x03,0xbc,0x8e,0xf7,0xe2,0xbf,0x22,0xa1,0x3c,
  0xb5,0xc0,0x3d,0x45,0x6a,0x99,0xb8,0x9a,0x22,0xa9,0x09,0xff,0x3a,0x09,0x83,0xc4,0xc0,0x4a,0xaa,0x60,0x7d,0xdf,0xa9,0x67,
  0xfb,0xa0,0x23,0x60,0x51,0xb5,0xcf,0x04,0xdc,0x7c,0x20,0x71,0x18,0xc4,0xb2,0x84,0xce,0xbf,0x14,0x33,0xe1,0x10,0x96,0xff,
  0x45,0x48,0x85,0xfb,0xe9,0x8a,0xd2,0x77,0x34,0xed,0x55,0x43,0x47,0x3f,0x90,0x6c,0xd2,0x16,0x47,0xd5,0x17,0x6c,0x5d,0xdd,
  0x93,0xad,0xe0,0xd6,0xd5,0xa3,0xd5,0x42,0xbe,0xb5,0x26,0xf4,0x54,0x4a,0xab,0x55,0x31,0x53,0x14,0x02,0x21,0xe9,0x06,0x0d,
  0xd8,0x72,0x1c,0xf4,0xa1,0x7d,0x4f,0xbb,0x5f,0x9c,0x7a,0x7e,0x71,0xe0,0x4a,0x19,0x52,0xbd,0xd9,0xef,0x3a,0x5e,0xd6,0xdb,
  0xe4,0x95,0x75,0x3c,0x46,0xaa,0x9d,0x97,0x38,0x5c,0xef,0x75,0x92,0x42,0xc3,0x8c,0x7c,0xba,0x7e,0x26,0x8d,0x79,0x88,0xab,
  0xd6,0xb5,0xfb,0x98,0xca,0x48,0x9b,0xae,0x02,0xba,0xe4,0x1f,0xeb,0xcd,0xc7,0x22,0x8f,0x94,0x46,0xe7,0x37,0x9b,0x4c,0x12,
  0xbb,0xaa,0x6d,0x57,0x14,0x23,0x52,0xbc,0xa5,0xad,0xb4,0xcb,0xa0,0x00,0xa9,0xa6,0x62,0xd8,0x0a,0x4a,0xc4,0xc6,0x3d,0x96,
  0x64,0xbe,0xaa,0x6d,0x6e,0x76,0x71,0x8b,0x79,0x2c,0x30,0x52,0x14,0x9f,0x5d,0x66,0x7d,0x58,0xfc,0x7b,0xcf,0x05,0xc2,0xed,
  0x1a,0xfd,0x73,0x1e,0x3b,0xfc,0x9c,0x9b,0x4f,0xb6,0x46,0x83,0x33,0xeb,0x23,0xa7,0x71,0xc5,0x2e,0x03,0x97,0xd1,0xc4,0x62,
  0x56,0x01,0x77,0x9a,0xb8,0x35,0xbd,0x86,0x19,0xc9,0xb5,0xa5,0x9c,0xe1,0xc4,0x87,0xa8,0x95,0xe2,0x41,0xef,0x24,0xae,0xf5,
  0x6d,0xb3,0x59,0x37,0x74,0xe8,0x83,0xa2,0x7c,0x61,0xf2,0xb5,0x98,0x58,0x1a,0xc3,0x4a,0x2e,0x17,0x26,0xbd,0xc8,0x99,0xa3,
  0x46,0xe6,0x80,0x15,0x6a,0x38,0x3d,0x34,0x00,0xe4,0x73,0x5c,0x11,0xb5,0xda,0x59,0xae,0xdf,0x4a,0xa4,0xd1,0x45,0xc9,0x83,
  0x44,0x05,0xce,0x4d,0x09,0x55,0x94,0x13,0x7a,0x8c,0x2e,0x43,0x43,0x3a,0xc6,0x44,0x65,0xe4,0x13,0x1b,0xee,0x14,0x7d,0x2a,
  0x96,0x45,0x07,0xb1,0x8c,0xad,0xac,0x09,0xbf,0xef,0x87,0xdc,0x69,0xd7,0x28,0x64,0x3e,0xd1,0xc2,0x3c,0x3d,0x57,0x70,0x76,
  0x4f,0x2f,0x25,0x44,0xf2,0x92,0x61,0x91,0x23,0x52,0x2f,0x06,0x01,0xad,0x04,0xa2,0xa6,0xbf,0x7f,0x4f,0x60,0x05,0xeb,0xe3,
  0x2c,0x01,0xad,0x99,0xcd,0xa7,0xee,0x00,0x72,0xa2,0x4b,0xc4,0x7e,0x16,0xc1,0x28,0x7d,0x04,0x70,0xff,0x0b,0xcd,0xf7,0x5f,
  0xe8,0x59,0xb4,0x6e,0x59,0xef,0x77,0x89,0x47,0xf5,0x75,0x6b,0x51,0xb7,0xca,0x43,0xf5,0xc9,0x85,0x8b,0x93,0x16,0x5a,0x77,
  0x45,0x5d,0x01,0xa0,0x0c,0xdc,0x15,0xe8,0xcf,0xf6,0xa3,0xe4,0xa2,0xcb,0x65,0x41,0xa5,0xe7,0x1c,0x95,0x4c,0x95,0xc2,0xee,
  0x7f,0x08,0x88,0xe4,0x57,0x91,0xb7,0x77,0x48,0x73,0xf9,0xed,0xfb,0x12,0x34,0x7f,0x71,0x53,0x7a,0x3c,0x74,0x9e,0x60,0xfb,
  0x6c,0x23,0xbd,0xc3,0x9a,0x7a,0x7d,0xbd,0x1f,0x0a,0x50,0xa4,0x58,0xa8,0x79,0xb3,0xe0,0x95,0x31,0xc8,0xc3,0xd2,0x41,0x08,
  0x30,0x9b,0x81,0x72,0x06,0x72,0x64,0x32,0x94,0x1c,0xd0,0xa3,0x98,0xba,0xf9,0x32,0x59,0x91,0x65,0xdf,0x89,0xf0,0xac,0x64,
  0x28,0xcb,0xd5,0x00,0x51,0x6d,0xa9,0xb9,0x2f,0xec,0x6d,0xaf,0xf1,0x11,0x8e,0x3f,0x0b,0xac,0x8c,0x7b,0x89,0x09,0x5c,0x6d,
  0x2f,0x3a,0xe0,0xcd,0xbb,0xf8,0xd8,0xa1,0x15,0x5a,0xb9,0xe7,0x74,0x2e,0x3e,0xd0,0xcf,0x29,0x4e,0xed,0x31,0xcf,0xf2,0x8a,
  0x37,0xd4,0x09,0x1f,0x91,0x12,0x9b,0x1d,0xec,0xa9,0x19,0x61,0xad,0x39,0x46,0x5c,0xaa,0x10,0x85,0x49,0xf6,0xb3,0x8c,0xc6,
  0x40,0xf6,0xa9,0x00,0x58,0x04,0x12,0x86,0xd8,0xd7,0xef,0x72,0x01,0x88,0x17,0x9a,0xdb,0xb4,0x3f,0xf1,0x91,0xd0,0x77,0x8a,
  0xba,0x80,0x6c,0xda,0x75,0x91,0x7a,0xa4,0xd1,0xb6,0x47,0x75,0x8d,0x75,0xe1,0x22,0x1e,0x99,0xaf,0x2f,0x00,0xc2,0xef,0xc8,
  0xd4,0xaa,0x38,0x2e,0x9e,0xb7,0xf3,0x66,0x7d,0x4d,0x75,0x39,0xc1,0xe3,0x42,0xb8,0x33,0x06,0xbf,0xf8,0xa5,0xa8,0x06,0x36,
  0x5a,0x74,0x1c,0xb2,0x8c,0x05,0x60,0x5e,0x49,0x2b,0x93,0x52,0xae,0x5a,0x42,0x05,0x7c,0x78,0x4e,0x11,0x7f,0xf3,0xbc,0x2b,
  0xb3,0x7d,0xcc,0xd5,0xc5,0x58,0xc7,0x0e,0xf8,0x18,0xa4,0xc7,0x5e,0xb2,0x8c,0xec,0x80,0xe6,0x90,0xde,0xb4,0x36,0x3a,0xeb,
  0xf9,0x72,0x42,0xef,0x3d,0x46,0x32,0xd1,0x7b,0x33,0x04,0x50,0xac,0xcf,0x03,0x95,0xbe,0x50,0xc5,0x54,0xbd,0x15,0x4a,0x56,
  0x4c,0xbe,0x56,0x26,0xec,0x0f,0x66,0x85,0x4c,0x3e,0xbf,0xe7,0x08,0x94,0x57,0x05,0x3f,0xdf,0x23,0x21,0xa6,0xae,0x60,0xf9,
  0x3d,0x8f,0x32,0xcb,0xd7,0x70,0xa7,0xfb,0x0b,0x54,0x06,0x5f,0x2a,0x53,0xd1,0xdf,0x79,0x44,0xb1,0xd6,0xdd,0xdc,0x1e,0x6c,
  0x1d,0x33,0xd3,0x19,0x09,0x33,0x98,0x7c,0x0d,0x3a,0xd6,0x30,0xab,0x6d,0x49,0x1e,0x27,0xcf,0xbd,0xb3,0x67,0x2d,0x5a,0x2f,
  0x16,0xaf,0xf5,0x1f,0x5e,0xed,0x7b,0x5c,0xc6,0xd0,0xfa,0x2e,0xb0,0x73,0x83,0x79,0x37,0xc9,0x66,0x05,0xc7,0x80,0x16,0xe3,
  0x81,0x9e,0x74,0x05,0xfa,0x6c,0x14,0x44,0x51,0x90,0xf8,0x45,0xd2,0x41,0x84,0x00,0x2d,0xec,0xe7,0xa5,0xd9,0xc1,0x9f,0xc7,
  0x39,0x92,0xd2,0x80,0xfd,0x5d,0xee,0x1b,0xf7,0xc4,0x2b,0x8b,0xb5,0xa6,0xd8,0x35,0x07,0xf0,0xcb,0x4a,0x03,0xd7,0xa2,0xd3,
  0x67,0x50,0x9f,0x13,0x4a,0x13,0x63,0xd6,0x05,0x8b,0x53,0x1a,0x26,0xf8,0xdf,0x84,0x86,0x74,0x37,0xdc,0xf5,0x3f,0xd2,0x9b,
  0x18,0x56,0xdc,0x91,0x29,0x32,0x67,0xff,0xea,0x80,0x6b,0x45,0x01,0x15,0x0e,0x99,0xf5,0xc5,0x5e,0xff,0xe2,0x03,0x52,0xf6,
  0x0c,0x1f,0xcc,0xc5,0x39,0x83,0xba,0xa0,0x3e,0xa6,0xca,0xe8,0x66,0xcf,0x33,0x33,0x56,0xcf,0xf3,0x48,0x10,0xd2,0xd5,0x05,
  0x42,0xae,0xe0,0x52,0x27,0xfb,0x06,0x96,0x20,0xbd,0x00,0x1c,0x8b,0xac,0x47,0x5a,0x36,0x4c,0x5e,0xd0,0x5b,0xa5,0x0e,0xa5,
  0x29,0xbf,0x04,0x85,0xca,0x04,0x8e,0x7e,0xc0,0x72,0x5b,0x62,0x43,0xae,0x3a,0xca,0x56,0x6b,0xb6,0x31,0x25,0xdd,0x1d,0x5c,
  0x8e,0x89,0xfd,0x29,0xcd,0x60,0x5a,0xbc,0x65,0xa3,0xcb,0xea,0xb5,0xd2,0x49,0x6a,0x07,0xb1,0xf3,0x6b,0x26,0xe6,0x3f,0x40,
  0x3f,0x55,0x70,0x39,0x66,0x9f,0xfd,0x1c,0x31,0x86,0x83,0xe7,0xdf,0x0a,0x64,0xad,0x1e,0x26,0x88,0x53,0x96,0xdc,0x93,0xd6,
  0xc7,0x68,0x30,0x31,0x51,0x3b,0xe4,0x27,0x5d,0x37,0xbe,0xb0,0xb4,0x7b,0xa1,0x65,0x76,0x7b,0x62,0xc1,0xef,0xa2,0x14,0x76,
  0xa7,0x90,0xd5,0x34,0x0e,0x92,0x1e,0x95,0x04,0x3d,0x97,0x8a,0xe2,0xe2,0xad,0x22,0x6d,0x70,0x05,0x46,0xf1,0xc8,0x05,0xbf,
  0xcc,0x47,0x63,0xe7,0x1b,0x70,0xdc,0x8e,0xcc,0x99,0xa0,0x9c,0xb3,0xe0,0x86,0x24,0x1c,0x3c,0x8d,0x86,0x8d,0xd4,0x40,0xc0,
  0x66,0x0b,0xfb,0x7f,0xb2,0xb9,0x7d,0x5a,0x30,0xd2,0x2c,0xe0,0x95,0x78,0x4a,0x17,0xcd,0x70,0xee,0x7a,0x53,0x86,0x8c,0xb2,
  0x22,0x0e,0xe3,0xb0,0x9d,0xe8,0xb0,0x4f,0x42,0xb8,0x97,0x91,0x4d,0x3f,0x11,0x4a,0x8e,0x96,0x7d,0x7f,0xed,0x11,0x35,0x8e,
  0xeb,0xac,0xa2,0xcf,0x7f,0x9a,0xb4,0x53,0x3e,0x61,0x9a,0x67,0xae,0x74,0xac,0x95,0x05,0xa3,0x4a,0xa7,0xed,0x56,0xc8,0x2f,
  0x42,0x24,0xf4,0x37,0x48,0xc6,0x6a,0x69,0x6d,0xb6,0x1f,0xdc,0x8c,0x27,0x93,0xc5,0x8f,0xd3,0xe6,0x2b,0x30,0x71,0x8f,0x0c,
  0xe3,0xf1,0x54,0x59,0x28,0x35,0xd8,0xd3,0x80,0x69,0xbc,0xf9,0x77,0xaa,0x25,0x5b,0x5b,0xb1,0xf5,0xd3,0x25,0x91,0x45,0xa9,
  0x1c,0x33,0xdd,0x5a,0x5d,0x44,0xe1,0xe8,0xbd,0x96,0x76,0x5e,0x95,0x1b,0x7b,0x22,0x6f,0xab,0x83,0xd9,0x63,0xfa,0x1a,0xe9,
  0x4b,0x1f,0xf1,0xf8,0xd4,0xf0,0x1b,0xab,0x79,0x78,0xe3,0x70,0x16,0x29,0xbe,0x56,0x82,0x93,0x3f,0x46,0x33,0xb4,0x68,0xe1,
  0xec,0x98,0x3d,0x32,0xa8,0xfb,0x08,0x46,0x24,0xbd,0xb2,0xef,0xde,0xdd,0x48,0x77,0x4b,0xf2,0xa8,0xd3,0xf6,0xac,0x53,0xb6,
  0xfd,0xaa,0xf0,0xdc,0x3b,0x16,0x65,0xb0,0x0a,0x0f,0x65,0x86,0xde,0x0b,0xdc,0xe6,0xcf,0x18,0x07,0x1b,0x61,0x8c,0x9c,0xd7,
  0xf7,0xbc,0x01,0xd4,0x3d,0xad,0xf0,0x5e,0x9c,0x2e,0x00,0x5a,0x38,0x00,0x00,0x11,0x4c,0xa8,0x12,0x9c,0xfa,0xd8,0xc4,0xd7,
  0x4c,0x51,0xbf,0x80,0x7a,0x91,0x7c,0x82,0xd8,0xfb,0xdf,0xa4,0xc0,0x0f,0x46,0x4f,0x96,0xfa,0x89,0x01,0x5f,0xfe,0xed,0xe2,
  0x8a,0x38,0xb1,0xe0,0x70,0x51,0x68,0x97,0x63,0xb7,0xcb,0x0e,0xd5,0x81,0x75,0xff,0x77,0x5c,0xf8,0x80,0xe0,0x60,0xc3,0xad,
  0x22,0x2d,0xfd,0xaa,0xbc,0x2a,0x0f,0xef,0x84,0x2b,0x3b,0x6e,0xf4,0x8c,0x53,0x91,0xbe,0x25,0x4a,0xa2,0xe6,0x69,0x1d,0x57,
  0xdc,0xcd,0x6f,0x3c,0x6d,0x28,0xae,0x75,0x56,0x2f,0x24,0x92,0xcf,0xf9,0x05,0x4e,0x97,0xe0,0x8e,0xb3,0x45,0x58,0x31,0x69,
  0xd3,0x89,0x93,0x60,0x69,0xe7,0x17,0xc3,0x11,0x4f,0xd0,0x4f,0xfc,0xa7,0xda,0xcd,0x23,0xd2,0xbc,0x7a,0xd5,0x10,0xae,0x42,
  0xf1,0xe5,0x36,0xbc,0xb6,0x54,0x6f,0x0d,0x1c,0x65,0x3e,0xc0,0xef,0x15,0xd2,0x87,0x56,0xff,0x3e,0x23,0x27,0x3d,0xc9,0x7a,
  0x22,0xe7,0x8e,0x46,0x02,0x97,0xc6,0xf3,0xd9,0x4e,0x51,0x14,0x01,0xad,0x79,0xd6,0x3f,0x15,0xeb,0x55,0x25,0x23,0xd2,0x29,
  0x51,0xf6,0xbd,0x67,0xc3,0xbc,0x9d,0xdf,0xc5,0xb6,0x82,0xeb,0x41,0x20,0xc0,0x66,0x0c,0x84,0x29,0x18,0x42,0x21,0x83,0x88,
  0x05,0x57,0x24,0x2a,0xf2,0x60,0xec,0x08,0x62,0x59,0x9a,0x75,0xe3,0x58,0xd2,0xbe,0x20,0x2b,0x3e,0x2e,0x3c,0xaa,0x39,0xfa,
  0x17,0x6e,0xd2,0xa0,0x3a,0x31,0x51,0x1b,0x73,0xdc,0xf2,0xf9,0x34,0xd8,0xb0,0xb7,0x2f,0x7b,0xc6,0xde,0x8f,0xee,0xcd,0xcb,
  0xbd,0x78,0x19,0x76,0xd4,0x0f,0x5b,0xf8,0xc1,0x1b,0xc8,0x97,0x2d,0x4d,0x36,0x3e,0xf9,0xd7,0x0f,0xef,0x80,0x2d,0x1c,0xea,
  0xac,0x97,0x4f,0xc8,0x40,0x2f,0xbd,0xf4,0xf0,0x7c,0xf6,0x72,0x33,0x5a,0xfa,0x6b,0x58,0x8c,0xcb,0x38,0xc1,0xeb,0xe7,0xc0,
  0xe7,0x6b,0x8d,0x9f,0x98,0xad,0xd3,0x9b,0x35,0xde,0x7e,0x1d,0xd6,0x56,0xae,0x2d,0x55,0xcd,0x70,0x8e,0x2d,0xd6,0x35,0x54,
  0xca,0xd6,0x69,0xe3,0x19,0x0b,0xc6,0xdc,0x77,0x1c,0x1c,0x88,0xcb,0xad,0x97,0x66,0x67,0x3c,0x31,0x24,0x57,0x46,0xbd,0x7b,
  0x51,0x68,0x0b,0x8c,0xe5,0x12,0x20,0x1e,0x0b,0xb9,0xf6,0x6b,0xdf,0xc1,0x8d,0x5d,0xf7,0x06,0xe9,0xff,0x6f,0x5b,0x65,0xe5,
  0x01,0x57,0x2b,0x23,0x7a,0x78,0xf9,0x98,0xc3,0x09,0x4e,0xde,0x92,0xe6,0xc9,0xa9,0xa1,0xaa,0xe7,0xf1,0xe9,0x7c,0xd5,0xa6,
  0x21,0xca,0xc7,0xd8,0x28,0x8b,0x05,0x76,0xa1,0xcf,0xf3,0x36,0xf6,0xd3,0x3a,0x5a,0xa2,0x63,0x80,0xfd,0xf8,0x0f,0xd4,0x81,
  0xf4,0xfe,0x29,0xab,0xb8,0x4b,0xbe,0x16,0xb0,0xb0,0x70,0x80,0x00,0x00,0x00,0xc0,0x49,0xfa,0x17,0x6b,0x9f,0xe2,0x32,0xb2,
  0xf2,0x64,0x7d,0x4b,0x8a,0x24,0x7d,0x03,0x59,0x26,0x55,0xb5,0xf7,0x20,0x01,0x30,0x3a,0xe3,0x97,0xfa,0xba,0x7a,0x3f,0x7a,
  0xf6,0xd7,0xdc,0x05,0x6c,0x34,0xbe,0x58,0x00,0x00,0x00,0x00,0x00,0x00
};

void handleLogo()
{
  // The phone keeps it for a week, so it is only sent once
  server.sendHeader("Cache-Control", "max-age=604800");
  server.send_P(200, "image/webp", (const char*)LOGO_WEBP, sizeof(LOGO_WEBP));
}

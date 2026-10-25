static portMUX_TYPE bleLock = portMUX_INITIALIZER_UNLOCKED;   // phone writes vs loop()

// One line per phone / watch: each gets only its own answers, in pieces of
// its own size, at the speed its own link takes them (Bluedroid, the ESP32-S3).
// Before, every answer went to both, in pieces as small as the smaller MTU,
// as slow as the slower link: the phone's live graph lagged and the watch
// waited for answers behind the phone's.
#if defined(CONFIG_BLUEDROID_ENABLED) || defined(BLE_MOCK_LINES)
#define BLE_LINES 1
#include <esp_gatt_common_api.h>        // esp_ble_get_cur_sendable_packets_num
#else
#define BLE_LINES 0
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

  void on(const char* path, Handler h)
  {
    if(routeCount < MAX_ROUTES)
    {
      routes[routeCount].path = path;
      routes[routeCount].handler = h;
      routeCount++;
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

#if BLE_LINES
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

    // pieces: each line at its own pace
    for(int i = 0; i < MAX_LINES; i++)
      sendPiece(lines[i], nowMs);
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
  static const int MAX_ROUTES = 40;
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
  };

  Line lines[MAX_LINES] = {};
  int turn = 0;
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
        break;
      }
    }

    portEXIT_CRITICAL(&bleLock);
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

  void sendPiece(Line& L, unsigned long nowMs)
  {
    if(!L.used || L.outPos >= L.out.length())
    {
      L.blockedSince = 0;
      return;
    }

    // sending to all: one pace for everyone, the slowest line's
    unsigned long gap = L.gapUs;

    if(broadcast)
      for(int i = 0; i < MAX_LINES; i++)
        if(lines[i].used && lines[i].gapUs > gap)
          gap = lines[i].gapUs;

    if(micros() - (broadcast ? lastShared : L.lastPiece) < gap)
      return;

    // all well for 2 s: a bit faster again
    if(L.gapUs > PIECE_GAP_US && nowMs - L.calmSince > 2000)
    {
      L.gapUs = L.gapUs / 2 > PIECE_GAP_US ? L.gapUs / 2 : PIECE_GAP_US;
      L.calmSince = nowMs;
    }

#if BLE_LINES
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
        return;

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
#if BLE_LINES
    else
    {
      esp_err_t rc = esp_ble_gatts_send_indicate(gattsIf, L.id, tx->getHandle(), n, (uint8_t*)data, false);

      if(rc != ESP_OK)
      {
        Serial.printf("Bluetooth: sending to device %d failed (error %d)\n", L.id, (int)rc);

        if(++sendFails >= 3)
        {
          broadcast = true;            // the old way: to everyone (it worked before)
          Serial.println("Bluetooth: sending to all devices from now on");
        }

        L.lastPiece = micros();
        return;                        // the same piece again next time
      }

      sendFails = 0;
    }
#endif

    L.outPos += n;
    L.lastPiece = micros();
    lastShared = L.lastPiece;

    if(L.outPos >= L.out.length())
    {
      L.out = "";
      L.outPos = 0;
    }
  }

#if BLE_LINES
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
      owner->lineOpened(0);            // (no connection numbers: one shared line)
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

#if BLE_LINES
    // (the library calls both onConnect versions)
    void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override
    {
      owner->lineOpened(p->connect.conn_id);
    }

    void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override
    {
      owner->lineClosed(p->disconnect.conn_id);
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

#if BLE_LINES
    // only a paired (encrypted) device can write: this one is ours
    void onWrite(BLECharacteristic* c, esp_ble_gatts_cb_param_t* p) override
    {
      write(c, p->write.conn_id);
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

#if BLE_LINES
BleServer* BleServer::me = nullptr;
#endif

BleServer server;

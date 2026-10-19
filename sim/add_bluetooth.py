#!/usr/bin/env python3
"""Bluetooth (BLE) instead of WiFi.

The rod no longer makes a WiFi hotspot. The phone app talks to it over
Bluetooth Low Energy with the same requests the web page used (/data,
/strike, /set?k=..&v=.., /reelin ...), so every handler and all of the
detection stay exactly as they were. The dashboard page lives in the app.

Protocol (service 6e8a0001-...):
  RX  (write)  : "<id>|<path>?<query>\\n"
  TX  (notify) : "#<id> <code> <length>\\n<body>", split into MTU-sized pieces
  "/mtu?v=N" tells the rod how big a piece may be.

Uses the BLE library that comes with the ESP32 board package (no install).
The host simulator keeps its WebServer stand-in.

    python3 add_bluetooth.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new):
    global s
    n = s.count(old)
    if n != 1:
        sys.exit(f"{path}: expected 1 x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''#include <WiFi.h>
#include <WebServer.h>
''', '''#if defined(ESP32)
#include <BLEDevice.h>
#include <BLEServer.h>
#if !defined(CONFIG_NIMBLE_ENABLED)
#include <BLE2902.h>
#endif
#else
#include <WebServer.h>        // host simulator
#endif
''')

rep('''WebServer server(80);
''', r'''// =====================================================
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

class BleServer
{
public:
  typedef void (*Handler)();

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
    BLEDevice::init("StrikeSense");
    BLEDevice::setMTU(517);

    BLEServer* srv = BLEDevice::createServer();
    srv->setCallbacks(new Callbacks(this));

    BLEService* service = srv->createService(BLE_SERVICE_UUID);

    BLECharacteristic* rx = service->createCharacteristic(
      BLE_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new RxCallbacks(this));

    tx = service->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);

#if !defined(CONFIG_NIMBLE_ENABLED)
    tx->addDescriptor(new BLE2902());    // older ESP32 boards (Bluedroid); NimBLE adds it by itself
#endif

    service->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();
  }

  bool connected() const
  {
    return isConnected;
  }

  // Called from loop(): answers one request at a time, sends the answer
  // in small pieces so the Bluetooth stack is never flooded
  void handleClient()
  {
    if(restartAdvertising)
    {
      restartAdvertising = false;
      out = "";
      outPos = 0;
      chunk = 20;
      BLEDevice::startAdvertising();
    }

    if(outPos < out.length())
    {
      if(isConnected && micros() - lastPiece >= PIECE_GAP_US)
      {
        size_t n = out.length() - outPos;

        if(n > chunk)
          n = chunk;

        tx->setValue((uint8_t*)out.c_str() + outPos, n);
        tx->notify();

        outPos += n;
        lastPiece = micros();
      }

      if(outPos >= out.length() || !isConnected)
      {
        out = "";
        outPos = 0;
      }

      return;
    }

    char line[REQ_MAX];

    if(!popLine(line))
      return;

    dispatch(line);
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
  static const int MAX_ROUTES = 24;
  static const int MAX_ARGS = 6;
  static const int REQ_QUEUE = 8;
  static const int REQ_MAX = 200;
  static const unsigned long PIECE_GAP_US = 4000;

  struct Route
  {
    const char* path;
    Handler handler;
  };

  Route routes[MAX_ROUTES];
  int routeCount = 0;

  BLECharacteristic* tx = nullptr;
  volatile bool isConnected = false;
  volatile bool restartAdvertising = false;

  // Lines written by the phone (Bluetooth task) -> loop()
  char queue[REQ_QUEUE][REQ_MAX];
  int qHead = 0;
  int qTail = 0;
  char partial[REQ_MAX];
  int partialLen = 0;

  // Answer being sent
  String out;
  size_t outPos = 0;
  size_t chunk = 20;
  unsigned long lastPiece = 0;
  String requestId = "0";

  String argNames[MAX_ARGS];
  String argValues[MAX_ARGS];
  int argCount = 0;

  class Callbacks : public BLEServerCallbacks
  {
  public:
    Callbacks(BleServer* s) : owner(s) {}

    void onConnect(BLEServer*) override
    {
      owner->isConnected = true;
      Serial.println("Bluetooth: phone connected");
    }

    void onDisconnect(BLEServer*) override
    {
      owner->isConnected = false;
      owner->restartAdvertising = true;
      Serial.println("Bluetooth: phone disconnected");
    }

  private:
    BleServer* owner;
  };

  class RxCallbacks : public BLECharacteristicCallbacks
  {
  public:
    RxCallbacks(BleServer* s) : owner(s) {}

    void onWrite(BLECharacteristic* c) override
    {
      auto v = c->getValue();

      for(size_t i = 0; i < v.length(); i++)
        owner->received(v[i]);
    }

  private:
    BleServer* owner;
  };

  void received(char ch)
  {
    portENTER_CRITICAL(&bleLock);

    if(ch == '\n')
    {
      int next = (qHead + 1) % REQ_QUEUE;

      if(next != qTail)
      {
        memcpy(queue[qHead], partial, partialLen);
        queue[qHead][partialLen] = 0;
        qHead = next;
      }

      partialLen = 0;
    }
    else if(ch != '\r' && partialLen < REQ_MAX - 1)
    {
      partial[partialLen++] = ch;
    }

    portEXIT_CRITICAL(&bleLock);
  }

  bool popLine(char* line)
  {
    bool got = false;

    portENTER_CRITICAL(&bleLock);

    if(qTail != qHead)
    {
      memcpy(line, queue[qTail], REQ_MAX);
      qTail = (qTail + 1) % REQ_QUEUE;
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

    // The app says how big one Bluetooth piece may be
    if(path == "/mtu")
    {
      int v = arg("v").toInt();
      chunk = v < 20 ? 20 : (v > 512 ? 512 : v);
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

    send(404, "text/plain", "NOT FOUND");
  }

  void reply(int code, const char* body, size_t length)
  {
    if(!isConnected)
      return;

    out = "#" + requestId + " " + String(code) + " " + String((unsigned long)length) + "\n";
    out.concat(body, length);
    outPos = 0;
  }
};

BleServer server;

#else

WebServer server(80);     // host simulator

#endif
''')

rep('''const char* apName = "StrikeSense-Test";
const char* apPassword = "12345678";
''', '')

rep('''  WiFi.softAP(apName, apPassword);

  Serial.println();
  Serial.print("Dashboard: http://");
  Serial.println(WiFi.softAPIP());
''', '''  Serial.println();
  Serial.println("Bluetooth: \\"StrikeSense\\" - open the StrikeSense app on the phone");
''')

# Dashboard header: says Bluetooth (the app adds the signal strength)
rep('''192.168.4.1
''', '''<span id="linkInfo">Bluetooth</span>
''')

open(path, 'w').write(s)
print("ok", path)

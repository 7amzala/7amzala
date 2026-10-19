#!/usr/bin/env python3
"""Watch over Bluetooth: the rod takes 2 connections (phone + watch).

- Both get every answer (BLE notifications go to all); each side only
  keeps answers to its own requests (phone ids below 500000, watch ids
  from 500000).
- After one phone/watch connects the rod keeps advertising, so the other
  can join. Pieces are as small as the smallest MTU announced.
- "/watch": a small answer with only what the watch shows. It keeps its
  own knock peak, so the phone's /data never misses a knock.

Run after add_bluetooth.py.

    python3 add_watch_link.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# ---------- BleServer: several connections ----------

rep('''  bool connected() const
  {
    return isConnected;
  }''', '''  bool connected() const
  {
    return clients > 0;
  }''')

rep('''    if(restartAdvertising)
    {
      restartAdvertising = false;
      out = "";
      outPos = 0;
      chunk = 20;
      BLEDevice::startAdvertising();
    }

    if(outPos < out.length())
    {
      if(isConnected && micros() - lastPiece >= PIECE_GAP_US)''', '''    // Keep advertising while there is room for another phone/watch
    if(advertiseAgain)
    {
      advertiseAgain = false;

      if(clients < MAX_CLIENTS)
        BLEDevice::startAdvertising();
    }

    if(clients == 0 && (out.length() > 0 || chunkKnown))
    {
      out = "";
      outPos = 0;
      chunk = 20;
      chunkKnown = false;
    }

    if(outPos < out.length())
    {
      if(clients > 0 && micros() - lastPiece >= PIECE_GAP_US)''')

rep('''      if(outPos >= out.length() || !isConnected)''', '''      if(outPos >= out.length() || clients == 0)''')

rep('''  BLECharacteristic* tx = nullptr;
  volatile bool isConnected = false;
  volatile bool restartAdvertising = false;
''', '''  static const int MAX_CLIENTS = 2;         // phone + watch

  BLECharacteristic* tx = nullptr;
  volatile int clients = 0;
  volatile bool advertiseAgain = false;
  bool chunkKnown = false;
''')

rep('''    void onConnect(BLEServer*) override
    {
      owner->isConnected = true;
      Serial.println("Bluetooth: phone connected");
    }

    void onDisconnect(BLEServer*) override
    {
      owner->isConnected = false;
      owner->restartAdvertising = true;
      Serial.println("Bluetooth: phone disconnected");
    }''', '''    void onConnect(BLEServer*) override
    {
      owner->clients = owner->clients + 1;
      owner->chunk = 20;               // until the newcomer says its MTU
      owner->advertiseAgain = true;
      Serial.printf("Bluetooth: connected (%d)\\n", (int)owner->clients);
    }

    void onDisconnect(BLEServer*) override
    {
      if(owner->clients > 0)
        owner->clients = owner->clients - 1;

      owner->advertiseAgain = true;
      Serial.printf("Bluetooth: disconnected (%d left)\\n", (int)owner->clients);
    }''')

rep('''    // The app says how big one Bluetooth piece may be
    if(path == "/mtu")
    {
      int v = arg("v").toInt();
      chunk = v < 20 ? 20 : (v > 512 ? 512 : v);
      send(200, "text/plain", "OK");
      return;
    }''', '''    // A phone/watch says how big one Bluetooth piece may be.
    // Everyone gets every piece, so use the smallest.
    if(path == "/mtu")
    {
      int v = arg("v").toInt();
      v = v < 20 ? 20 : (v > 512 ? 512 : v);

      if(!chunkKnown || (size_t)v < chunkMin)
        chunkMin = v;

      chunkKnown = true;
      chunk = chunkMin;
      send(200, "text/plain", "OK");
      return;
    }''')

rep('''  size_t chunk = 20;
''', '''  size_t chunk = 20;
  size_t chunkMin = 512;
''')

rep('''  void reply(int code, const char* body, size_t length)
  {
    if(!isConnected)
      return;''', '''  void reply(int code, const char* body, size_t length)
  {
    if(clients == 0)
      return;''')

# the min MTU is per session: forget it when everyone has gone
rep('''      chunk = 20;
      chunkKnown = false;
    }''', '''      chunk = 20;
      chunkKnown = false;
      chunkMin = 512;
    }''')

# ---------- /watch: small answer for the watch ----------

rep('''float energyPeakSincePoll = 0;''', '''float energyPeakSincePoll = 0;
float energyPeakSinceWatch = 0;          // same, for the watch (its own poll)''')

rep('''  if(knockEnergy > energyPeakSincePoll)
    energyPeakSincePoll = knockEnergy;''', '''  if(knockEnergy > energyPeakSincePoll)
    energyPeakSincePoll = knockEnergy;

  if(knockEnergy > energyPeakSinceWatch)
    energyPeakSinceWatch = knockEnergy;''')

rep('''void handleData()
{''', '''// Only what the watch shows (much smaller than /data)
void handleWatch()
{
  String json;
  json.reserve(320);

  json += "{\\"state\\":\\"";
  json += stateName();
  json += "\\",\\"type\\":\\"";
  json += strikeType;
  json += "\\"";

  addField(json, "strike", strikeActive);
  addField(json, "strikeid", (long)strikeEventID);
  addField(json, "knockid", (long)knockAlertID);
  addField(json, "energy", energyPeakSinceWatch, 4);
  energyPeakSinceWatch = 0;
  addField(json, "knockthr", knockThreshold, 4);
  addField(json, "tilt", tiltDeg, 2);
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

  json += "}";

  server.send(200, "application/json", json);
}

void handleData()
{''')

rep('''  server.on("/data", handleData);''', '''  server.on("/data", handleData);
  server.on("/watch", handleWatch);''')

open(path, 'w').write(s)
print("ok", path)

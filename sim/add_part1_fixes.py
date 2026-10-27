#!/usr/bin/env python3
"""Rod: watch signal bars, LoRa answers, fresh rod after a new upload.

- Watch on Bluetooth showed grey bars in the app: the rod only used what
  the watch said (0 = unknown, and a /watch question without it reset it
  to 0). Now a question without "r" changes nothing, and when the watch
  does not know, the rod measures the watch's link itself (NimBLE).
- LoRa: a watch packet with a lower number than the last one was thrown
  away as a copy - after the watch restarted that could be every packet.
  Only a copy of a recent packet is refused now.
- LoRa: listen before talk saying "busy" 3 times in a row: send anyway (a
  check that always said "busy" held every heartbeat back).
- A new sketch uploaded (not a restart): a fresh rod. Settings back to
  default (link Auto, channel Auto, sensitivity...), paired phone and
  watch forgotten (pair again with the PIN), new LoRa key. Kept: the PIN,
  the strike log, the total time and the sensor's noise calibration.

    python3 add_part1_fixes.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()
lora = 'void handlePacket(const uint8_t* buf, size_t len)' in s


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# ---------- the program's fingerprint (fresh after upload) ----------
rep('''#include <Preferences.h>
''', '''#include <Preferences.h>
#if defined(ESP32) && __has_include("esp_app_desc.h")
#include "esp_app_desc.h"
#endif
''')

# ---------- watch signal bars ----------
rep('''int watchBtRssi = 0;                    // how strong the watch hears the rod (dBm, 0 = unknown)
''', '''int watchBtRssi = 0;                    // how strong the watch hears the rod (dBm, 0 = unknown)
int watchBtConn = -1;                   // the watch's Bluetooth connection (the rod can measure it too)

// what the watch said about the signal (only when it says something)
void noteWatchHere()
{
  watchBtAt = millis();
#if defined(ESP32)
  watchBtConn = server.askerConn();

  if(server.hasArg("r"))
    watchBtRssi = server.arg("r").toInt();
#else
  watchBtRssi = server.arg("r").toInt();
#endif
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
''')

rep('''void handleWatchPing()
{
  watchBtAt = millis();
  watchBtRssi = server.arg("r").toInt();''', '''void handleWatchPing()
{
  noteWatchHere();''')

rep('''void handleWatch()
{
  watchBtAt = millis();
  watchBtRssi = server.arg("r").toInt();''', '''void handleWatch()
{
  noteWatchHere();''')

# the fight screen changed: phone and watch hear it now, not at the next tick
rep('''void setup()
{
  Serial.begin(115200);
''', '''void landNews()
{
#if defined(ESP32)
  server.pushLive();
#endif
#if defined(ENABLE_LORA) && ENABLE_LORA
  lastHeartbeatSent = 0;                // LoRa: the next heartbeat goes now
#endif
}

void setup()
{
  Serial.begin(115200);
''')

rep('''  addField(json, "wbars", (long)(bt ? rssiBars(watchBtRssi) : loraBarsNow));''',
    '''  addField(json, "wbars", (long)(bt ? rssiBars(watchSignal()) : loraBarsNow));''')

# the watch on Bluetooth: its ping in the last 6 s, or its link still up
# (pinged in the last minute) - a busy link must not show "not connected"
rep('''  bool bt = watchBtAt != 0 && millis() - watchBtAt < 6000;      // it pings every 2 s''',
    '''  bool bt = watchOnBluetooth();''')

rep('''// the watch's signal: what it said, else what the rod measures''', '''// the watch is on Bluetooth: its ping in the last 6 s, or its link still up
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

// the watch's signal: what it said, else what the rod measures''')

# ---------- LoRa ----------
if lora:
    # our rod's number but the wrong key: our own watch with an old key (the
    # rod started fresh) - not another rod (that started crowd mode)
    rep('''  if(w.rod != rodTag || memcmp(mac, buf + sizeof(w) + n, SS_MAC_LEN) != 0)
  {
    noteOtherRod(w.rod, loraAirtimeMs(len));      // not ours: only counted (crowd)
    return;
  }''', '''  if(w.rod != rodTag || memcmp(mac, buf + sizeof(w) + n, SS_MAC_LEN) != 0)
  {
    static unsigned long oldKeySaid = 0;

    if(w.rod == rodTag && w.from == 1)
    {
      if(millis() - oldKeySaid > 30000)
      {
        oldKeySaid = millis();
        Serial.println("[LoRa] our watch has an old key: it gets the new one when it connects over Bluetooth");
      }

      return;
    }

    noteOtherRod(w.rod, loraAirtimeMs(len));      // not ours: only counted (crowd)
    return;
  }''')

    # Bluetooth only: LoRa goes quiet only once the watch is really on
    # Bluetooth (the old link is kept until the new one works)
    rep('''  if(linkMode == 1 && (long)(now - loraQuietFrom) >= 0)
  {''', '''  if(linkMode == 1 && (long)(now - loraQuietFrom) >= 0 && watchOnBluetooth())
  {''')

    # crowd mode: the watch answers every 15 s, so 15 s without it is not "gone"
    rep('''  return loraOk && watchLastSeen != 0 && millis() - watchLastSeen < 15000;''',
        '''  return loraOk && watchLastSeen != 0 && millis() - watchLastSeen < (crowd ? 40000UL : 15000UL);''')

    rep('''  if(w.from != 1 || (lastWatchCtr != 0 && w.ctr <= lastWatchCtr) || n > 64)
    return;                                       // not from our watch, or a copy of an old packet''',
        '''  // a copy of a recent packet is refused; a much lower number = the watch
  // restarted (its numbers start again), accepted
  if(w.from != 1 || (lastWatchCtr != 0 && w.ctr <= lastWatchCtr && lastWatchCtr - w.ctr < 0x100000u) || n > 64)
    return;                                       // not from our watch, or a copy of an old packet''')

    rep('''  // listen before talk: another LoRa packet on the air? wait a moment
  int16_t cad = radio.scanChannel();
  loraIrq = false;

  if(cad == RADIOLIB_LORA_DETECTED)
  {''', '''  // listen before talk: another LoRa packet on the air? wait a moment.
  // "Busy" 3 times in a row: send anyway (a check that always says busy
  // must not hold everything back)
  static uint8_t busyRow = 0;
  int16_t cad = radio.scanChannel();
  loraIrq = false;

  if(cad != RADIOLIB_LORA_DETECTED || busyRow >= 3)
    busyRow = 0;
  else
  {
    busyRow++;''')

# ---------- fresh rod after a new upload ----------
rep('''void setup()
{
  Serial.begin(115200);

  delay(1000);
''', '''// A new sketch uploaded (not a restart): a fresh rod - settings back to
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
''')

rep('''  server.begin();
''', '''  server.begin();

  if(freshBonds)
    forgetPairedDevices();              // new sketch: phone and watch pair again
''')

open(path, 'w').write(s)

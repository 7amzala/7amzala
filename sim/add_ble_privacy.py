# Private, encrypted Bluetooth for the rod.
#
# - Every rod has its own ID (ROD-4F2A, from its chip) and its own 6-digit
#   PIN (made once at random, kept in flash, printed on the Serial Monitor:
#   write both on the box).
# - The phone and the watch pair once with the PIN. The link is encrypted
#   (Bluetooth LE Secure Connections); without pairing nothing can be read
#   or sent.
# - New pairing only while the rod has no paired device yet, or in the
#   first 3 minutes after switching it on. Then it is locked.
# - The rod shows its ID only while pairing is open; after that its signal
#   has no name and no StrikeSense service, and a device that connects but
#   never pairs is sent away after 30 s.
# - /forget (from a paired phone): forgets all paired devices.
#
#   python3 sim/add_ble_privacy.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

PRIVACY = r'''// ---------- Private Bluetooth: rod ID + PIN, pairing window ----------

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

'''

PRIVACY_FUNCS = r'''// ---------- Private Bluetooth: rod ID, PIN, paired devices ----------

int bondedDevices()
{
#if defined(CONFIG_BLUEDROID_ENABLED)
  return esp_ble_get_bond_device_num();
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

void forgetPairedDevices()
{
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
#endif

  Serial.println("Bluetooth: all paired devices forgotten - pairing open");
}

'''


def patch(path):
    s = open(path).read()

    if "void privacyBegin()" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep("""#include <BLEDevice.h>
#include <BLEServer.h>
""", """#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <esp_mac.h>
""")

    rep("""// =====================================================
// BLUETOOTH LINK TO THE PHONE APP
""", PRIVACY + """// =====================================================
// BLUETOOTH LINK TO THE PHONE APP
""")

    # the functions go further down: an Arduino IDE rule (its automatic
    # declarations go before the first function, before the rod's types)
    rep("""// ---------- Watch link: auto / Bluetooth only / LoRa only (phone app) ----------
""", PRIVACY_FUNCS + """// ---------- Watch link: auto / Bluetooth only / LoRa only (phone app) ----------
""")

    # ---- begin(): name = rod ID, PIN, encryption needed to use the rod ----
    rep("""    BLEDevice::init("StrikeSense");
    BLEDevice::setMTU(517);
""", """    privacyBegin();

    BLEDevice::init(rodId);
    BLEDevice::setMTU(517);

#if defined(CONFIG_BLUEDROID_ENABLED)
    BLEDevice::setSecurityCallbacks(new PairCallbacks());
#endif
    BLESecurity::setPassKey(true, rodPin);
    BLESecurity::setCapability(ESP_IO_CAP_OUT);              // the PIN is on the box: the phone / watch types it
    BLESecurity::setAuthenticationMode(true, true, true);    // bond, PIN protected, Secure Connections
""")
    rep("""      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new RxCallbacks(this));
""", """      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR |
      BLECharacteristic::PROPERTY_WRITE_ENC | BLECharacteristic::PROPERTY_WRITE_AUTHEN);   // (NimBLE)
    rx->setCallbacks(new RxCallbacks(this));

#if defined(CONFIG_BLUEDROID_ENABLED)
    rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);  // only a paired device can ask anything
#endif
""")
    rep("""#if !defined(CONFIG_NIMBLE_ENABLED)
    tx->addDescriptor(new BLE2902());    // older ESP32 boards (Bluedroid); NimBLE adds it by itself
#endif
""", """#if !defined(CONFIG_NIMBLE_ENABLED)
    BLE2902* cccd = new BLE2902();       // older ESP32 boards (Bluedroid); NimBLE adds it by itself
#if defined(CONFIG_BLUEDROID_ENABLED)
    cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);   // only a paired device gets the data
#endif
    tx->addDescriptor(cccd);
#endif
""")
    rep("""    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();
  }""", """    advertiseFor(pairWindowOpen());
    BLEDevice::startAdvertising();
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

    Serial.printf("Bluetooth: %s\\n", pairing ? "pairing OPEN - the rod shows its ID" : "pairing locked - the rod is hidden");
  }

  // A device that connects but never pairs (writes nothing) within 30 s is sent away
  void kickStrangers()
  {
#if defined(CONFIG_BLUEDROID_ENABLED)
    for(int i = 0; i < MAX_CONNS; i++)
    {
      if(conns[i].used && !conns[i].talked && millis() - conns[i].since > 30000)
      {
        Serial.printf("Bluetooth: device %d never paired - sent away\\n", conns[i].id);
        conns[i].used = false;
        BLEDevice::getServer()->disconnect(conns[i].id);
      }
    }
#endif
  }""")

    # every 5 s: pairing window closed? hide; strangers out
    rep("""    if(advertiseAgain || millis() - lastAdvertiseCheck >= 5000)
    {""", """    if(advertiseAgain || millis() - lastAdvertiseCheck >= 5000)
    {
      kickStrangers();

      if(pairWindowOpen() != advPairing)
        advertiseFor(pairWindowOpen());
""")

    rep("""  static const int MAX_CLIENTS = 2;         // phone + watch
""", """  static const int MAX_CLIENTS = 2;         // phone + watch
  bool advPairing = true;

  // who is connected, and did it pair (a paired device can write)
  static const int MAX_CONNS = 4;

  struct Conn
  {
    bool used;
    bool talked;
    uint16_t id;
    unsigned long since;
  };

  Conn conns[MAX_CONNS] = {};

  void connOpened(uint16_t id)
  {
    for(int i = 0; i < MAX_CONNS; i++)
    {
      if(!conns[i].used)
      {
        conns[i] = { true, false, id, millis() };
        return;
      }
    }
  }

  void connTalked(uint16_t id)
  {
    for(int i = 0; i < MAX_CONNS; i++)
      if(conns[i].used && conns[i].id == id)
        conns[i].talked = true;
  }

  void connClosed(uint16_t id)
  {
    for(int i = 0; i < MAX_CONNS; i++)
      if(conns[i].used && conns[i].id == id)
        conns[i].used = false;
  }
""")

    rep("""    void onDisconnect(BLEServer*) override
    {
      if(owner->clients > 0)
        owner->clients = owner->clients - 1;

      owner->advertiseAgain = true;
      Serial.printf("Bluetooth: disconnected (%d left)\\n", (int)owner->clients);
    }
""", """    void onDisconnect(BLEServer*) override
    {
      if(owner->clients > 0)
        owner->clients = owner->clients - 1;

      owner->advertiseAgain = true;
      Serial.printf("Bluetooth: disconnected (%d left)\\n", (int)owner->clients);
    }

#if defined(CONFIG_BLUEDROID_ENABLED)
    // (the library calls both onConnect versions)
    void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override
    {
      owner->connOpened(p->connect.conn_id);
    }

    void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override
    {
      owner->connClosed(p->disconnect.conn_id);
    }
#endif
""")

    rep("""    void onWrite(BLECharacteristic* c) override
    {
      auto v = c->getValue();

      for(size_t i = 0; i < v.length(); i++)
        owner->received(v[i]);
    }
""", """    void onWrite(BLECharacteristic* c) override
    {
      auto v = c->getValue();

      for(size_t i = 0; i < v.length(); i++)
        owner->received(v[i]);
    }

#if defined(CONFIG_BLUEDROID_ENABLED)
    // only a paired (encrypted) device can write: this one is ours
    void onWrite(BLECharacteristic* c, esp_ble_gatts_cb_param_t* p) override
    {
      owner->connTalked(p->write.conn_id);
      onWrite(c);
    }
#endif
""")

    # /data: the rod ID and pairing state; /forget
    rep("""  addWatchLink(json);
""", """  addWatchLink(json);
  json += ",\\"rodid\\":\\"";
  json += rodId;
  json += "\\"";
  addField(json, "paired", (long)bondedDevices());
  addField(json, "pairopen", pairWindowOpen());
""")
    rep("""  server.on("/wping", handleWatchPing);
""", """  server.on("/wping", handleWatchPing);
  server.on("/forget", handleForget);
""")
    rep("""// The watch: "I'm here over Bluetooth, hearing you this strong" (every 2 s)""", """// From a paired phone: forget every paired phone and watch (new phone, lost watch).
// Pairing is open again until the next device pairs.
void handleForget()
{
  server.send(200, "text/plain", "OK");
  forgetPairedDevices();
}

// The watch: "I'm here over Bluetooth, hearing you this strong" (every 2 s)""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

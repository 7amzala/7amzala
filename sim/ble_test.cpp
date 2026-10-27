// Bluetooth link test on the host.
//
// Runs the real rod sketch with its real Bluetooth server code (the BLE
// library itself is a stand-in, mock_ble/) and plays the phone: connect,
// ask for pages, change settings, big answers, many requests at once,
// disconnect in the middle. Answers are put back together the same way
// the app does (android/.../RodLink.java).
//
//   g++ -O2 -std=c++17 -Imock_ble -Imock -o ble_test ble_test.cpp && ./ble_test

#define SIM_NO_MAIN
#define ESP32 1
#include "scenarios.cpp"

#include <map>

BLEServer* srv;
BLECharacteristic* rxc;
BLECharacteristic* txc;
// conn ids: the phone and the watch, each its own Bluetooth connection
const int PHONE = 0, WATCH = 1;
size_t seen[2] = {0, 0};            // pieces already read, per connection
std::string inbox[2];
int wrongLine = 0;                  // an answer that reached the other device

// requests 500000-599999 are the watch's, the rest the phone's
int lineOf(int id) { return id >= 500000 && id < 600000 ? WATCH : PHONE; }

struct Answer { int code; std::string body; };
std::map<int, Answer> answers;
size_t biggestPiece = 0;

int checks = 0, failed = 0;

void check(bool ok, const char* what)
{
  checks++;
  if (!ok) failed++;
  printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

void step(double secs)
{
  double end = simMillis / 1000.0 + secs;

  while (simMillis / 1000.0 < end) {
    simMillis++;
    computeSensor(simMillis / 1000.0);
    loop();
  }
}

void readInbox(int c);

// Same as RodLink.received() in the app (one per device)
void readNotifications()
{
  for (int c = 0; c < 2; c++) {
    std::vector<std::string>& got = mockLinks()[c].pieces;
    if (seen[c] > got.size()) seen[c] = 0;           // a new connection
    for (; seen[c] < got.size(); seen[c]++) {
      biggestPiece = std::max(biggestPiece, got[seen[c]].size());
      inbox[c] += got[seen[c]];
    }
    readInbox(c);
  }
}

void readInbox(int c)
{
  std::string& inbox = ::inbox[c];

  while (true) {
    size_t start = inbox.find('#');
    if (start == std::string::npos) { inbox.clear(); return; }
    size_t nl = inbox.find('\n', start);
    if (nl == std::string::npos) { inbox.erase(0, start); return; }

    int id = 0, code = 0, len = 0;
    if (sscanf(inbox.c_str() + start + 1, "%d %d %d", &id, &code, &len) != 3) { inbox.erase(0, nl + 1); continue; }
    if (inbox.size() - (nl + 1) < (size_t)len) { inbox.erase(0, start); return; }

    if (lineOf(id) != c) wrongLine++;            // (like the app / watch: not ours, ignored)
    else answers[id] = { code, inbox.substr(nl + 1, len) };
    inbox.erase(0, nl + 1 + len);
  }
}

// The phone writes a request, in 20-byte pieces like a small Bluetooth packet
void write(int id, const std::string& path)
{
  std::string line = std::to_string(id) + "|" + path + "\n";

  for (size_t i = 0; i < line.size(); i += 20) {
    rxc->value = line.substr(i, 20);
    mockWrite(rxc, rxc->cb, lineOf(id));
  }
}

bool ask(int id, const std::string& path, Answer& a, double timeout = 5)
{
  write(id, path);
  unsigned long t0 = simMillis;

  while (simMillis - t0 < timeout * 1000) {
    step(0.005);
    readNotifications();
    if (answers.count(id)) { a = answers[id]; return true; }
  }

  return false;
}

int main()
{
  Serial.quiet = true;
  setup();

  srv = BLEDevice::server();
  rxc = srv->service->chars[0];
  txc = srv->service->chars[1];

  printf("\n=== BLUETOOTH LINK ===\n");
  check(BLEDevice::adv().started == 1, "rod advertises after start-up");

  srv->mockConnect(PHONE, 183, false);              // phone: MTU 183 = pieces up to 180
  step(1);
  check(BLEDevice::adv().started == 2, "phone connected: rod keeps advertising so the watch can join");

  Answer a;

  check(ask(1, "/mtu?v=180", a) && a.code == 200 && a.body == "OK", "MTU message answered OK");

  bool ok = ask(2, "/data", a);
  check(ok && a.code == 200 && a.body.size() > 200 && a.body.front() == '{' && a.body.back() == '}', "/data: complete JSON");
  printf("        /data = %zu bytes, biggest piece %zu bytes\n", a.body.size(), biggestPiece);
  check(biggestPiece <= 180, "pieces never bigger than the MTU");
  check(a.body.find("\"state\"") != std::string::npos && a.body.find("\"tilt\"") != std::string::npos, "/data has state and tilt");

  // time for one /data answer (the dashboard asks 10 times a second)
  unsigned long t0 = simMillis;
  ask(3, "/data", a);
  printf("        one /data answer took %lu ms\n", simMillis - t0);
  check(simMillis - t0 < 60, "/data answer fast enough for 10 per second");

  check(ask(4, "/set?k=kmul&v=1.5", a) && a.code == 200, "setting changed (/set)");
  check(fabs(settings.knockMul - 1.5) < 0.001, "rod really uses knock limit x1.5");

  check(ask(5, "/set?k=pdeg&v=1%2E8", a) && fabs(settings.pullDeg - 1.8) < 0.001, "URL-encoded value (1%2E8 = 1.8) understood");

  check(ask(6, "/settings", a) && a.body.find("\"kmul\"") != std::string::npos, "/settings returns the settings");

  check(ask(7, "/nothing", a) && a.code == 404, "unknown request answered 404");

  // a very big answer: the whole page (~90 KB), checked byte for byte
  t0 = simMillis;
  ok = ask(8, "/", a, 30);
  check(ok && a.body == std::string(webpage), "big answer (whole page) arrives complete and exact");
  printf("        page = %zu bytes in %.1f s\n", a.body.size(), (simMillis - t0) / 1000.0);

  // several requests at once, like the dashboard + a button
  write(10, "/data");
  write(11, "/watching");
  write(12, "/data");
  step(0.5);
  readNotifications();
  check(answers.count(10) && answers.count(11) && answers.count(12), "3 requests at once: all answered");
  check(answers[11].body == "OK", "/watching answered OK");

  // phone walks away in the middle of an answer
  write(20, "/");
  step(0.2);
  int advBeforeLeave = BLEDevice::adv().started;
  srv->mockDisconnect(PHONE);
  step(0.2);
  check(BLEDevice::adv().started == advBeforeLeave + 1, "after a disconnect the rod advertises again");

  size_t before = mockLinks()[PHONE].pieces.size() + mockLinks()[WATCH].pieces.size();
  step(1);
  check(mockLinks()[PHONE].pieces.size() + mockLinks()[WATCH].pieces.size() == before, "nothing is sent while nobody is connected");

  // phone comes back (a new connection: the app starts with an empty buffer)
  inbox[PHONE].clear();
  srv->mockConnect(PHONE, 247, false);
  seen[PHONE] = 0;
  step(0.2);
  check(ask(21, "/mtu?v=244", a) && ask(22, "/data", a) && a.body.front() == '{' && a.body.back() == '}', "reconnect: /data works again");

  // REEL IN and READY over Bluetooth
  check(ask(23, "/reelin", a) && rodState == ST_PAUSED, "/reelin: rod reeled in (paused)");
  check(ask(24, "/ready", a) && rodState != ST_PAUSED, "/ready: rod back to watching");

  // ---------- the watch joins as a second device ----------
  printf("\n=== PHONE + WATCH ===\n");
  int advBefore = BLEDevice::adv().started;
  srv->mockConnect(WATCH, 247, true);           // watch connects, on a calm (slow) link
  step(0.1);
  check(BLEDevice::adv().started == advBefore, "phone + watch connected: full, the rod stops advertising");

  check(ask(500001, "/mtu?v=200", a) && a.code == 200, "watch says its MTU");
  check(ask(25, "/data", a) && a.body.front() == '{' && a.body.back() == '}', "phone /data still complete with both connected");
  printf("        biggest piece now %zu bytes (each device its own MTU)\n", biggestPiece);

  ok = ask(500002, "/watch", a);
  check(ok && a.code == 200 && a.body.front() == '{' && a.body.back() == '}', "/watch: complete JSON");
  printf("        /watch = %zu bytes: %s\n", a.body.size(), a.body.c_str());
  check(a.body.size() < 400, "/watch is small (watch-friendly)");
  check(a.body.find("\"state\"") != std::string::npos && a.body.find("\"tilt\"") != std::string::npos &&
        a.body.find("\"bat\"") != std::string::npos && a.body.find("\"ptneed\"") != std::string::npos, "/watch has state, tilt, battery, pull test");

  // A knock: both the phone's /data and the watch's /watch must see it
  energyPeakSincePoll = 0; energyPeakSinceWatch = 0;
  knockEnergy = 0.5;
  for (int i = 0; i < 3; i++) { simMillis++; processSample(); }
  ask(26, "/data", a);
  bool phoneSaw = a.body.find("\"energy\":0.0000") == std::string::npos;
  ask(500003, "/watch", a);
  bool watchSaw = a.body.find("\"energy\":0.0000") == std::string::npos;
  check(phoneSaw && watchSaw, "a knock reaches both phone and watch (separate peaks)");

  // the live message: 10 a second, for everyone, between answers
  {
    std::string all;
    size_t from = mockLinks()[WATCH].pieces.size();
    step(2);
    for (size_t i = from; i < mockLinks()[WATCH].pieces.size(); i++) all += mockLinks()[WATCH].pieces[i];
    int lives = 0;
    std::string body;
    for (size_t p = all.find("#live 200 "); p != std::string::npos; p = all.find("#live 200 ", p + 1)) {
      lives++;
      size_t nl = all.find('\n', p);
      long len = atol(all.c_str() + p + 10);
      if (nl != std::string::npos && all.size() >= nl + 1 + len) body = all.substr(nl + 1, len);
    }
    printf("        %d live messages in 2 s, last: %zu bytes\n", lives, body.size());
    check(lives >= 18 && lives <= 21, "rod sends the live message 10 times a second by itself");
    check(!body.empty() && body.front() == '{' && body.back() == '}' && body.find("\"strike\"") != std::string::npos &&
          body.find("\"strikeid\"") != std::string::npos && body.find("\"bat\"") != std::string::npos,
          "live message: complete, has strike, strike id, battery (phone alarm + watch)");
  }
  check(ask(47, "/data", a) && a.body.front() == '{' && a.body.back() == '}', "answers still complete between live messages");

  // NEW PULL DIRECTION (phone settings / watch): a pull test on request
  check(ask(48, "/teachdir", a) && a.body == "OK" && rodState == ST_PULLTEST, "/teachdir: pull test started on request");
  check(ask(49, "/skippull", a) && rodState == ST_MONITORING, "skip it: back to watching");

  // the phone leaves: the watch keeps working
  srv->mockDisconnect(PHONE);
  step(0.2);
  check(ask(500004, "/watch", a) && a.code == 200, "phone gone: watch still answered");
  check(ask(500005, "/watching", a) && a.body == "OK", "watch button (I'M WATCHING) works");

  // watch pages: last strike (fresh after a rod start) and battery time left
  printf("\n=== WATCH PAGES ===\n");
  check(ask(500006, "/watch", a) && a.body.find("\"batleft\"") != std::string::npos, "/watch has battery time left");
  check(a.body.find("\"ft\":") != std::string::npos, "/watch has the quicker tilt for the watch ring");
  check(ask(500007, "/laststrike", a) && a.body.find("\"have\":false") != std::string::npos &&
        a.body.find("\"count\":0") != std::string::npos, "/laststrike: no strike yet after start");

  step(25);                                       // some history before the strike
  strikeActive = false;
  rodState = ST_MONITORING;
  startStrike("SLOW PULL");
  step(2.5);
  ok = ask(500008, "/laststrike", a);
  {
    size_t kp = a.body.find("\"k\":["), zp = a.body.find("\"zk\":[");
    int kn0 = 0, zn0 = 0;
    for (size_t i = kp; kp != std::string::npos && i < a.body.find(']', kp); i++) if (a.body[i] == ',') kn0++;
    for (size_t i = zp; zp != std::string::npos && i < a.body.find(']', zp); i++) if (a.body[i] == ',') zn0++;
    printf("        2.5 s after the strike: %d points, zoom %d points\n", kn0 + 1, zn0 + 1);
    check(ok && a.body.find("\"recording\":true") != std::string::npos && a.body.find("\"have\":true") != std::string::npos &&
          kn0 + 1 >= 100 && a.body.find("SLOW PULL") != std::string::npos,
          "strike shown at once while recording (graph before + after so far)");
    check(zn0 + 1 >= 200, "zoom: 2 s before + 2 s after already there");
  }
  step(10);
  ok = ask(500009, "/laststrike", a);
  size_t kpos = a.body.find("\"k\":["), tpos = a.body.find("\"t\":[");
  int kn = 0, tn = 0;
  for (size_t i = kpos; i < a.body.find(']', kpos); i++) if (a.body[i] == ',') kn++;
  for (size_t i = tpos; i < a.body.find(']', tpos); i++) if (a.body[i] == ',') tn++;
  printf("        /laststrike = %zu bytes, %d points\n", a.body.size(), kn + 1);
  check(ok && a.body.find("\"have\":true") != std::string::npos && a.body.find("SLOW PULL") != std::string::npos &&
        a.body.find("\"count\":1") != std::string::npos, "/laststrike: type, count after a strike");
  check(kn == tn && kn + 1 == 150 && a.body.find("\"pre\":100") != std::string::npos, "/laststrike: 150 points, 100 before (20 s) + 50 after (10 s)");
  {
    size_t zp = a.body.find("\"zk\":["); int zn0 = 0;
    for (size_t i = zp; zp != std::string::npos && i < a.body.find(']', zp); i++) if (a.body[i] == ',') zn0++;
    check(zn0 + 1 == 200 && a.body.find("\"zpre\":100") != std::string::npos, "zoom: 200 points at 50 a second, strike in the middle");
  }
  printf("        /laststrike = %zu bytes, zoom part: %.120s\n", a.body.size(), a.body.c_str() + a.body.find("\"zpre\""));
  check(a.body.size() < 4500, "/laststrike small enough for the watch");
  {
    // the phone app's LAST STRIKE view: /strike, the app waits 6 s -
    // with the watch also connected, on its calm (slow) link
    srv->mockConnect(PHONE, 247, false);      // the phone comes back
    seen[PHONE] = 0; inbox[PHONE].clear();
    Answer w; ask(700019, "/mtu?v=244", w);
    int dropsBefore = mockLinks()[PHONE].drops + mockLinks()[WATCH].drops;
    Answer b; unsigned long t0 = simMillis;
    bool got = ask(700020, "/strike", b, 6);
    int dropped = mockLinks()[PHONE].drops + mockLinks()[WATCH].drops - dropsBefore;
    printf("        /strike = %zu bytes in %.2f s (code %d)\n", b.body.size(), (simMillis - t0) / 1000.0, b.code);
    printf("        pieces dropped on the way: %d\n", dropped);
    check(got && b.code == 200 && b.body.find("\"e\":[") != std::string::npos && b.body.back() == '}' && dropped == 0,
          "/strike (phone LAST STRIKE) arrives whole within 6 s, also with the watch on its slow link");
  }

  // ---- for the phone app: how the watch is connected, the last strikes ----
  printf("\n=== WATCH LINK + STRIKE LIST (phone app) ===\n");
  {
    const size_t NO = std::string::npos;
    Answer b;
    step(7);
    ask(700001, "/data", b);
    check(b.body.find("\"wlink\":\"none\"") != NO, "watch not pinging: phone shows 'watch not connected'");
    check(ask(500030, "/wping?r=-66", b) && b.body == "OK", "watch ping (every 2 s) answered");
    ask(700002, "/data", b);
    check(b.body.find("\"wlink\":\"bt\"") != NO && b.body.find("\"wbars\":3") != NO, "watch on Bluetooth at -66 dBm: phone shows 'Watch Bluetooth', 3 bars");
    step(7);
    ask(700003, "/data", b);
    check(b.body.find("\"wlink\":\"none\"") != NO, "watch gone (no ping for 6 s): 'not connected'");

    ask(700004, "/strikes", b);
    printf("        /strikes = %s\n", b.body.c_str());
    check(b.body.find("\"type\":\"SLOW PULL\"") != NO && b.body.find("\"done\":true") != NO, "/strikes: the earlier strike, fight finished");

    // a fish: pull 3 degrees for 6 s
    strikeActive = false;
    rodState = ST_MONITORING;
    events.push_back({ (float)(simMillis / 1000.0), 1, 3.0, 0.5, 6 });
    startStrike("SHARP STRIKE");
    step(3);
    ask(700005, "/strikes", b);
    check(b.body.rfind("\"done\":false") != NO && b.body.find("SHARP STRIKE") != NO, "fight still going: done false");
    cmdWatching();
    step(25);
    events.clear();
    ask(700006, "/strikes", b);
    size_t at = b.body.find("SHARP STRIKE");
    long fight = at != NO ? atol(b.body.c_str() + b.body.find("\"fight\":", at) + 8) : -1;
    float peak = at != NO ? atof(b.body.c_str() + b.body.find("\"peak\":", at) + 7) : 0;
    printf("        fight %ld s, strongest pull %.2f deg\n", fight, peak);
    check(b.body.find("\"done\":true", at) != NO && fight >= 4 && fight <= 12, "fight time = while the fish pulled (about 6 s)");
    check(peak >= 2.0, "strongest pull recorded");

    for (int i = 0; i < 12; i++) { strikeActive = false; startStrike("SLOW PULL"); step(0.2); }
    ask(700007, "/strikes", b);
    int n = 0; for (size_t p = b.body.find("\"id\":"); p != NO; p = b.body.find("\"id\":", p + 1)) n++;
    check(n == 10 && b.body.size() < 1500, "/strikes keeps the last 10 (small answer)");
  }

  // ---- private Bluetooth: rod ID + PIN ----
  {
    const size_t NO = std::string::npos;
    Answer b;
    printf("        %s  PIN %06u\n", rodId.c_str(), (unsigned)rodPin);
    check(rodId == "ROD-4F2A", "rod ID from the chip: ROD-4F2A");
    uint32_t pin = rodPin;
    check(pin >= 100000 && pin <= 999999 && BLESecurity::pin() == pin, "6-digit PIN given to the Bluetooth pairing");
    privacyBegin();
    check(rodPin == pin, "same PIN after a restart (kept in flash)");
    check(BLEDevice::name() == "ROD-4F2A", "Bluetooth name = rod ID (no 'StrikeSense')");
    BLEAdvertising& ad = BLEDevice::adv();
    check(ad.scan.name == "ROD-4F2A" && !ad.adv.service.empty(), "pairing open: the signal shows the rod ID");
    server.advertiseFor(false);
    check(ad.scan.name.empty() && ad.adv.service.empty() && ad.adv.name.empty(), "locked: no name, no service in the signal");
    server.advertiseFor(true);
    ask(700201, "/data", b);
    check(b.body.find("\"rodid\":\"ROD-4F2A\"") != NO && b.body.find("\"pairopen\":true") != NO, "/data tells the app the rod ID and pairing state");
  }

  // ---- watch link choice (phone app) ----
  {
    const size_t NO = std::string::npos;
    Answer b;
    ask(700101, "/data", b);
    check(b.body.find("\"lmode\":\"auto\"") != NO && b.body.find("\"haslora\":false") != NO, "/data: link Auto, this rod build has no LoRa");
    check(ask(700102, "/linkmode?m=lora", b) && b.body == "{\"lmode\":\"lora\"}", "/linkmode: LoRa only");
    check(ask(500040, "/watch", b) && b.body.find("\"lmode\":\"lora\"") != NO, "the watch's answer has the choice too");
    linkMode = 0;
    loadLinkMode();
    check(linkMode == 2, "the choice is kept after a rod restart");
    check(ask(700103, "/linkmode?m=bad", b) && b.code == 400, "a wrong choice is refused");
    ask(700104, "/linkmode?m=auto", b);
  }

  // ---- 5 named setups (presets), kept on the rod ----
  {
    Answer b;
    check(ask(600001, "/presets", b) && b.body == "{\"presets\":[\"\",\"\",\"\",\"\",\"\"]}", "/presets: 5 empty setups");
    ask(600002, "/set?k=pdeg&v=2.5", b);
    check(ask(600003, "/presetsave?s=0&n=Calm%20beach%20%C3%B1", b) && b.body.find("\"Calm beach \xC3\xB1\"") != std::string::npos,
          "save setup 1 with a name (Spanish letters kept)");
    ask(600004, "/set?k=pdeg&v=1.0", b);
    check(ask(600005, "/presetload?s=0", b) && b.body.find("\"pdeg\":2.500") != std::string::npos && b.body.find("\"saved\":false") != std::string::npos,
          "load setup 1: its values come back (SAVE keeps them)");
    check(ask(600006, "/presetsave?s=1&n=%22bad%5C%22%0A", b) && b.body.find("\"bad\"") != std::string::npos,
          "quotes, backslash, new line removed from a name");
    ask(600007, "/presetsave?s=2&n=" + std::string(40, 'x'), b);
    check(b.body.find("\"" + std::string(24, 'x') + "\"") != std::string::npos, "a long name is cut to 24 letters");
    check(ask(600008, "/presetsave?s=3&n=", b) && b.body.find("\"Setup 4\"") != std::string::npos, "no name: 'Setup 4'");
    check(ask(600009, "/presetsave?s=7&n=x", b) && b.code == 400, "only setups 1-5");
    check(ask(600010, "/presetdel?s=0", b) && b.body.find("Calm") == std::string::npos, "delete setup 1");
    check(ask(600011, "/presetload?s=0", b) && b.code == 404, "an empty setup cannot be loaded");
    check(ask(600012, "/defaultsettings", b) && b.body.find("\"pdeg\":1.000") != std::string::npos, "Default: back to the factory values");
  }

  // ---- time and date ----
  {
    Answer b;
    check(ask(600101, "/watch", b) && b.body.find("\"time\":0") != std::string::npos, "no time yet: \"time\":0");
    check(ask(600102, "/settime?t=1760000000&tz=60", b) && b.code == 200, "/settime from the phone");
    step(5);
    check(ask(600103, "/watch", b) && b.body.find("\"time\":176000000") != std::string::npos && b.body.find("\"tz\":60") != std::string::npos,
          "the watch gets the time (counting on) and the time zone");
    check(ask(600104, "/settime?t=12&tz=0", b) && b.code == 400, "a wrong time is refused");
  }

  // ---- watchdog ----
  {
    Answer b;
    printf("        watchdog: on=%d, %ld check-ins, longest gap %lu ms (limit %u)\n", wdtMock().on, wdtMock().resets, wdtMock().longest, (unsigned)wdtMock().timeout);
    check(wdtMock().on && wdtMock().timeout == 10000 && wdtMock().longest < 2000, "watchdog on (10 s), the rod checks in far more often");
    check(ask(600201, "/data", b) && b.body.find("\"rst\":\"power\"") != std::string::npos, "/data says why the rod started (rst)");
  }


  // ---- the last 5 strikes kept with their graphs (phone app: by number) ----
  printf("\n=== LAST 5 STRIKE GRAPHS ===\n");
  {
    const size_t NO = std::string::npos;
    Answer b;
    strikeActive = false; rodState = ST_MONITORING;
    step(5);
    startStrike("SLOW PULL");
    long first = (long)strikeEventID;
    step(4);                                   // a second strike 4 s later
    strikeActive = false;
    startStrike("SHARP STRIKE");
    long second = (long)strikeEventID;
    step(12);
    bool g1 = ask(700300, "/laststrike?n=" + std::to_string(first), b) && b.body.find("\"have\":true") != NO &&
              b.body.find("\"id\":" + std::to_string(first) + ",") != NO && b.body.find("SLOW PULL") != NO &&
              b.body.find("\"zk\":[") != NO && b.body.find("\"zk\":[]") == NO;
    printf("        strike %ld (4 s before the next): %zu bytes\n", first, b.body.size());
    bool g2 = ask(700301, "/laststrike?n=" + std::to_string(second), b) && b.body.find("\"have\":true") != NO &&
              b.body.find("SHARP STRIKE") != NO && b.body.find("\"recording\":false") != NO;
    check(g1, "strike 4 s before the next one: its graph + zoom still there");
    check(g2, "the next one too");
    check(ask(700302, "/laststrike", b) && b.body.find("SHARP STRIKE") != NO, "/laststrike without a number: the newest (watch)");

    for (int i = 0; i < 6; i++) { strikeActive = false; startStrike("SLOW PULL"); step(11); }
    bool gone = ask(700303, "/laststrike?n=" + std::to_string(first), b) && b.body.find("\"have\":false") != NO;
    bool kept = ask(700304, "/laststrike?n=" + std::to_string((long)strikeEventID - 4), b) && b.body.find("\"have\":true") != NO;
    check(gone && kept, "the rod keeps the newest 5 (older ones: have false)");
    cmdWatching();
    step(2);
  }

  // ---- smooth with phone + watch: a strike while the dashboard graph runs ----
  printf("\n=== PHONE + WATCH DURING A STRIKE (smoothness) ===\n");
  {
    Answer b;
    readNotifications();
    int dropsBefore = mockLinks()[PHONE].drops + mockLinks()[WATCH].drops;
    size_t watchFrom = mockLinks()[WATCH].pieces.size();
    size_t phoneFrom = mockLinks()[PHONE].pieces.size();

    // the dashboard: /data, next one 100 ms after the answer (like pollLoop);
    // the watch: a ping every 2 s; a strike after 1 s; the watch taps after 3 s
    strikeActive = false;
    rodState = ST_MONITORING;
    int phoneAnswers = 0, nextId = 800000;
    unsigned long worstData = 0, tapAt = 0, tapTook = 0;
    unsigned long start = simMillis, askedAt = 0, nextAsk = simMillis;
    int asking = -1;
    bool struck = false, tapped = false;

    while (simMillis - start < 6000) {
      if (asking < 0 && simMillis >= nextAsk) {
        asking = nextId++;
        askedAt = simMillis;
        write(asking, "/data");
      }
      if (!struck && simMillis - start >= 1000) { struck = true; startStrike("SHARP STRIKE"); }
      if (!tapped && simMillis - start >= 3000) { tapped = true; tapAt = simMillis; write(500900, "/watching"); }
      if ((simMillis - start) % 2000 == 0) write(500901 + (int)((simMillis - start) / 2000), "/wping?r=-60");

      step(0.001);
      readNotifications();

      if (asking >= 0 && answers.count(asking)) {
        worstData = std::max(worstData, simMillis - askedAt);
        phoneAnswers++;
        asking = -1;
        nextAsk = simMillis + 100;
      }
      if (tapped && tapTook == 0 && answers.count(500900)) tapTook = simMillis - tapAt;
    }

    // the watch's live messages: how often, and the longest gap
    std::string all;
    std::vector<size_t> at;
    for (size_t i = watchFrom; i < mockLinks()[WATCH].pieces.size(); i++) all += mockLinks()[WATCH].pieces[i];
    int lives = 0;
    for (size_t q = all.find("#live 200 "); q != std::string::npos; q = all.find("#live 200 ", q + 1)) lives++;
    size_t phoneBytes = 0;
    for (size_t i = phoneFrom; i < mockLinks()[PHONE].pieces.size(); i++) phoneBytes += mockLinks()[PHONE].pieces[i].size();
    int dropped = mockLinks()[PHONE].drops + mockLinks()[WATCH].drops - dropsBefore;

    printf("        phone: %d /data answers in 6 s (slowest %lu ms), %zu bytes\n", phoneAnswers, worstData, phoneBytes);
    printf("        watch: %d live messages in 6 s, tap answered in %lu ms, pieces lost %d\n", lives, tapTook, dropped);
    check(phoneAnswers >= 40, "phone live graph: 7+ updates a second while the watch is connected");
    check(worstData < 150, "phone: every /data answer within 150 ms");
    check(lives >= 54, "watch: the live message 9+ times a second, also during the strike");
    check(tapTook > 0 && tapTook < 250, "watch tap (I'M WATCHING) answered within 250 ms");
    check(dropped == 0, "no piece lost on either link");
    check(wrongLine == 0, "every answer only reached the device that asked");
    cmdWatching();
    step(12);
  }

  // ---- the watch leaves: the phone gets the whole Bluetooth to itself ----
  {
    Answer b;
    unsigned long t0 = simMillis;
    bool ok2 = ask(800900, "/", b, 30);
    unsigned long both = simMillis - t0;
    srv->mockDisconnect(WATCH);
    step(0.2);
    t0 = simMillis;
    ok2 = ask(800901, "/", b, 30) && ok2;
    unsigned long alone = simMillis - t0;
    printf("        whole page to the phone: %.2f s with the watch, %.2f s alone\n", both / 1000.0, alone / 1000.0);
    check(ok2 && both < alone * 1.3 + 100, "phone full speed with the watch connected too (own line)");
    srv->mockConnect(WATCH, 247, true);
    seen[WATCH] = 0; inbox[WATCH].clear();
  }


  // ---- strike log in flash: strikes kept with no phone near, and after power off ----
  printf("\n=== STRIKE LOG (flash) ===\n");
  {
    const size_t NO = std::string::npos;
    Answer b;
    srv->mockDisconnect(PHONE);               // the phone is far away
    step(1);
    uint32_t seq0 = logSeq;
    for (int i = 0; i < 3; i++) {
      strikeActive = false; rodState = ST_MONITORING;
      startStrike(i == 1 ? "SHARP STRIKE" : "SLOW PULL");
      step(12);
      cmdWatching();
      step(40);                               // the fight ends, the rod saves it
    }
    printf("        log: %d slots, newest %u, %d files written\n", logSlots, (unsigned)logSeq, mockFs().writes);
    check(logReady && logSeq == seq0 + 3, "3 strikes with the phone away: numbered 1, 2, 3 more");

    // the phone comes back and collects them
    srv->mockConnect(PHONE, 247, false);
    seen[PHONE] = 0; inbox[PHONE].clear();
    check(ask(700400, "/log?after=" + std::to_string(seq0), b) && b.code == 200, "/log answered");
    printf("        /log = %.300s\n", b.body.c_str());
    int n = 0; for (size_t p = b.body.find("\"seq\":"); p != NO; p = b.body.find("\"seq\":", p + 1)) n++;
    check(n == 3 && b.body.find("SHARP STRIKE") != NO && b.body.find("\"graph\":true") != NO, "/log: the 3 missed strikes, with type, fight and graph");
    check(b.body.find("\"utc\":0,") == NO, "each has its real time (the rod knew the time)");
    std::string q = std::to_string(seq0 + 2);
    check(ask(700401, "/logstrike?seq=" + q, b) && b.body.find("\"have\":true") != NO && b.body.find("SHARP STRIKE") != NO &&
          b.body.find("\"k\":[") != NO && b.body.find("\"zk\":[]") == NO, "/logstrike: that strike's graph + zoom");

    // the rod is switched off and on again: the strikes are still there
    logReady = false; logSeq = 0; slistCount = 0;
    logBegin();
    check(logSeq == seq0 + 3, "after power off: the rod still knows its strike numbers");
    check(ask(700402, "/log?after=" + std::to_string(seq0), b) && b.body.find("SHARP STRIKE") != NO && b.body.find("\"ago\":-1") != NO,
          "after power off: the strikes are still there (time from before)");
    check(ask(700403, "/logstrike?seq=" + q, b) && b.body.find("\"have\":true") != NO, "after power off: the graph too");

    // many strikes in a row (rough sea): none lost, the oldest replaced when full
    int writesBefore = mockFs().writes;
    for (int i = 0; i < 14; i++) { strikeActive = false; rodState = ST_MONITORING; startStrike("SLOW PULL"); step(0.5); }
    cmdWatching();
    step(60);
    check(ask(700404, "/log?after=" + std::to_string(seq0 + 3), b), "/log after a burst");
    int burst = 0; for (size_t p = b.body.find("\"seq\":"); p != NO; p = b.body.find("\"seq\":", p + 1)) burst++;
    printf("        burst of 14: %d files written, first 8 listed: %d\n", mockFs().writes - writesBefore, burst);
    check(mockFs().writes - writesBefore == 14 && burst == 8, "14 strikes in 7 s: all 14 saved, each written once (8 per answer)");
    check(ask(700405, "/log?after=" + std::to_string(seq0 + 11), b) && b.body.find("\"seq\":" + std::to_string(seq0 + 17)) != NO,
          "the rest come with the next question");

    check(ask(700406, "/log?after=0", b) && b.body.find("\"seq\":1,") != NO, "asking from 0: starts at the oldest kept");

    // a small storage (10 strikes): the oldest are replaced, the newest 10 all there
    mockFs().total = 60000;
    logReady = false; slistCount = 0;
    logBegin();
    check(logSlots == 10, "small storage: room for 10");
    for (int i = 0; i < 13; i++) { strikeActive = false; rodState = ST_MONITORING; startStrike("SLOW PULL"); step(0.5); }
    cmdWatching();
    step(60);
    uint32_t newest = logSeq;
    ask(700407, "/log?after=0", b);
    std::string first = "\"seq\":" + std::to_string(newest - 9) + ",";
    check(b.body.find(first) != NO && b.body.find("\"seq\":" + std::to_string(newest - 10) + ",") == NO,
          "full: starts at the oldest kept (the 10 newest), older ones replaced");
    ask(700408, "/log?after=" + std::to_string(newest - 2), b);
    check(b.body.find("\"seq\":" + std::to_string(newest) + ",") != NO, "the newest is there");
    mockFs().total = 1536 * 1024;
    logReady = false; slistCount = 0;
    logBegin();
  }

  // ---- devices that vanish without the rod being told (the app stuck on LEARNING) ----
  printf("\n=== LINES LEFT BEHIND ===\n");
  {
    Answer b;
    srv->mockDisconnect(PHONE);
    srv->mockDisconnect(WATCH);
    step(0.5);
    // 4 connections the library forgot to report as closed: all lines taken
    for (uint16_t id = 4; id < 8; id++) {
      mockLinks()[id].up = true;
      srv->mockConnect(id, 247, false);
      mockLinks()[id].up = false;            // gone, no word to the rod
    }
    step(0.2);
    srv->mockConnect(PHONE, 247, false);     // the phone comes back
    seen[PHONE] = 0; inbox[PHONE].clear();
    unsigned long t0 = simMillis;
    bool got = false;
    for (int k = 0; k < 40 && !got; k++)     // the app asks again and again
      got = ask(810000 + k, "/data", b, 0.2) && b.body.back() == '}';
    printf("        phone answered after %.1f s\n", (simMillis - t0) / 1000.0);
    check(got && simMillis - t0 < 3500, "lines left by vanished devices: freed, the phone gets its data within 3.5 s (no rod restart)");
    srv->mockConnect(WATCH, 247, true);
    seen[WATCH] = 0; inbox[WATCH].clear();
    check(ask(510000, "/watch", b, 3) && b.body.back() == '}', "... and the watch too");
  }

  // ---- safety nets (if the real Bluetooth stack behaves differently) ----
  printf("\n=== SAFETY NETS ===\n");
  {
    Answer b;
    mockAlwaysFull() = true;                   // the stack always says "full"
    unsigned long t0 = simMillis;
    bool ok1 = ask(800950, "/data", b, 5) && b.body.back() == '}';
    bool ok2 = ask(500950, "/watch", b, 5) && b.body.back() == '}';
    printf("        stack always 'full': answers after %lu ms\n", simMillis - t0);
    check(ok1 && ok2 && simMillis - t0 < 1500, "stack stuck on 'full': the rod stops believing it, answers still come");
    t0 = simMillis;
    ok1 = ask(800951, "/data", b, 5);
    printf("        then: %lu ms\n", simMillis - t0);
    check(ok1 && simMillis - t0 < 100, "...and then at full speed again");
    mockAlwaysFull() = false;

    mockSendRefused() = true;                  // sending to one device never works
    step(0.1);
    mockSendRefused() = false;
    size_t p0 = mockLinks()[PHONE].pieces.size(), w0 = mockLinks()[WATCH].pieces.size();
    step(0.5);
    printf("        after refusal: phone +%zu pieces, watch +%zu, drops %d/%d\n", mockLinks()[PHONE].pieces.size() - p0, mockLinks()[WATCH].pieces.size() - w0, mockLinks()[PHONE].drops, mockLinks()[WATCH].drops);
    ok1 = ask(800952, "/data", b, 5) && b.body.back() == '}';
    ok2 = ask(500952, "/watch", b, 5);
    bool whole = ok2 && b.body.back() == '}';
    printf("        direct send refused: phone %d, watch %d (whole %d) %.60s ... %.60s\n", ok1, ok2, whole, b.body.c_str(), b.body.size() > 60 ? b.body.c_str() + b.body.size() - 60 : "");
    ok2 = whole;
    check(ok1 && ok2, "direct send refused: the rod sends to all (the old way), phone and watch still answered");
    step(65);                                  // a minute later: own lines again
    readNotifications();
    wrongLine = 0;
    ok1 = ask(800960, "/data", b, 5) && b.body.back() == '}';
    ok2 = ask(500960, "/watch", b, 5) && b.body.back() == '}';
    readNotifications();
    check(ok1 && ok2 && wrongLine == 0, "a minute later: own line for each device again (answers reach only the asker)");
  }

  printf("\n%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

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
size_t seen = 0;                    // notifications already read
std::string inbox;

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

// Same as RodLink.received() in the app
void readNotifications()
{
  for (; seen < txc->notified.size(); seen++) {
    biggestPiece = std::max(biggestPiece, txc->notified[seen].size());
    inbox += txc->notified[seen];
  }

  while (true) {
    size_t start = inbox.find('#');
    if (start == std::string::npos) { inbox.clear(); return; }
    size_t nl = inbox.find('\n', start);
    if (nl == std::string::npos) { inbox.erase(0, start); return; }

    int id = 0, code = 0, len = 0;
    if (sscanf(inbox.c_str() + start + 1, "%d %d %d", &id, &code, &len) != 3) { inbox.erase(0, nl + 1); continue; }
    if (inbox.size() - (nl + 1) < (size_t)len) { inbox.erase(0, start); return; }

    answers[id] = { code, inbox.substr(nl + 1, len) };
    inbox.erase(0, nl + 1 + len);
  }
}

// The phone writes a request, in 20-byte pieces like a small Bluetooth packet
void write(int id, const std::string& path)
{
  std::string line = std::to_string(id) + "|" + path + "\n";

  for (size_t i = 0; i < line.size(); i += 20) {
    rxc->value = line.substr(i, 20);
    rxc->cb->onWrite(rxc);
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

  srv->cb->onConnect(srv);
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
  srv->cb->onDisconnect(srv);
  step(0.2);
  check(BLEDevice::adv().started == advBeforeLeave + 1, "after a disconnect the rod advertises again");

  size_t before = txc->notified.size();
  step(1);
  check(txc->notified.size() == before, "nothing is sent while nobody is connected");

  // phone comes back (a new connection: the app starts with an empty buffer)
  inbox.clear();
  seen = txc->notified.size();
  srv->cb->onConnect(srv);
  step(0.2);
  check(ask(21, "/mtu?v=244", a) && ask(22, "/data", a) && a.body.front() == '{' && a.body.back() == '}', "reconnect: /data works again");

  // REEL IN and READY over Bluetooth
  check(ask(23, "/reelin", a) && rodState == ST_PAUSED, "/reelin: rod reeled in (paused)");
  check(ask(24, "/ready", a) && rodState != ST_PAUSED, "/ready: rod back to watching");

  // ---------- the watch joins as a second device ----------
  printf("\n=== PHONE + WATCH ===\n");
  int advBefore = BLEDevice::adv().started;
  srv->cb->onConnect(srv);                      // watch connects
  step(0.1);
  check(BLEDevice::adv().started == advBefore, "phone + watch connected: full, the rod stops advertising");

  check(ask(500001, "/mtu?v=200", a) && a.code == 200, "watch says its MTU");
  check(ask(25, "/data", a) && a.body.front() == '{' && a.body.back() == '}', "phone /data still complete with both connected");
  printf("        biggest piece now %zu bytes (smallest MTU wins)\n", biggestPiece);

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
    size_t from = txc->notified.size();
    step(2);
    for (size_t i = from; i < txc->notified.size(); i++) all += txc->notified[i];
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
  srv->cb->onDisconnect(srv);
  step(0.2);
  check(ask(500004, "/watch", a) && a.code == 200, "phone gone: watch still answered");
  check(ask(500005, "/watching", a) && a.body == "OK", "watch button (I'M WATCHING) works");

  // watch pages: last strike (fresh after a rod start) and battery time left
  printf("\n=== WATCH PAGES ===\n");
  check(ask(500006, "/watch", a) && a.body.find("\"batleft\"") != std::string::npos, "/watch has battery time left");
  check(ask(500007, "/laststrike", a) && a.body.find("\"have\":false") != std::string::npos &&
        a.body.find("\"count\":0") != std::string::npos, "/laststrike: no strike yet after start");

  step(25);                                       // some history before the strike
  strikeActive = false;
  rodState = ST_MONITORING;
  startStrike("SLOW PULL");
  step(2);
  check(ask(500008, "/laststrike", a) && a.body.find("\"recording\":true") != std::string::npos, "strike: being recorded (10 s after)");
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
  check(a.body.size() < 3000, "/laststrike small enough for the watch");

  printf("\n%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

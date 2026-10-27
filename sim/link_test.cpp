// Rod <-> watch LoRa link test on the host.
//
// Runs the real rod sketch (StrikeSense.ino) and the real watch link
// (watch_ble/StrikeSense_Watch/lora_link.cpp + link_merge.c) together
// over a virtual radio, with lost packets, walks through a whole
// fishing session, then the Bluetooth <-> LoRa switching.
//
//   W=../watch_ble/StrikeSense_Watch
//   g++ -O2 -std=c++17 -Imock -Imock_lora -I$W -o link_test link_test.cpp $W/lora_link.cpp $W/link_merge.c $W/ss_crypto.c && ./link_test

#define SIM_NO_MAIN
#define ENABLE_LORA 1
#include "scenarios.cpp"
#include "lora_link.h"
#include "ss_crypto.h"
#include <algorithm>
#include "link_merge.h"

SPIClass SPI;

extern "C" int watch_bat_percent(void) { return 77; }

const char* ROD_STATE_NAMES[] = { "LEARNING", "PULLTEST", "MONITORING", "SETTLING", "PAUSED" };

void step(double secs)
{
  double end = simMillis / 1000.0 + secs;

  while (simMillis / 1000.0 < end) {
    simMillis++;
    computeSensor(simMillis / 1000.0);
    loop();                 // rod
    SX1262::tickAir();
    lora_link_poll();       // watch
  }
}

int checks = 0, failed = 0;

void check(bool ok, const char* what)
{
  checks++;
  if (!ok) failed++;
  printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

void show(const char* when)
{
  lora_link_status_t w = lora_link_get_status();
  printf("  %-34s watch sees rod %-10s connected=%d heartbeats=%u strike=%s tilt=%+.1f\n",
         when, ROD_STATE_NAMES[w.rod_state], w.connected, (unsigned)w.hb_count,
         w.strike_type ? lora_link_strike_name(w.strike_type) : "-", w.tilt);
}

// A watch button while on LoRa (what ble_link_command does), until the rod confirms
void ble_link_command_lora(const char* path)
{
  uint32_t utc = 0;
  int tz = 0;
  uint8_t cmd = link_path_to_cmd(path, &utc, &tz);

  if (cmd == CMD_SETTIME) lora_link_send_settime(utc, tz);
  else if (cmd == CMD_LINKMODE || cmd == CMD_CHANMODE) lora_link_send_command_arg(cmd, utc);
  else lora_link_send_command(cmd);

  unsigned long t0 = simMillis;
  while (lora_link_command_pending() && simMillis - t0 < 20000) step(0.05);
  printf("  watch %-32s -> LoRa command %d, confirmed after %.1f s\n", path, cmd, (simMillis - t0) / 1000.0);
  step(0.3);
}

// Watch button: send the command and wait until the rod confirms
bool command(uint8_t cmd, const char* name)
{
  lora_link_send_command(cmd);
  unsigned long t0 = simMillis;
  while (lora_link_command_pending() && simMillis - t0 < 20000) step(0.05);
  bool ok = !lora_link_command_pending();
  printf("  watch -> %-9s confirmed after %.1f s\n", name, (simMillis - t0) / 1000.0);
  return ok;
}

void session(float loss)
{
  printf("\n===== SESSION with %.0f%% of packets lost =====\n", loss * 100);
  simLoraLoss = loss;
  simLoraSent = simLoraLost = 0;

  step(3);
  show("after 3 s:");
  check(lora_link_get_status().connected, "watch hears the rod's heartbeat");

  check(command(CMD_LEARN, "LEARN"), "LEARN confirmed");
  step(1);
  check(rodState == ST_LEARNING, "rod is learning");
  show("learning:");
  step(16);
  show("after learning:");
  check(rodState == ST_PAUSED, "rod paused for the cast after learning");
  for (int i = 0; i < 80 && lora_link_get_status().rod_state != ROD_PAUSED; i++) step(0.1);
  check(lora_link_get_status().rod_state == ROD_PAUSED, "watch knows the rod is paused");

  // pull test only the very first time (then the direction is kept)
  bool firstTime = !haveStoredDir;
  check(command(CMD_READY, "READY"), "READY confirmed");

  if (firstTime) {
    for (int i = 0; i < 40 && rodState != ST_PULLTEST; i++) step(0.5);
    for (int i = 0; i < 50 && lora_link_get_status().rod_state != ROD_PULLTEST; i++) step(0.1);
    show("after settling:");
    check(lora_link_get_status().rod_state == ROD_PULLTEST, "first time: watch shows the PULL TEST screen");
  } else {
    for (int i = 0; i < 40 && rodState != ST_MONITORING; i++) step(0.5);
    show("after settling:");
    check(rodState == ST_MONITORING, "direction already known: no pull test, straight to watching");
  }

  if (firstTime) pullTest(simMillis / 1000.0 + 0.5);
  for (int i = 0; i < 20 && rodState != ST_MONITORING; i++) step(0.5);
  for (int i = 0; i < 100 && lora_link_get_status().rod_state != ROD_MONITORING; i++) step(0.1);
  show("after pull test:");
  check(lora_link_get_status().rod_state == ROD_MONITORING, "watch shows MONITORING");
  check(watchConnected(), "rod dashboard: watch CONNECTED");
  check(watchReported && loraBars(watchRssi) == 4, "rod dashboard: 4 bars, watch reports how it hears the rod");

  // A fish
  unsigned long before = strikeEventID;
  events.push_back({ (float)(simMillis / 1000.0 + 1), 1, 3.0, 2, 20 });   // fish keeps the line pulled
  unsigned long t0 = simMillis;
  while (strikeEventID == before && simMillis - t0 < 20000) step(0.05);
  unsigned long strikeAt = simMillis;
  while (lora_link_get_status().strike_id != (uint16_t)strikeEventID && simMillis - strikeAt < 10000) step(0.01);
  printf("  rod: %s -> watch knows after %.2f s\n", strikeType.c_str(), (simMillis - strikeAt) / 1000.0);
  check(lora_link_get_status().strike_type == STRIKE_SLOW, "watch shows SLOW PULL");
  // with lost packets the confirmation may need a few tries: wait up to 10 s
  unsigned long ackFrom = simMillis;
  while (strikeNeedsAck && simMillis - ackFrom < 10000) step(0.05);
  printf("  rod got the watch's confirmation after %.1f s\n", (simMillis - ackFrom) / 1000.0);
  check(!strikeNeedsAck, "rod got the watch's confirmation (stops repeating)");

  check(command(CMD_WATCHING, "WATCHING"), "I'M WATCHING confirmed");
  step(1);
  check(!strikeActive, "rod alarm stopped");
  for (int i = 0; i < 30 && rodState != ST_MONITORING; i++) step(0.5);
  check(rodState == ST_MONITORING, "rod armed again");

  check(command(CMD_REELIN, "REELIN"), "REEL IN confirmed");
  step(1);
  check(rodState == ST_PAUSED, "rod paused");

  check(command(CMD_RESUME, "RESUME"), "ADJUST -> READY (resume) confirmed");
  for (int i = 0; i < 30 && rodState != ST_MONITORING; i++) step(0.5);
  check(rodState == ST_MONITORING, "rod armed again without pull test");

  // Link loss
  simLoraLoss = 1.0;
  step(7);
  step(10);
  check(!lora_link_get_status().connected, "watch notices the link is lost");
  check(!watchConnected(), "rod dashboard: watch NOT CONNECTED");
  simLoraLoss = loss;
  unsigned long back = simMillis;
  while (!lora_link_get_status().connected && simMillis - back < 40000) step(0.1);
  printf("  link back after %.1f s (the rod may have changed channel: the watch searches)\n", (simMillis - back) / 1000.0);
  check(lora_link_get_status().connected, "watch reconnects by itself");

  printf("  air: %lu packets, %lu lost\n", simLoraSent, simLoraLost);
}

// ---------- Bluetooth <-> LoRa switching ----------

// The Bluetooth status as ble_link.cpp would give it
ble_link_status_t btStatus(bool fresh)
{
  ble_link_status_t b;
  memset(&b, 0, sizeof(b));
  b.phase = fresh ? BLE_LINK_CONNECTED : BLE_LINK_SEARCHING;
  b.seq = fresh ? 500 : 0;
  b.age_ms = fresh ? 120 : 0xFFFFFFFF;
  strcpy(b.state, "monitoring");
  b.knockid = (long)knockAlertID;
  b.knockthr = knockThreshold;
  b.allow = pullAllow;
  b.bat = -1;
  b.batleft = -1;
  return b;
}

ble_link_status_t screens(bool btFresh, bool takePeak = true)
{
  ble_link_status_t s = btStatus(btFresh);
  lora_link_status_t lo = lora_link_get_status();
  link_merge(&s, &lo, simMillis, takePeak);
  return s;
}

// Rod heartbeats sent in "secs" seconds
SX1262* watchRadio();

int heartbeatsIn(double secs)
{
  uint32_t before = lora_link_get_status().hb_count;
  step(secs);
  return (int)(lora_link_get_status().hb_count - before);
}

// SX1262 time on air, ms (explicit header, CRC on)
double airtimeMs(int bytes, int sf, double bwKHz, int cr, int preamble)
{
  double ts = (1 << sf) / bwKHz;
  int de = ts > 16 ? 1 : 0;
  double n = 8 * bytes - 4 * sf + 28 + 16;
  int payload = 8 + (int)fmax(ceil(n / (4.0 * (sf - 2 * de))) * (cr), 0);
  return (preamble + 4.25 + payload) * ts;
}

// Let the fish go (events end), answer any alarm, back to armed and calm
void settle()
{
  step(25);
  events.clear();
  if (strikeActive) cmdWatching();
  cmdReelIn();              // as after landing a fish: reel in, cast, resume
  step(1);
  cmdResume();
  for (int i = 0; i < 40 && rodState != ST_MONITORING; i++) step(0.5);
  step(6);
}

void switching()
{
  printf("\n===== BLUETOOTH <-> LORA =====\n");
  simLoraLoss = 0;
  settle();

  // the extra heartbeat fields (rod 2.3)
  lora_link_status_t w = lora_link_get_status();
  check(w.ext, "rod sends the new heartbeat (rod 2.3 fields)");
  check(fabs(w.allow - pullAllow) < 0.02, "pull allowance reaches the watch over LoRa");
  // (compared right after a heartbeat: on the 1 km channel they come every 1.5 s)
  uint32_t hbAt = w.hb_count;
  float sent = fastTiltDeg;
  for (int i = 0; i < 400 && lora_link_get_status().hb_count == hbAt; i++) { sent = fastTiltDeg; step(0.01); }
  w = lora_link_get_status();
  check(fabs(w.ftilt - sent) < 0.3, "fast tilt (the ring) reaches the watch");
  check(((w.flags & SS_F_DIR) != 0) == haveDir, "pull direction learned: same on the watch");

  // Bluetooth near: the screens use Bluetooth, untouched
  ble_link_status_t s = screens(true);
  check(s.via == LINK_BT && link_merge_via() == LINK_BT, "rod near: Bluetooth is used");
  check(s.seq == 500, "Bluetooth data passed through as it is");

  // walk away: Bluetooth gone, LoRa takes over
  s = screens(false);
  check(s.via == LINK_LORA, "Bluetooth lost: LoRa takes over at once");
  check(s.phase == BLE_LINK_CONNECTED && s.seq > 0 && s.age_ms < 4000, "screens see a connected rod (no 'rod lost' buzz)");
  check(strcmp(s.state, "monitoring") == 0, "rod state over LoRa: monitoring");
  check(s.allow > 0.05 && fabs(s.allow - pullAllow) < 0.02, "pull allowance for the ring");

  // a knock: the knock number counts on, the peak shows
  long k0 = s.knockid;
  events.push_back({ (float)(simMillis / 1000.0 + 0.2), 0, 0.06, 0, 0 });
  step(2);
  s = screens(false);
  printf("  knock: id %ld -> %ld, peak %.1f x the knock limit\n", k0, s.knockid, s.energy_peak / s.knockthr);
  check(s.knockid > k0, "knock over LoRa: the watch buzzes for it");
  check(s.knockid - k0 <= 3, "knock number counts on (no jump from the 1-byte id)");
  check(s.energy_peak / s.knockthr >= 0.3, "knock peak reaches the watch (graph spike)");
  s = screens(false);
  check(s.energy_peak == 0, "peak taken once (graph shows one spike)");

  // heartbeat rate per channel: calm / something happening
  for (int ch = 0; ch < 2; ch++) {
    setChanMode(ch == 0 ? CHM_FAST : CHM_LONG1);
    settle();
    for (int i = 0; i < 100 && lora_link_get_status().chan != curChan; i++) step(0.2);
    int calm = heartbeatsIn(12);
    events.push_back({ (float)(simMillis / 1000.0 + 0.1), 1, pullAllow * 1.6f, 2.0, 8 });   // slow building pull
    int busy = heartbeatsIn(6);
    bool fastCh = curChan == CH_FAST;
    printf("  %s: %d heartbeats in 12 s calm, %d in 6 s while the line is pulled\n", fastCh ? "FAST 500 m" : "LONG RANGE 1 km", calm, busy);
    if (fastCh) {
      check(calm >= 11 && calm <= 13, "fast channel, calm: 1 a second");
      check(busy >= 15, "fast channel, pull building: 3-4 a second (smooth ring)");
    } else {
      check(calm >= 7 && calm <= 9, "long range, calm: 1 every 1.5 s (10% rule)");
      check(busy >= 5, "long range, pull building: about 1 a second");
    }
    // airtime used on this channel
    ss_heartbeat_t hb; (void)hb;
    double air = SX1262::airtimeMs(sizeof(ss_heartbeat_t) + SS_WIRE_OVERHEAD, SS_CHANNELS[curChan].sf, SS_CHANNELS[curChan].khz);
    double share = air * (fastCh ? 1.0 : 1 / 1.5) / 10.0;
    printf("    heartbeat %d bytes on air %.1f ms: %.1f%% of the time when calm%s\n", (int)(sizeof(ss_heartbeat_t) + SS_WIRE_OVERHEAD), air, share,
           SS_CHANNELS[curChan].limited ? " (limit 10%)" : " (no limit on this band)");
    if (!fastCh) check(share < 10 && air / 1.1 / 10.0 < 10, "long range stays under the 10% transmit time (calm and busy)");
  }
  setChanMode(CHM_AUTO);
  settle();

  // a strike far away: on the watch over LoRa
  settle();
  unsigned long before = strikeEventID;
  events.push_back({ (float)(simMillis / 1000.0 + 0.5), 1, 3.0, 2, 20 });
  unsigned long t0 = simMillis;
  while (strikeEventID == before && simMillis - t0 < 20000) step(0.05);
  unsigned long at = simMillis;
  while (!screens(false).strike && simMillis - at < 5000) step(0.01);
  s = screens(false);
  printf("  strike: rod %s, watch shows '%s' after %.2f s\n", strikeType.c_str(), s.type, (simMillis - at) / 1000.0);
  check(s.strike && strcmp(s.type, strikeType.c_str()) == 0, "strike over LoRa: same name as Bluetooth");
  check(simMillis - at < 1000, "strike on the wrist within 1 s");

  // the buttons go over LoRa
  ble_link_command_lora("/watching");
  check(!strikeActive, "I'M WATCHING over LoRa: rod alarm stopped");

  for (int i = 0; i < 30 && rodState != ST_MONITORING; i++) step(0.5);
  ble_link_command_lora("/teachdir");
  check(rodState == ST_PULLTEST, "NEW PULL DIRECTION over LoRa: rod starts a pull test");
  ble_link_command_lora("/skippull");
  check(rodState == ST_MONITORING, "skip pull test over LoRa");
  ble_link_command_lora("/relearn");
  check(rodState == ST_LEARNING && !learnThenPause, "LEARN SEA over LoRa: learns, then stays armed");
  step(17);

  clockSet = false;
  ble_link_command_lora("/settime?t=1760000000&tz=120");
  check(clockSet && nowEpoch() >= 1760000000UL && nowEpoch() < 1760000100UL && clockTzMin == 120,
        "watch gives a restarted rod the time over LoRa");
  step(2);
  s = screens(false);
  check(s.time >= 1760000000L && s.tz == 120, "rod time and zone come back over LoRa");

  // the link and channel choices set on the watch reach the rod over LoRa too
  ble_link_command_lora("/lorachan?m=long2");
  check(chanMode == CHM_LONG2, "LoRa channel set on the watch (over LoRa): the rod keeps 1 km B");
  step(3);
  check(lora_link_get_status().chm == CHM_LONG2, "... and tells the watch back (it shows 1 km B)");
  ble_link_command_lora("/lorachan?m=auto");
  ble_link_command_lora("/linkmode?m=lora");
  check(linkMode == 2, "watch link set on the watch (over LoRa): LoRa only");
  ble_link_command_lora("/linkmode?m=auto");
  check(linkMode == 0 && chanMode == CHM_AUTO, "back to Auto / Auto");

  // the phone app changes things while the rod does NOT hear the watch (the
  // watch's LoRa sending broken, it still hears the rod): it must follow at once
  {
    simDeafTx = watchRadio();
    step(20);
    setChanMode(CHM_LONG1);                      // the app: LoRa channel 1 km A
    for (int i = 0; i < 60 && (lora_link_get_status().chan != CH_LONG1 || curChan != CH_LONG1); i++) step(0.1);
    lora_link_status_t st = lora_link_get_status();
    printf("  rod -> 1 km while deaf to the watch: watch on %d after the change, connected %d\n", st.chan, st.connected);
    check(curChan == CH_LONG1 && st.chan == CH_LONG1, "app changes the channel, rod can't hear the watch: the watch follows within 6 s");
    step(4);
    check(lora_link_get_status().connected, "... and keeps the rod");
    setLinkMode(1);                              // the app: Bluetooth only
    step(4);
    check(lora_link_get_status().lmode == 1, "app chooses Bluetooth only: the watch hears it over LoRa at once");
    step(12);
    int quiet = heartbeatsIn(5);
    check(quiet == 0, "... and after 10 s the rod's LoRa is quiet");
    setLinkMode(0);
    setChanMode(CHM_AUTO);
    simDeafTx = nullptr;
    step(20);
  }

  uint32_t u; int tz;
  check(link_path_to_cmd("/ready", &u, &tz) == CMD_READY && link_path_to_cmd("/reelin", &u, &tz) == CMD_REELIN &&
        link_path_to_cmd("/watch", &u, &tz) == 0, "every button path has its LoRa command");

  // the phone app's link choice
  linkMode = 2;
  step(2.5);
  check((lora_link_get_status().flags & SS_F_LORA_ONLY) != 0, "LoRa only chosen: the heartbeat tells the watch (it leaves Bluetooth)");
  linkMode = 1;
  step(0.5);                              // a heartbeat already on the air still arrives
  uint32_t hb0 = lora_link_get_status().hb_count;
  unsigned long sent0 = simLoraSent;
  step(6);
  check(lora_link_get_status().hb_count == hb0, "Bluetooth only: the rod's LoRa stays quiet (saves battery)");
  printf("  Bluetooth only: %lu packets in 6 s\n", simLoraSent - sent0);
  linkMode = 0;
  step(2.5);
  check(lora_link_get_status().hb_count > hb0 && !(lora_link_get_status().flags & SS_F_LORA_ONLY), "Auto again: heartbeats back");

  // back near: Bluetooth again
  s = screens(true);
  check(s.via == LINK_BT, "rod near again: back to Bluetooth");

  // both gone
  simLoraLoss = 1.0;
  step(6);
  s = screens(false);
  check(s.via == LINK_NONE && s.phase != BLE_LINK_CONNECTED, "both links gone: 'looking for the rod'");
  simLoraLoss = 0;
  unsigned long back2 = simMillis;
  while (screens(false).via != LINK_LORA && simMillis - back2 < 40000) step(0.1);
  printf("  LoRa back after %.1f s\n", (simMillis - back2) / 1000.0);
  check(screens(false).via == LINK_LORA, "LoRa back by itself");
}

// ---------- private LoRa, auto channel, crowd ----------

SX1262* watchRadio()
{
  for (SX1262* r : SX1262::all()) if (r != &radio && r->cb != nullptr && r->x == SX1262::all()[0]->x + 0 && r != nullptr) {}
  for (SX1262* r : SX1262::all()) if (r != &radio) return r;
  return nullptr;
}

// another fisherman's rod: its own key, its own tag, a heartbeat now and then
struct FakeRod
{
  SX1262* r;
  ss_aes_t aes;
  uint16_t tag;
  uint32_t ctr = 1;
  unsigned long next = 0;
  int gap;

  FakeRod(uint16_t t, float x, int ch, int g) : tag(t), gap(g)
  {
    r = new SX1262(new Module(0, 0, 0));
    r->x = x;
    const ss_channel_t& c = SS_CHANNELS[ch];
    r->setFrequency(c.mhz); r->setBandwidth(c.khz); r->setSpreadingFactor(c.sf); r->setOutputPower(c.dbm);
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(t * 7 + i * 13);
    ss_aes_init(&aes, key);
    next = simMillis + rand() % g;
  }

  void tick()
  {
    if (simMillis < next || r->txing) return;
    next = simMillis + gap + rand() % 200;
    if (r->scanChannel() == RADIOLIB_LORA_DETECTED) { next = simMillis + 30 + rand() % 50; return; }
    uint8_t out[64];
    ss_wire_t w = { SS_MAGIC, 0, tag, ctr++ };
    memcpy(out, &w, sizeof(w));
    for (size_t i = sizeof(w); i < sizeof(ss_heartbeat_t) + SS_WIRE_OVERHEAD; i++) out[i] = (uint8_t)rand();
    r->startTransmit(out, sizeof(ss_heartbeat_t) + SS_WIRE_OVERHEAD);
  }
};

std::vector<FakeRod*> fakes;

void stepAll(double secs)
{
  double end = simMillis / 1000.0 + secs;
  while (simMillis / 1000.0 < end) {
    for (FakeRod* f : fakes) f->tick();
    step(0.001);
  }
}

void privateLora()
{
  printf("\n===== PRIVATE LORA: OTHER RODS, COPIED AND CHANGED PACKETS =====\n");
  simLoraLoss = 0;
  setChanMode(CHM_FAST);              // (the channel kept still: this is about other rods)
  settle();
  for (int i = 0; i < 120 && (curChan != CH_FAST || lora_link_get_status().chan != CH_FAST || !lora_link_get_status().connected); i++) stepAll(0.5);
  SX1262* wr = watchRadio();

  // a friend's rod right next to us, same channel
  FakeRod* f = new FakeRod(0x91C7, 5, curChan, 900);
  fakes.push_back(f);
  lora_link_status_t before = lora_link_get_status();
  stepAll(8);
  lora_link_status_t after = lora_link_get_status();
  check(othersCount() == 1, "the rod counts the friend's rod (1 rod near)");
  check(after.rod_state == before.rod_state && after.connected, "the watch ignores the friend's rod (still shows ours only)");

  // someone copies one of our rod's packets and sends it again later
  std::vector<uint8_t> copy;
  for (int i = 0; i < 3000 && copy.empty(); i++) { step(0.001); if (radio.txing && radio.txData.size() > 40) copy = radio.txData; }
  stepAll(2);
  linkMode = 1;                     // our rod quiet now: only the copy could reach the watch
  stepAll(1);
  uint32_t hb = lora_link_get_status().hb_count;
  f->r->startTransmit(copy.data(), copy.size());
  for (int i = 0; i < 200; i++) step(0.001);
  check(lora_link_get_status().hb_count == hb, "a copied (replayed) packet of our rod is ignored");

  // a changed packet (one byte different)
  std::vector<uint8_t> bad = copy;
  ((ss_wire_t*)bad.data())->ctr += 5000;
  bad[20] ^= 0x40;
  hb = lora_link_get_status().hb_count;
  f->r->startTransmit(bad.data(), bad.size());
  for (int i = 0; i < 200; i++) step(0.001);
  check(lora_link_get_status().hb_count == hb, "a changed packet is refused (signature wrong)");
  linkMode = 0;

  fakes.clear();
  delete f;
  SX1262::all().erase(std::remove(SX1262::all().begin(), SX1262::all().end(), f->r), SX1262::all().end());
  (void)wr;
}

void autoChannelTest()
{
  printf("\n===== AUTO CHANNEL: 500 m FAST, 1 km LONG RANGE =====\n");
  simLoraLoss = 0;
  setChanMode(CHM_AUTO);
  SX1262* wr = watchRadio();
  wr->x = 50;
  for (int i = 0; i < 200 && curChan != CH_FAST; i++) stepAll(1);
  settle();
  check(curChan == CH_FAST && lora_link_get_status().chan == CH_FAST, "near (50 m): fast channel");

  // walk to 800 m: too far for the fast channel
  wr->x = 800;
  unsigned long t0 = simMillis;
  while ((curChan == CH_FAST || !lora_link_get_status().connected || lora_link_get_status().chan != curChan) && simMillis - t0 < 60000) step(0.2);
  printf("  at 800 m: rod on %s after %.1f s, watch %s\n", curChan == CH_FAST ? "FAST" : "LONG RANGE", (simMillis - t0) / 1000.0,
         lora_link_get_status().connected ? "connected" : "lost");
  check(curChan != CH_FAST && lora_link_get_status().chan == curChan && lora_link_get_status().connected, "far (800 m): both on long range, connected");

  // a strike far away
  unsigned long before = strikeEventID;
  events.push_back({ (float)(simMillis / 1000.0 + 0.5), 1, 3.0, 2, 20 });
  unsigned long ts = simMillis;
  while (strikeEventID == before && simMillis - ts < 20000) step(0.05);
  unsigned long at = simMillis;
  while (lora_link_get_status().strike_id != (uint16_t)strikeEventID && simMillis - at < 10000) step(0.01);
  printf("  strike at 800 m: on the wrist after %.2f s\n", (simMillis - at) / 1000.0);
  check(simMillis - at < 1000, "strike at 800 m arrives within 1 s");
  settle();

  // back near: fast again after a strong minute
  wr->x = 30;
  t0 = simMillis;
  while (curChan != CH_FAST && simMillis - t0 < 150000) step(0.5);
  for (int i = 0; i < 100 && lora_link_get_status().chan != CH_FAST; i++) step(0.1);
  printf("  back at 30 m: fast channel after %.0f s\n", (simMillis - t0) / 1000.0);
  check(curChan == CH_FAST && lora_link_get_status().chan == CH_FAST, "back near: fast channel again (after a strong minute)");
}

void crowdTest()
{
  printf("\n===== CROWD: 12 OTHER RODS ON OUR CHANNEL =====\n");
  simLoraLoss = 0;
  setChanMode(CHM_FAST);
  SX1262* wr = watchRadio();
  wr->x = 30;
  settle();
  for (int i = 0; i < 12; i++) fakes.push_back(new FakeRod(0x1000 + i, 10 + i * 3, CH_FAST, 1000));
  stepAll(15);
  printf("  rod hears %d rods, crowd mode %s, quality %d, calm news every %lu ms\n", othersCount(), crowd ? "ON" : "off", loraQuality(), calmGapMs());
  check(crowd && loraQuality() == Q_CONGESTED, "12 rods near: crowd mode ON, quality Congested");
  check(calmGapMs() >= 4000, "calm news less often (every few seconds)");
  stepAll(3);
  check(lora_link_get_status().crowd && lora_link_get_status().quality == Q_CONGESTED, "the watch shows Congested (crowd)");

  int ok = 0, tries = 5;
  double worst = 0;
  for (int k = 0; k < tries; k++) {
    unsigned long before = strikeEventID;
    events.push_back({ (float)(simMillis / 1000.0 + 0.3), 1, 3.0, 2, 6 });
    unsigned long ts = simMillis;
    while (strikeEventID == before && simMillis - ts < 20000) stepAll(0.01);
    unsigned long at = simMillis;
    while (lora_link_get_status().strike_id != (uint16_t)strikeEventID && simMillis - at < 10000) stepAll(0.002);
    double d = (simMillis - at) / 1000.0;
    worst = d > worst ? d : worst;
    if (d < 1.5) ok++;
    cmdWatching();
    stepAll(12);
    events.clear();
  }
  printf("  %d of %d strikes on the wrist within 1.5 s, slowest %.2f s; collisions so far %lu\n", ok, tries, worst, simLoraCollided);
  check(ok == tries, "crowd: every strike arrives within 1.5 s");

  for (FakeRod* f : fakes) { SX1262::all().erase(std::remove(SX1262::all().begin(), SX1262::all().end(), f->r), SX1262::all().end()); delete f; }
  fakes.clear();
  stepAll(35);
  check(!crowd && loraQuality() == Q_CLEAR, "the others leave: crowd mode off, Clear");
  setChanMode(CHM_AUTO);
}

int main()
{
  Serial.quiet = true;
  verbose = false;

  Env calm; calm.waveA = 0.15; calm.waveP = 5; calm.windA = 1;
  env = calm;

  setup();
  lora_link_begin();
  lora_link_set_key(rodTag, loraKey);     // what the watch gets over the Bluetooth pairing (/lorakey)
  step(17);          // rod's own start-up learning
  handleSkipPull();

  session(0.0);
  session(0.3);
  switching();
  privateLora();
  autoChannelTest();
  crowdTest();

  printf("\n%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

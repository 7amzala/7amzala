// Rod <-> watch LoRa link test on the host.
//
// Runs the real rod sketch (StrikeSense.ino) and the real watch link
// (watch_ble/StrikeSense_Watch/lora_link.cpp + link_merge.c) together
// over a virtual radio, with lost packets, walks through a whole
// fishing session, then the Bluetooth <-> LoRa switching.
//
//   W=../watch_ble/StrikeSense_Watch
//   g++ -O2 -std=c++17 -Imock -Imock_lora -I$W -o link_test link_test.cpp $W/lora_link.cpp $W/link_merge.c && ./link_test

#define SIM_NO_MAIN
#define ENABLE_LORA 1
#include "scenarios.cpp"
#include "lora_link.h"
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
  step(3);
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
  check(fabs(w.ftilt - fastTiltDeg) < 0.3, "fast tilt (the ring) reaches the watch");
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

  // heartbeat rate: 1 a second calm, 2 a second when something happens
  settle();
  int calm = heartbeatsIn(10);
  events.push_back({ (float)(simMillis / 1000.0 + 0.1), 1, pullAllow * 1.6f, 2.0, 8 });   // slow building pull
  int busy = heartbeatsIn(6);
  printf("  heartbeats: %d in 10 s calm, %d in 6 s while the line is pulled\n", calm, busy);
  check(calm >= 9 && calm <= 11, "calm: about 1 heartbeat a second");
  check(busy >= 10, "pull building: about 2 a second (ring moves faster)");

  double air = airtimeMs(sizeof(ss_heartbeat_t), SS_SF, SS_BW_KHZ, SS_CR, SS_PREAMBLE);
  printf("  heartbeat %d bytes = %.1f ms on air: %.1f%% (calm) / %.1f%% (busy) of the 10%% allowed\n",
         (int)sizeof(ss_heartbeat_t), air, air / 10.0, air * 2 / 10.0);
  check(air * 2 / 1000.0 < 0.09, "2 a second stays under the 10% legal transmit time");

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

  uint32_t u; int tz;
  check(link_path_to_cmd("/ready", &u, &tz) == CMD_READY && link_path_to_cmd("/reelin", &u, &tz) == CMD_REELIN &&
        link_path_to_cmd("/watch", &u, &tz) == 0, "every button path has its LoRa command");

  // the phone app's link choice
  linkMode = 2;
  step(2.5);
  check((lora_link_get_status().flags & SS_F_LORA_ONLY) != 0, "LoRa only chosen: the heartbeat tells the watch (it leaves Bluetooth)");
  linkMode = 1;
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
  step(3);
  check(screens(false).via == LINK_LORA, "LoRa back by itself");
}

int main()
{
  Serial.quiet = true;
  verbose = false;

  Env calm; calm.waveA = 0.15; calm.waveP = 5; calm.windA = 1;
  env = calm;

  setup();
  lora_link_begin();
  step(17);          // rod's own start-up learning
  handleSkipPull();

  session(0.0);
  session(0.3);
  switching();

  printf("\n%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

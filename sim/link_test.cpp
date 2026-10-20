// Rod <-> watch LoRa link test on the host.
//
// Runs the real rod sketch (StrikeSense.ino) and the real watch link
// (watch/lora_link.cpp) together over a virtual radio, with lost
// packets, and walks through a whole fishing session.
//
//   g++ -O2 -std=c++17 -Imock -Imock_lora -o link_test link_test.cpp ../watch/lora_link.cpp && ./link_test

#define SIM_NO_MAIN
#define ENABLE_LORA 1
#include "scenarios.cpp"
#include "../watch/lora_link.h"

SPIClass SPI;

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
  for (int i = 0; i < 50 && lora_link_get_status().rod_state != ROD_MONITORING; i++) step(0.1);
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

  printf("\n%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

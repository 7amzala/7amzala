# How the watch talks to the rod, chosen in the phone app:
# Auto (Bluetooth near, LoRa far) / Bluetooth only (LoRa off: saves battery)
# / LoRa only (the watch leaves Bluetooth). Saved on the rod, sent to the
# watch in its Bluetooth answers and in the LoRa heartbeat.
#
#   python3 sim/add_link_mode.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

BLOCK = r'''// ---------- Watch link: auto / Bluetooth only / LoRa only (phone app) ----------

int linkMode = 0;                       // 0 auto, 1 Bluetooth only, 2 LoRa only

const char* linkModeName()
{
  return linkMode == 1 ? "bt" : linkMode == 2 ? "lora" : "auto";
}

void loadLinkMode()
{
  prefs.begin("strikesense", true);
  linkMode = prefs.getInt("lmode", 0);
  prefs.end();

  if(linkMode < 0 || linkMode > 2)
    linkMode = 0;
}

// /linkmode?m=auto|bt|lora
void handleLinkMode()
{
  String m = server.arg("m");
  int v = m == "auto" ? 0 : m == "bt" ? 1 : m == "lora" ? 2 : -1;

  if(v < 0)
  {
    server.send(400, "text/plain", "BAD MODE");
    return;
  }

  linkMode = v;

  prefs.begin("strikesense", false);
  prefs.putInt("lmode", v);
  prefs.end();

  Serial.printf("WATCH LINK: %s\n", linkModeName());
  server.send(200, "application/json", String("{\"lmode\":\"") + linkModeName() + "\"}");
}

void addLinkMode(String& json)
{
  bool hasLora = false;

#if defined(ENABLE_LORA) && ENABLE_LORA
  hasLora = loraRadioOk();
#endif

  json += ",\"lmode\":\"";
  json += linkModeName();
  json += "\"";
  addField(json, "haslora", hasLora);
}

'''


def patch(path):
    s = open(path).read()

    if "void handleLinkMode()" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep("// ---------- The watch's link, for the phone ----------\n", BLOCK + "// ---------- The watch's link, for the phone ----------\n")

    rep("""void fightOnStrike();
void updateFight();""", """void fightOnStrike();
void updateFight();
bool loraRadioOk();
void loadLinkMode();
const char* linkModeName();""")

    # /data (phone) and the watch's answer
    rep("""  addWatchLink(json);
""", """  addWatchLink(json);
  addLinkMode(json);
""")
    rep("""  addField(json, "batleft", batLeftMin);
""", """  addField(json, "batleft", batLeftMin);
  json += ",\\"lmode\\":\\"";
  json += linkModeName();
  json += "\\"";
""")

    rep("""  server.on("/wping", handleWatchPing);
""", """  server.on("/wping", handleWatchPing);
  server.on("/linkmode", handleLinkMode);
""")

    rep("""  startWatchdog();                      // from here on: stuck 10 s = restart""",
        """  loadLinkMode();
  startWatchdog();                      // from here on: stuck 10 s = restart""")

    if "void loraPoll()\n{\n  if(!loraOk)" in s:
        rep("""  p.flags = (dropWatch ? SS_F_DROPWATCH : 0) | (haveDir ? SS_F_DIR : 0);""",
            """  p.flags = (dropWatch ? SS_F_DROPWATCH : 0) | (haveDir ? SS_F_DIR : 0) | (linkMode == 2 ? SS_F_LORA_ONLY : 0);""")

        # Bluetooth only: the rod's LoRa stays quiet (still answers a watch command)
        rep("""  if(strikeNeedsAck && strikeActive && (long)(now - nextStrikeSend) >= 0)
  {
    sendStrike();""", """  if(linkMode == 1)
    return;

  if(strikeNeedsAck && strikeActive && (long)(now - nextStrikeSend) >= 0)
  {
    sendStrike();""")

        rep("""// Watch heard in the last 15 s (it reports every 5 s)
bool watchConnected()""", """bool loraRadioOk()
{
  return loraOk;
}

// Watch heard in the last 15 s (it reports every 5 s)
bool watchConnected()""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

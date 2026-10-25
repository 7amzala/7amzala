# The link choice and the LoRa channel choice, set on the watch too:
# the watch sends them to the rod (Bluetooth: /linkmode, /lorachan; LoRa:
# CMD_LINKMODE, CMD_CHANMODE), the rod keeps them and tells both the watch
# (heartbeat "modes", Bluetooth answer "lmode"/"lchm") and the app (/data).
#
#   python3 sim/add_link_sync.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]


def patch(path):
    s = open(path).read()

    if "CMD_LINKMODE:" in s or "addWatchChanMode" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    lora = "void handleRaw(" in s

    # the watch's Bluetooth answer: the channel choice too (LoRa rods)
    rep("""  addField(json, "batleft", batLeftMin);
  json += ",\\"lmode\\":\\"";
  json += linkModeName();
  json += "\\"";
""", """  addField(json, "batleft", batLeftMin);
  json += ",\\"lmode\\":\\"";
  json += linkModeName();
  json += "\\"";
  addWatchChanMode(json);
""")

    if lora:
        rep("""void addLoraChannel(String& json);""", """void addLoraChannel(String& json);
void addWatchChanMode(String& json);""")
        rep("""    case CMD_SETTIME:  setClock((long)c.arg, (long)c.tz15 * 15); break;""",
            """    case CMD_SETTIME:  setClock((long)c.arg, (long)c.tz15 * 15); break;
    case CMD_LINKMODE: setLinkMode((int)c.arg); break;
    case CMD_CHANMODE: setChanMode((int)c.arg); break;""")
        rep("""  p.gap10 = (uint8_t)min(calmGapMs() / 100, 255UL);""", """  p.gap10 = (uint8_t)min(calmGapMs() / 100, 255UL);
  p.modes = (uint8_t)((linkMode & 3) | (chanMode << 2));""")
        rep("""void addLoraChannel(String&) {}
""", """void addLoraChannel(String&) {}
void addWatchChanMode(String&) {}
""")
        rep("""// the app's watch line: channel, quality, rods near
void addLoraChannel(String& json)""", """// the watch's Bluetooth answer: the channel choice (its SETUP page shows it)
void addWatchChanMode(String& json)
{
  addField(json, "lchm", (long)chanMode);
}

// the app's watch line: channel, quality, rods near
void addLoraChannel(String& json)""")
    else:
        rep("""void startWatchdog();
const char* resetReason();""", """void startWatchdog();
const char* resetReason();
void addWatchChanMode(String& json);""")
        rep("""// ---------- Watch link: auto / Bluetooth only / LoRa only (phone app) ----------
""", """void addWatchChanMode(String&) {}      // no LoRa on this rod

// ---------- Watch link: auto / Bluetooth only / LoRa only (phone app) ----------
""")

    # /linkmode and the watch's LoRa command share one setter
    rep("""  linkMode = v;

  prefs.begin("strikesense", false);
  prefs.putInt("lmode", v);
  prefs.end();

  Serial.printf("WATCH LINK: %s\\n", linkModeName());
  server.send(200, "application/json", String("{\\"lmode\\":\\"") + linkModeName() + "\\"}");
}""", """  setLinkMode(v);
  server.send(200, "application/json", String("{\\"lmode\\":\\"") + linkModeName() + "\\"}");
}

void setLinkMode(int v)
{
  if(v < 0 || v > 2)
    return;

  linkMode = v;

  prefs.begin("strikesense", false);
  prefs.putInt("lmode", v);
  prefs.end();

  Serial.printf("WATCH LINK: %s\\n", linkModeName());
}""")
    rep("""void loadLinkMode();
const char* linkModeName();""", """void loadLinkMode();
const char* linkModeName();
void setLinkMode(int v);""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

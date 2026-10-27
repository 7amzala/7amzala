#!/usr/bin/env python3
"""Rod: fight timer from REEL IN to FISH OUT (or Lost it).

After a strike the angler taps REEL IN (watch or app): a timer runs until
FISH OUT or LOST IT is tapped, on the watch or in the app. The time and
the result are kept with the strike (memory, strike log in flash, /strikes,
/log) and shown live (/data, the watch's /watch, LoRa heartbeat).

A REEL IN with no strike in the last 10 minutes (new bait, moving the
rod) starts no timer.

    python3 add_fight_timer.py <sketch.ino>      (edits in place)
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


# ---------- forward declarations (used before the code below) ----------
rep('''void fightOnStrike();
''', '''void fightOnStrike();
void landOnReelIn();
void landTick();
void addLand(String& json);
''')

# ---------- the strike keeps its landing ----------
rep('''  int knocks;
  bool done;
};

StrikeSummary slist[SLIST_N];''', '''  int knocks;
  bool done;
  int16_t land;                         // REEL IN -> FISH OUT / LOST IT, seconds
  uint8_t result;                       // 0 none, 1 fish out, 2 lost it
};

StrikeSummary slist[SLIST_N];''')

rep('''  if(slistCount == 0)
    return;

  StrikeSummary& s = slist[slistCount - 1];

  if(s.done)
    return;
''', '''  landTick();

  if(slistCount == 0)
    return;

  StrikeSummary& s = slist[slistCount - 1];

  if(s.done)
    return;
''')

rep('''    addField(json, "done", s.done);
    json += "}";''', '''    addField(json, "done", s.done);
    addField(json, "land", (long)s.land);
    json += ",\\"result\\":\\"";
    json += s.result == 1 ? "fish" : s.result == 2 ? "lost" : "";
    json += "\\"}";''')

# ---------- strike log: two more numbers at the end (older records still read) ----------
rep('''  int16_t zk[ZOOM_EACH * 2];
  int16_t zt[ZOOM_EACH * 2];
};

const uint32_t LOG_MAGIC''', '''  int16_t zk[ZOOM_EACH * 2];
  int16_t zt[ZOOM_EACH * 2];
  // rod 3.1: the landing (older records end before this: read as 0)
  int16_t land;                         // REEL IN -> FISH OUT / LOST IT, seconds
  int16_t result;                       // 0 none, 1 fish out, 2 lost it
};

const size_t LOG_OLD_SIZE = offsetof(LogRec, land);

const uint32_t LOG_MAGIC''')

rep('''    bool ok = f.read((uint8_t*)&r, sizeof(r)) == sizeof(r) && r.magic == LOG_MAGIC && r.seq == seq;
    f.close();
    return ok;''', '''    memset(&r, 0, sizeof(r));
    size_t n = f.read((uint8_t*)&r, sizeof(r));
    bool ok = (n == sizeof(r) || n == LOG_OLD_SIZE) && r.magic == LOG_MAGIC && r.seq == seq;
    f.close();
    return ok;''')

rep('''    r.fight = (int16_t)((s.lastActiveMs - s.startMs) / 1000);
''', '''    r.fight = (int16_t)((s.lastActiveMs - s.startMs) / 1000);
    r.land = s.land;
    r.result = s.result;
''')

rep('''    Serial.printf("STRIKE LOG: strike %lu saved (%s%s)\\n", (unsigned long)s.seq, r.type, r.count > 1 ? ", with graph" : "");
  }
};''', '''    Serial.printf("STRIKE LOG: strike %lu saved (%s%s)\\n", (unsigned long)s.seq, r.type, r.count > 1 ? ", with graph" : "");
  }

  // FISH OUT after the strike was saved: add the landing to its record
  static void setLand(uint32_t seq, int16_t land, int16_t result)
  {
    static LogRec r;

    if(!read(seq, r))
      return;

    r.land = land;
    r.result = result;

    File f = LittleFS.open(path(seq).c_str(), "w");

    if(f)
    {
      f.write((uint8_t*)&r, sizeof(r));
      f.close();
    }
  }
};

// =====================================================
// LANDING: REEL IN after a strike starts the fight timer, FISH OUT or
// LOST IT (watch or app) stops it. Kept with the strike.
// =====================================================

const unsigned long LAND_AFTER_STRIKE_MS = 600000;   // REEL IN up to 10 min after the strike

int landState = 0;                      // 0 none, 1 fighting, 2 fish out, 3 lost it
unsigned long landStartMs = 0;
unsigned long landEndMs = 0;
unsigned long landStrikeId = 0;

long landSeconds()
{
  if(landState == 0)
    return 0;

  unsigned long end = landState == 1 ? millis() : landEndMs;
  long sec = (long)((end - landStartMs) / 1000);
  return sec > 32000 ? 32000 : sec;
}

void landOnReelIn()
{
  if(landState == 1 || slistCount == 0)
    return;

  StrikeSummary& s = slist[slistCount - 1];

  if(millis() - s.startMs > LAND_AFTER_STRIKE_MS || s.result != 0)
    return;                             // no strike lately: new bait, no fight

  landState = 1;
  landStartMs = millis();
  landEndMs = 0;
  landStrikeId = s.id;
  Serial.println("FIGHT: timer started (REEL IN)");
}

// Every reading: casting again (READY / resume / learning) ends the landing
void landTick()
{
  if(landState != 0 && rodState != ST_PAUSED)
    landState = 0;
}

// FISH OUT (fish = true) or LOST IT
bool cmdFishOut(bool fish)
{
  if(landState != 1)
    return false;

  landState = fish ? 2 : 3;
  landEndMs = millis();

  int16_t sec = (int16_t)landSeconds();

  for(int i = 0; i < slistCount; i++)
  {
    StrikeSummary& st = slist[i];

    if(st.id != landStrikeId)
      continue;

    st.land = sec;
    st.result = fish ? 1 : 2;

    if(st.stored && logReady)
      StrikeLog::setLand(st.seq, st.land, st.result);   // else written with it later
  }

  Serial.printf("FIGHT: %s after %d:%02d\\n", fish ? "FISH OUT" : "LOST IT", sec / 60, sec % 60);
  return true;
}

void handleFishOut()
{
  server.send(200, "text/plain", cmdFishOut(true) ? "OK" : "NO FIGHT");
}

void handleLostFish()
{
  server.send(200, "text/plain", cmdFishOut(false) ? "OK" : "NO FIGHT");
}

void addLand(String& json)
{
  addField(json, "land", (long)landState);
  addField(json, "landsec", landSeconds());
}''')

rep('''      addField(json, "graph", r.count > 1);
      json += "}";''', '''      addField(json, "graph", r.count > 1);
      addField(json, "land", (long)r.land);
      json += ",\\"result\\":\\"";
      json += r.result == 1 ? "fish" : r.result == 2 ? "lost" : "";
      json += "\\"}";''')

# ---------- REEL IN starts it ----------
rep('''  Serial.println("REEL IN - DETECTION PAUSED");
''', '''  Serial.println("REEL IN - DETECTION PAUSED");

  landOnReelIn();
''')

# ---------- live: dashboard (/data) and watch (/watch) ----------
rep('''  addWatchChanMode(json);

  json += "}";''', '''  addWatchChanMode(json);
  addLand(json);

  json += "}";''')

# /data: the first "knockid" after "strikeready" (watchJson has its own)
rep('''  addField(json, "strikeready", lastStrikeReady);''', '''  addField(json, "strikeready", lastStrikeReady);
  addLand(json);''')

rep('''  server.on("/ready", handleReady);
''', '''  server.on("/ready", handleReady);
  server.on("/fishout", handleFishOut);
  server.on("/lostfish", handleLostFish);
''')

# ---------- LoRa: the watch's buttons and the timer ----------
if lora:
    rep('''    case CMD_CHANMODE: setChanMode((int)c.arg); break;
''', '''    case CMD_CHANMODE: setChanMode((int)c.arg); break;
    case CMD_FISHOUT:  cmdFishOut(true); break;
    case CMD_LOSTFISH: cmdFishOut(false); break;
''')
    rep('''  p.modes = (uint8_t)((linkMode & 3) | (chanMode << 2));
''', '''  p.modes = (uint8_t)((linkMode & 3) | (chanMode << 2));

  // rod 3.1: the landing
  p.land = (uint8_t)landState;
  p.landSec = (uint16_t)landSeconds();
''')

# ---------- dashboard: the fight card and its buttons ----------
rep('''<button id="readyButton" class="action ready" onclick="ready()" style="display:none">READY</button>''',
    '''<div id="landCard" class="landCard" style="display:none">
<div class="landTitle" id="landTitle">FIGHT</div>
<div class="landTime" id="landTime">0:00</div>
<div class="landSub" id="landSub">from REEL IN</div>
<button id="landPhotos" class="action fishout" onclick="addPhotos()" style="display:none;margin-top:10px">&#128247; ADD PHOTOS</button>
</div>
<button id="fishOutButton" class="action fishout" onclick="fishOut()" style="display:none">&#127907; FISH OUT</button>
<button id="lostButton" class="action lost" onclick="lostFish()" style="display:none">Lost it</button>
<button id="readyButton" class="action ready" onclick="ready()" style="display:none">READY</button>''')

rep('''.reel {''', '''.landCard {
  border:1px solid #ff8a3d;
  border-radius:16px;
  padding:12px;
  text-align:center;
  margin-bottom:12px;
  background:rgba(255,138,61,.06);
}

.landCard.done { border-color:#3ddc84; background:rgba(61,220,132,.06); }
.landCard.lost { border-color:#ff5d6c; background:rgba(255,93,108,.06); }
.landTitle { color:#ff8a3d; font-weight:800; letter-spacing:1px; font-size:14px; }
.landCard.done .landTitle { color:#3ddc84; }
.landCard.lost .landTitle { color:#ff8a94; }
.landTime { font-size:64px; font-weight:800; font-variant-numeric:tabular-nums; line-height:1.1; }
.landSub { color:#8fa0bf; font-size:13px; }

.fishout {
  background:#3ddc84 !important;
  color:#05210f !important;
}

.lost {
  background:transparent !important;
  border:2px solid #ff5d6c !important;
  color:#ff8a94 !important;
}

.reel {''')

rep('''async function ready()
{''', '''async function fishOut()
{
  await fetch("/fishout");
  if(window.StrikeApp && StrikeApp.fishOut) StrikeApp.fishOut();   // the app: the photo page
}

// in the app: the catch page (3 photos, kg, cm, fish)
function addPhotos()
{
  if(window.StrikeApp && StrikeApp.fishOut) StrikeApp.fishOut();
}

async function lostFish()
{
  await fetch("/lostfish");
}

function fmtLand(sec)
{
  return Math.floor(sec / 60) + ":" + ("0" + (sec % 60)).slice(-2);
}

function showLand(d)
{
  var land = d.land || 0;
  var card = el("landCard");

  card.style.display = land ? "block" : "none";
  el("fishOutButton").style.display = land === 1 ? "block" : "none";
  el("lostButton").style.display = land === 1 ? "block" : "none";

  if(!land)
    return;

  card.className = "landCard" + (land === 2 ? " done" : land === 3 ? " lost" : "");
  el("landTitle").innerText = land === 1 ? "FIGHT" : land === 2 ? "FISH OUT" : "LOST IT";
  el("landTime").innerText = fmtLand(d.landsec || 0);
  var app = !!(window.StrikeApp && StrikeApp.fishOut);
  el("landPhotos").style.display = land === 2 && app ? "block" : "none";
  el("landSub").innerText = land === 1 ? "from REEL IN" : land === 2 ? (app ? "fight time" : "fight time - add photos in the app") : "fight time";
}

async function ready()
{''')

rep('''  else if(d.state === "paused")
  {
    el("readyButton").style.display = "block";
  }''', '''  else if(d.state === "paused")
  {
    el("readyButton").style.display = d.land === 1 ? "none" : "block";   // fighting: FISH OUT / Lost it first
  }''')

rep('''function showButtons(d)
{
''', '''function showButtons(d)
{
  showLand(d);
''')

open(path, 'w').write(s)

#!/usr/bin/env python3
"""Rod: one phone and one watch per rod.

After pairing with the PIN, the app says "I am the phone" (/iam?d=phone)
and the watch "I am the watch" (/iam?d=watch). The rod keeps the first
phone and the first watch (their own Bluetooth address, the same after
every reconnect). Another phone (or watch) is answered 403 TAKEN, its
pairing deleted and its link closed - even with the right PIN.

Changing phone: "Forget my devices" on the old phone, or a new sketch
(a fresh rod). Both forget the phone and the watch.

(NimBLE only, like the user's ESP32 core: with Bluedroid the address is
not read and every paired device is accepted, as before.)

    python3 add_part2.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''void forgetPairedDevices()
{''', '''// "I am the phone" / "I am the watch": the rod keeps one of each
void handleIam()
{
#if defined(ESP32)
  String d = server.arg("d");
  String addr = server.askerAddr();

  if(addr.length() == 0 || (!(d == "phone") && !(d == "watch")))
  {
    server.send(200, "text/plain", "OK");     // (address not known: as before)
    return;
  }

  const char* key = d == "phone" ? "phoneAddr" : "watchAddr";
  Preferences p;
  p.begin("strikesense", false);
  String mine = p.getString(key, "");

  if(mine.length() == 0)
  {
    p.putString(key, addr);
    p.end();
    Serial.printf("Bluetooth: this rod's %s is %s\\n", d.c_str(), addr.c_str());
    server.send(200, "text/plain", "OK");
    return;
  }

  p.end();

  if(mine == addr)
  {
    server.send(200, "text/plain", "OK");
    return;
  }

  Serial.printf("Bluetooth: a second %s (%s) refused - this rod already has one (%s)\\n", d.c_str(), addr.c_str(), mine.c_str());
  server.send(403, "text/plain", "TAKEN");
  server.dropAsker();
#else
  server.send(200, "text/plain", "OK");
#endif
}

void forgetPairedDevices()
{
  Preferences fp;
  fp.begin("strikesense", false);
  fp.remove("phoneAddr");
  fp.remove("watchAddr");
  fp.end();
''')

rep('''  server.on("/ready", handleReady);
''', '''  server.on("/ready", handleReady);
  server.on("/iam", handleIam);
''')

# ---------- live screen: rod / watch lines with symbols (approved mockup) ----------
ROD_SVG = ('<svg class="who" viewBox="0 0 24 24"><path d="M3 21 L19 4" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/>'
           '<circle cx="7" cy="17" r="2.6" fill="none" stroke="currentColor" stroke-width="2"/>'
           '<path d="M19 4 Q22 9 21 15 q1 3 -1.5 3" stroke="currentColor" stroke-width="1.4" fill="none"/></svg>')
WATCH_SVG = ('<svg class="who" viewBox="0 0 24 24"><rect x="8" y="1.5" width="8" height="4" rx="1" fill="currentColor"/>'
             '<rect x="8" y="18.5" width="8" height="4" rx="1" fill="currentColor"/>'
             '<circle cx="12" cy="12" r="6.6" fill="none" stroke="currentColor" stroke-width="2.2"/>'
             '<path d="M12 8.5 V12 L14.5 13.5" stroke="currentColor" stroke-width="1.8" fill="none" stroke-linecap="round"/></svg>')

rep('''<span id="linkInfo"><span id="linkName">Bluetooth</span>''',
    '''<span id="linkInfo"><span class="whoName">''' + ROD_SVG + '''Rod</span><span id="linkName">Bluetooth</span>''')
rep('''<span id="watchInfo"><span>Watch</span><span id="watchVia">&ndash;</span>''',
    '''<span id="watchInfo"><span class="whoName" id="watchWho">''' + WATCH_SVG + '''Watch</span><span id="watchVia">&ndash;</span>''')
rep('''#watchVia {
  font-weight:800;
}''', '''#watchVia {
  font-weight:800;
}

/* rod / watch lines: symbol + name, always one line */
#linkInfo, #watchInfo { white-space:nowrap; }
.whoName { display:inline-flex; align-items:center; gap:4px; font-weight:900; color:#eaf3ff; }
.whoName svg.who { width:15px; height:15px; }
#watchInfo.off .whoName { color:#8fa0bf; }
.viaDim { color:#8fa6c9; font-weight:700; font-size:11px; }''')
rep('''  via.innerHTML = bt ? 'connected &middot; <b style="color:#22a8ff">Bluetooth</b>' :
    lora ? 'connected &middot; <b style="color:#ffc928">LoRa</b> &middot; ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined && d.lqual >= 2 ? ' &middot; ' + QUAL[d.lqual] : '') : 'not connected';''',
    '''  via.innerHTML = bt ? '<b style="color:#22a8ff">Bluetooth</b>' :
    lora ? '<b style="color:#ffc928">LoRa</b> <span class="viaDim">' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined && d.lqual >= 2 ? ' &middot; ' + QUAL[d.lqual] : '') + '</span>' : 'not connected';''')
rep('''  box.className = 'show';
}''', '''  box.className = bt || lora ? 'show' : 'show off';
}''')

# ---------- big fight buttons (approved mockup) ----------
FISH_OK = ('<svg viewBox="0 0 120 80"><path d="M10 40 Q40 8 78 26 L100 12 L94 40 L100 68 L78 54 Q40 72 10 40 Z" fill="#05210f" opacity=".9"/>'
           '<circle cx="28" cy="36" r="4" fill="#3ddc84"/><circle cx="92" cy="22" r="17" fill="#05210f" stroke="#3ddc84" stroke-width="3"/>'
           '<path d="M84 22 l6 6 l11 -12" stroke="#3ddc84" stroke-width="4.5" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg>')
FISH_LOST = ('<svg viewBox="0 0 120 80"><path d="M58 4 L58 30" stroke="#ff8a94" stroke-width="3"/>'
             '<path d="M58 36 L58 44" stroke="#ff8a94" stroke-width="3" stroke-dasharray="3 3"/>'
             '<path d="M52 31 l12 4 M52 37 l12 -4" stroke="#ff5d6c" stroke-width="3" stroke-linecap="round"/>'
             '<path d="M16 62 Q38 44 62 56 L76 48 L72 62 L76 76 L62 68 Q38 80 16 62 Z" fill="none" stroke="#ff8a94" stroke-width="3"/>'
             '<path d="M84 52 q8 -4 14 0 M84 62 q10 0 16 4" stroke="#ff8a94" stroke-width="2" fill="none" stroke-linecap="round"/></svg>')
NO_FISH = ('<svg viewBox="0 0 120 80"><path d="M60 4 L60 44 Q60 62 46 62 Q34 62 34 50" stroke="#a9bbd6" stroke-width="4" fill="none" stroke-linecap="round"/>'
           '<path d="M34 50 l-5 6" stroke="#a9bbd6" stroke-width="4" stroke-linecap="round"/>'
           '<circle cx="86" cy="44" r="16" fill="none" stroke="#a9bbd6" stroke-width="3"/><path d="M75 55 L97 33" stroke="#a9bbd6" stroke-width="3"/></svg>')

rep('''<button id="fishOutButton" class="action fishout" onclick="fishOut()" style="display:none">&#127907; FISH OUT</button>
<button id="lostButton" class="action lost" onclick="lostFish()" style="display:none">Lost it (it got off)</button>
<button id="noFishButton" class="action nofish" onclick="noFish()" style="display:none">No fish (false alarm / new bait)</button>''',
    '<button id="fishOutButton" class="action fishout bigsym" onclick="fishOut()" style="display:none">' + FISH_OK +
    '<span>FISH OUT</span><small>caught it</small></button>\n' +
    '<button id="lostButton" class="action lost bigsym" onclick="lostFish()" style="display:none">' + FISH_LOST +
    '<span>LOST IT</span><small>it got off</small></button>\n' +
    '<button id="noFishButton" class="action nofish bigsym" onclick="noFish()" style="display:none">' + NO_FISH +
    '<span>NO FISH</span><small>false alarm</small></button>')
rep('''.nofish {''', '''button.bigsym {
  display:none;
  flex-direction:column;
  align-items:center;
  justify-content:center;
  gap:4px;
  min-height:150px;
  font-size:18px;
  font-weight:900;
}

button.bigsym svg { width:100%; max-width:110px; height:auto; }
button.bigsym small { font-size:12px; font-weight:700; opacity:.8; }

/* phone: the timer on its own row, the three buttons side by side under it */
@media (max-width:600px) {
  #landCard { flex-basis:100%; }
  button.bigsym { flex:1 1 0; min-width:0; min-height:130px; font-size:15px; }
}

.nofish {''')
# the fight buttons show as flex columns
rep('''  el("fishOutButton").style.display = land === 1 ? "block" : "none";
  el("lostButton").style.display = land === 1 ? "block" : "none";
  el("noFishButton").style.display = land === 1 ? "block" : "none";''',
    '''  el("fishOutButton").style.display = land === 1 ? "flex" : "none";
  el("lostButton").style.display = land === 1 ? "flex" : "none";
  el("noFishButton").style.display = land === 1 ? "flex" : "none";''')

# ---------- watch bars: keep the last real signal (a 0 = "don't know" no longer wipes it) ----------
rep('''  if(server.hasArg("r"))
    watchBtRssi = server.arg("r").toInt();''', '''  int r = server.hasArg("r") ? server.arg("r").toInt() : 0;

  if(r != 0)
    watchBtRssi = r;                    // (0 = the watch did not know: the last real one stays)

  static unsigned long saidAt = 0;

  if(millis() - saidAt > 30000)
  {
    saidAt = millis();
    Serial.printf("Bluetooth: watch signal %d dBm (watch says), %d dBm (rod measures)\\n", watchBtRssi, server.connRssi(watchBtConn));
  }''')

open(path, 'w').write(s)

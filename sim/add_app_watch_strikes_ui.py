# Dashboard (the page the phone app shows): how the watch is connected,
# the MY STRIKES button and the "Phone + watch / Watch only" choice.
#
#   python3 sim/add_app_watch_strikes_ui.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

CSS = r'''
/* WATCH LINK, MY STRIKES, PHONE + WATCH / WATCH ONLY (phone app) */

#watchInfo {
  display:none;
  align-items:center;
  gap:6px;
  margin-top:3px;
  padding:3px 8px;
  border-radius:10px;
  background:#0d1a30;
  border:1px solid #1b2c4c;
  color:#dbe9ff;
  font-size:12px;
  font-weight:700;
}

#watchInfo.show {
  display:inline-flex;
}

#watchVia {
  font-weight:800;
}

.appOnly {
  display:none !important;
}

body.inApp .appOnly {
  display:inline-block !important;
}

body.inApp #whoBox.appOnly {
  display:block !important;
}

#whoBox {
  margin:6px 0 2px auto;
  max-width:330px;
}

.whoSeg {
  display:flex;
  background:#0b1428;
  border:1px solid #24324f;
  border-radius:11px;
  padding:3px;
}

.whoSeg button {
  flex:1;
  border:0;
  background:none;
  color:#8ea6c8;
  border-radius:8px;
  padding:7px 4px;
  font-size:12px;
  font-weight:800;
  cursor:pointer;
}

.whoSeg button.on {
  background:linear-gradient(90deg,#1e7bff,#22d3ff);
  color:#04121a;
}

.whoNote {
  margin-top:4px;
  color:#a9bbd6;
  font-size:10.5px;
  line-height:1.35;
}

.woBanner {
  display:none;
  margin:6px 0 2px auto;
  max-width:330px;
  padding:8px 10px;
  border-radius:11px;
  background:#3a2a08;
  border:1px solid #ffba52;
  color:#ffd38a;
  font-size:11.5px;
  font-weight:700;
  text-align:left;
  line-height:1.35;
}

body.watchOnly .woBanner {
  display:block;
}

body.watchOnly .layout {
  opacity:.35;
  pointer-events:none;
}

@media(max-width:600px) {

  #whoBox,
  .woBanner {
    margin-left:auto;
    margin-right:auto;
  }
}
'''

WATCH_PILL = r'''<br>
<span id="watchInfo"><span>Watch</span><span id="watchVia">&ndash;</span><span class="sigBars" id="wBars"><i></i><i></i><i></i><i></i></span></span>
<script>
// How the watch is connected to the rod (rod 2.3+: "wlink" in /data)
function showWatch(d)
{
  var box = document.getElementById('watchInfo');

  if(d.wlink === undefined)
  {
    box.className = '';
    return;
  }

  var lora = d.wlink == 'lora', bt = d.wlink == 'bt';
  var c = bt ? '#22d3ff' : lora ? '#ffba52' : '#ff4d5e';
  var b = document.querySelectorAll('#wBars i');

  for(var i = 0; i < b.length; i++)
    b[i].style.background = i < d.wbars ? c : '';

  var via = document.getElementById('watchVia');
  via.textContent = bt ? 'Bluetooth' : lora ? 'LoRa' : 'not connected';
  via.style.color = c;
  document.getElementById('wBars').style.display = bt || lora ? '' : 'none';
  box.className = 'show';
}
</script>
'''

BUTTONS = r'''<button class="soundButton" onclick="openSettings()">&#9881; SETTINGS</button>
<button class="soundButton appOnly" onclick="openStrikes()">&#128205; MY STRIKES</button>
</div>

<div class="woBanner" id="woBanner">&#128245; Phone off the rod: <u>WATCH ONLY</u>. No alarms on the phone. It checks every 2 min that the watch is still there.<br><span id="woInfo"></span></div>

<div class="appOnly" id="whoBox">
<div class="whoSeg">
<button id="whoBoth" class="on" onclick="setWho(false)">Phone + watch</button>
<button id="whoWatch" onclick="setWho(true)">Watch only</button>
</div>
<div class="whoNote">Watch only saves phone battery and a little rod battery. Live data and strike alarms are just as fast in both modes.</div>
</div>
<script>
if(window.StrikeApp)
  document.body.classList.add('inApp');

var watchOnly = false;

function openStrikes()
{
  if(window.StrikeApp && StrikeApp.openStrikes)
    StrikeApp.openStrikes();
}

function setWho(only)
{
  if(only == watchOnly || !window.StrikeApp || !StrikeApp.setWatchOnly)
    return;

  if(only && !confirm('Watch only: the phone leaves the rod and gives NO alarms.\nThe watch alerts you. The phone checks every 2 min that the watch is still there.\n\nOK?'))
    return;

  StrikeApp.setWatchOnly(only);
  setWatchOnlyView(only, '');
}

// Called by the phone app every second
function setWatchOnlyView(only, info)
{
  watchOnly = only;
  document.body.classList.toggle('watchOnly', only);
  document.getElementById('whoBoth').className = only ? '' : 'on';
  document.getElementById('whoWatch').className = only ? 'on' : '';
  document.getElementById('woInfo').textContent = info || '';

  if(only)
  {
    el('mainStatus').innerText = 'WATCH ONLY';
    el('connState').innerHTML = '&#9679; PHONE OFF THE ROD';
    el('connState').className = '';
    document.getElementById('linkInfo').style.display = 'none';
    document.getElementById('watchInfo').style.display = 'none';     // the banner says the last check
    stopContinuousAlarm();
  }
  else
  {
    document.getElementById('linkInfo').style.display = '';
    document.getElementById('watchInfo').style.display = '';
  }
}
</script>'''


def patch(path):
    s = open(path).read()

    if "function setWatchOnlyView(" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep("""@media(max-width:850px) {
""", CSS.lstrip("\n") + """
@media(max-width:850px) {
""")

    rep("""  document.getElementById('sigDbm').textContent = ready && rssi ? rssi + ' dBm' : '';
}
</script>
""", """  document.getElementById('sigDbm').textContent = ready && rssi ? rssi + ' dBm' : '';
}
</script>
""" + WATCH_PILL)

    rep("""<button class="soundButton" onclick="openSettings()">&#9881; SETTINGS</button>
</div>""", BUTTONS)

    rep("""function showTimes(d)
{
  el("upTime").innerText = clock(d.up);
""", """function showTimes(d)
{
  el("upTime").innerText = clock(d.up);
  showWatch(d);
""")

    # Watch only: the phone is off the rod, don't keep asking
    rep("""async function pollLoop()
{
  await updateData();
  setTimeout(pollLoop, 100);
}""", """async function pollLoop()
{
  if(watchOnly)
  {
    setTimeout(pollLoop, 500);
    return;
  }

  await updateData();
  setTimeout(pollLoop, 100);
}""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

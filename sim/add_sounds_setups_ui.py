#!/usr/bin/env python3
"""Dashboard (phone app page):
- 4 fishing alarm sounds (screaming reel, reel clicks, rod bell, falling
  tone), one chosen per alarm type in SETTINGS, with a play button. The
  app serves /sounds/*.ogg; without them (rod web page) the old beeps play.
- No ENABLE SOUND / TEST SHARP / TEST SLOW: sound is always on.
- SAVE closes the settings panel ("Saved" note).
- MY SETUPS: Default (fixed) + 5 named setups kept on the rod (add_presets.py).

    python3 add_sounds_setups_ui.py <sketch.ino>      (edits in place)
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


# ---------- CSS ----------
rep('''.sigDbm {
  color:#8fa6c9;
  font-size:10px;
  font-weight:600;
}
''', '''.sigDbm {
  color:#8fa6c9;
  font-size:10px;
  font-weight:600;
}

.presetRow {
  display:flex;
  align-items:center;
  gap:6px;
  margin-bottom:6px;
}

.presetRow .pName {
  flex:1;
  text-align:left;
  background:#050b18;
  border:1px solid #16305a;
  color:#e3eeff;
  border-radius:10px;
  padding:9px 10px;
  font-size:12px;
  font-weight:800;
  cursor:pointer;
}

.presetRow .pName:disabled {
  color:#4d6182;
  font-weight:600;
  font-style:italic;
  border-style:dashed;
  cursor:default;
}

.presetRow .pName.def {
  border-color:#22406e;
  background:#0b1630;
}

.presetRow .pName small {
  display:block;
  color:#7189ad;
  font-size:9px;
  font-weight:600;
  font-style:normal;
  margin-top:2px;
}

.presetRow .pSave,
.presetRow .pDel,
.soundRow .play {
  border:1px solid #22406e;
  background:#0b1630;
  color:#c3d2ea;
  border-radius:10px;
  padding:9px 9px;
  font-size:10px;
  font-weight:900;
  cursor:pointer;
}

.presetRow .pDel.sure {
  border-color:#ff4d5e;
  color:#ff4d5e;
}

#presetNameIn {
  width:100%;
  box-sizing:border-box;
  background:#050b18;
  border:1px solid #22406e;
  border-radius:10px;
  color:#e3eeff;
  padding:9px 10px;
  font-size:12px;
  margin:2px 0 6px;
}

.soundRow {
  display:flex;
  align-items:center;
  gap:8px;
}

.soundRow span {
  flex:1;
  color:#c3d2ea;
  font-size:12px;
  font-weight:700;
}

.soundRow select {
  background:#0b1630;
  color:#e3eeff;
  border:1px solid #22406e;
  border-radius:8px;
  padding:6px 6px;
  font-size:11px;
  font-weight:700;
}

.soundRow .play {
  color:#22d3ff;
  padding:6px 10px;
}

#savedToast {
  position:fixed;
  left:50%;
  bottom:28px;
  transform:translateX(-50%);
  background:#0e2a1c;
  border:1px solid #3ddc84;
  color:#3ddc84;
  border-radius:20px;
  padding:9px 18px;
  font-size:13px;
  font-weight:900;
  z-index:9999;
  opacity:0;
  transition:opacity .25s;
  pointer-events:none;
}

#savedToast.show {
  opacity:1;
}
''')

# ---------- header: only SETTINGS ----------
rep('''<button id="soundButton" class="soundButton" onclick="enableSound()">ENABLE SOUND</button>
<button class="soundButton" onclick="testSharpSound()">TEST SHARP</button>
<button class="soundButton" onclick="testSlowSound()">TEST SLOW</button>
<button class="soundButton" onclick="openSettings()">&#9881; SETTINGS</button>''',
    '''<button class="soundButton" onclick="openSettings()">&#9881; SETTINGS</button>''')

# ---------- sound always on (no button, no beep) ----------
rep('''    el("soundButton").innerText = "SOUND ON";
    el("soundButton").classList.add("enabled");

    keepScreenOn();

    tone(900, 180, 0);''', '''    keepScreenOn();''')

rep('''async function enableSound()
{''', '''// Sound is always on: started when the page opens (the app allows it),
// and again at the first touch in a normal browser (it needs one)
window.addEventListener("load", () => enableSound());
document.addEventListener("pointerdown", () =>
{
  if(!soundEnabled || (audioContext && audioContext.state !== "running"))
    enableSound();
}, { passive: true });

async function enableSound()
{''')

# ---------- fishing sounds ----------
rep('''function playAlarmPattern()
{
  if(!soundEnabled || activeAlarmType === "")
    return;

  if(activeAlarmType === "SHARP STRIKE") sharpPattern();
  else if(activeAlarmType === "SLOW PULL") slowPattern();
  else if(activeAlarmType === "REPEATED BITE") repeatedPattern();
  else if(activeAlarmType.startsWith("DROP")) dropPattern();
}''', '''// ---------- fishing alarm sounds (files from the phone app) ----------

const SOUND_NAMES = {
  reel_scream: "Screaming reel",
  reel_clicks: "Reel clicks",
  rod_bell: "Rod bell",
  falling_tone: "Falling tone"
};

const SOUND_DEFAULT = { strike: "reel_scream", slow: "reel_clicks", knock: "rod_bell", drop: "falling_tone" };
const SOUND_VIBRATE = {
  strike: [300, 100, 300, 100, 600],
  slow: [450, 200, 550],
  knock: [80, 80, 80],
  drop: [500, 150, 250]
};

const audioFiles = {};
let soundFilesOk = true;

function soundFor(cat)
{
  let v = null;

  try { v = localStorage.getItem("snd_" + cat); } catch(e) {}

  return SOUND_NAMES[v] ? v : SOUND_DEFAULT[cat];
}

function alarmCategory(type)
{
  if(type === "SLOW PULL") return "slow";
  if(type === "REPEATED BITE") return "knock";
  if(type.startsWith("DROP")) return "drop";
  return "strike";
}

// Plays /sounds/<name>.ogg; if the files are not there (rod web page), the old beeps
function playSoundFile(name, fallback)
{
  if(!soundFilesOk)
  {
    fallback();
    return;
  }

  try
  {
    let a = audioFiles[name];

    if(!a)
    {
      a = new Audio("/sounds/" + name + ".ogg");
      a.onerror = () => { soundFilesOk = false; };
      audioFiles[name] = a;
    }

    a.volume = 1.0;
    a.currentTime = 0;

    const p = a.play();

    if(p)
      p.catch(() => fallback());
  }
  catch(e)
  {
    fallback();
  }
}

function stopSoundFiles()
{
  for(const k in audioFiles)
  {
    try { audioFiles[k].pause(); } catch(e) {}
  }
}

function playAlarmPattern()
{
  if(!soundEnabled || activeAlarmType === "")
    return;

  const cat = alarmCategory(activeAlarmType);
  const type = activeAlarmType;

  playSoundFile(soundFor(cat), () =>
  {
    if(type === "SHARP STRIKE") sharpPattern();
    else if(type === "SLOW PULL") slowPattern();
    else if(type === "REPEATED BITE") repeatedPattern();
    else if(type.startsWith("DROP")) dropPattern();
    else sharpPattern();
  });

  vibrate(SOUND_VIBRATE[cat]);
}

// The phone app plays the same sounds when it is in the background
function tellAppSounds()
{
  if(window.StrikeApp && StrikeApp.setSound)
  {
    for(const c in SOUND_DEFAULT)
      StrikeApp.setSound(c, soundFor(c));
  }
}

window.addEventListener("load", () => tellAppSounds());

function showSoundChoices()
{
  for(const c in SOUND_DEFAULT)
  {
    if(el("snd_" + c))
      el("snd_" + c).value = soundFor(c);
  }
}

function chooseSound(cat)
{
  const v = el("snd_" + cat).value;

  try { localStorage.setItem("snd_" + cat, v); } catch(e) {}

  tellAppSounds();
  stopSoundFiles();
  playSoundFile(v, () => {});
}

function previewSound(cat)
{
  stopSoundFiles();
  playSoundFile(soundFor(cat), () => {});
}''')

rep('''function knockPattern()
{
  tone(1250, 70, 0);
  vibrate(40);
}''', '''function knockPattern()
{
  playSoundFile(soundFor("knock"), () => tone(1250, 70, 0));
  vibrate(40);
}''')

rep('''  let repeatTime = 1500;

  if(type === "SHARP STRIKE") repeatTime = 1200;
  else if(type === "SLOW PULL") repeatTime = 1800;
  else if(type === "REPEATED BITE") repeatTime = 1400;
  else if(type.startsWith("DROP")) repeatTime = 1900;''', '''  // again when the sound has finished (the old beeps were shorter)
  const cat = alarmCategory(type);
  let repeatTime = soundFilesOk
    ? (cat === "strike" ? 3000 : cat === "slow" ? 3600 : cat === "knock" ? 2800 : 2000)
    : (type === "SHARP STRIKE" ? 1200 : type === "SLOW PULL" ? 1800 : type === "REPEATED BITE" ? 1400 : 1900);''')

rep('''  activeAlarmType = "";

  document.body.classList.remove("strikeFlash");''', '''  activeAlarmType = "";

  stopSoundFiles();

  document.body.classList.remove("strikeFlash");''')

# ---------- settings: SAVE closes, setups ----------
rep('''function openSettings()
{
  el("drawer").classList.add("open");
  settingsCall("/settings");
}''', '''function openSettings()
{
  el("drawer").classList.add("open");
  settingsCall("/settings");
  presetCall("/presets");
  showSoundChoices();
}

// ---------- my setups (5 on the rod) + Default ----------

let presets = [];

function escapeHtml(t)
{
  return String(t).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

async function presetCall(url)
{
  try
  {
    const r = await fetch(url);

    if(r.ok)
    {
      presets = (await r.json()).presets || [];
      showPresets();
    }
  }
  catch(e)
  {
  }
}

function showPresets()
{
  let h = "";

  for(let i = 0; i < 5; i++)
  {
    const n = presets[i] || "";

    h += '<div class="presetRow">' +
         '<button class="pName" onclick="loadPreset(' + i + ')"' + (n ? "" : " disabled") + '>' +
         (n ? escapeHtml(n) : (i + 1) + " \\u2013 empty") + '</button>' +
         '<button class="pSave" onclick="savePreset(' + i + ')">SAVE HERE</button>' +
         (n ? '<button class="pDel" onclick="delPreset(' + i + ', this)">&#10005;</button>' : '') +
         '</div>';
  }

  el("presetList").innerHTML = h;
}

function presetMsg(t)
{
  el("presetMsg").innerText = t;
}

async function loadPreset(i)
{
  try
  {
    const r = await fetch("/presetload?s=" + i);

    if(r.ok)
    {
      showSettings(await r.json());
      presetMsg("Loaded \\u201c" + presets[i] + "\\u201d \\u2013 press SAVE to use it");
    }
  }
  catch(e)
  {
  }
}

function savePreset(i)
{
  const n = el("presetNameIn").value.trim() || presets[i] || "";

  presetCall("/presetsave?s=" + i + "&n=" + encodeURIComponent(n));
  el("presetNameIn").value = "";
  presetMsg("The current settings are kept in place " + (i + 1) + ".");
}

function delPreset(i, b)
{
  if(!b.classList.contains("sure"))
  {
    b.classList.add("sure");
    b.innerText = "SURE?";
    setTimeout(() => { b.classList.remove("sure"); b.innerHTML = "&#10005;"; }, 3000);
    return;
  }

  presetCall("/presetdel?s=" + i);
  presetMsg("Deleted.");
}

function loadDefault()
{
  defaultSettings();
  presetMsg("Default loaded \\u2013 press SAVE to use it");
}''')

rep('''function saveSettings()
{
  settingsCall("/savesettings");
}''', '''// SAVE: the rod keeps the settings, a short "Saved", the panel closes
async function saveSettings()
{
  await settingsCall("/savesettings");

  const t = el("savedToast");
  t.classList.add("show");
  setTimeout(() => t.classList.remove("show"), 1800);

  setTimeout(closeSettings, 500);
}''')

# ---------- settings panel HTML ----------
rep('''<div class="setGroup">LIVE NOW</div>''', '''<div class="setGroup">MY SETUPS</div>

<div class="presetRow">
<button class="pName def" onclick="loadDefault()">&#128274; Default<small>factory settings &ndash; cannot be changed</small></button>
</div>

<div id="presetList"></div>

<input id="presetNameIn" maxlength="24" placeholder="Name, then SAVE HERE (e.g. Calm beach)">
<div class="setHelp" id="presetMsg">Tap a setup to load it, then SAVE. SAVE HERE keeps the current settings in that place.</div>

<div class="setGroup">LIVE NOW</div>''')

rep('''<div class="saveState" id="saveState">&ndash;</div>''', '''<div class="setGroup">ALARM SOUNDS</div>

<div class="setRow soundRow"><span>Strike / run</span><select id="snd_strike" onchange="chooseSound('strike')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('strike')">&#9654;</button></div>
<div class="setRow soundRow"><span>Slow pull</span><select id="snd_slow" onchange="chooseSound('slow')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('slow')">&#9654;</button></div>
<div class="setRow soundRow"><span>Knocks</span><select id="snd_knock" onchange="chooseSound('knock')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('knock')">&#9654;</button></div>
<div class="setRow soundRow"><span>Drop-back</span><select id="snd_drop" onchange="chooseSound('drop')"><option value="reel_scream">Screaming reel</option><option value="reel_clicks">Reel clicks</option><option value="rod_bell">Rod bell</option><option value="falling_tone">Falling tone</option></select><button class="play" onclick="previewSound('drop')">&#9654;</button></div>
<div class="setHelp">Chosen on this phone. The app plays the same sound in the background.</div>

<div class="saveState" id="saveState">&ndash;</div>''')

rep('''</body>

</html>
)rawliteral";''', '''<div id="savedToast">&#10003; Saved</div>

</body>

</html>
)rawliteral";''')

open(path, 'w').write(s)
print("ok", path)

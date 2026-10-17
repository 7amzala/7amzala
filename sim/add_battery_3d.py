#!/usr/bin/env python3
"""Battery percentage for the XIAO ESP32-S3, and two more 3D views.

Battery: the XIAO cannot measure its own battery, so two equal resistors
(100k) make a divider: BAT+ -> 100k -> D1 -> 100k -> GND. The dashboard
shows % and volts (LiPo curve), red and blinking below 15 %.

3D views (next to 3D NOISE, which stays as it was):
  3D WAVES - a sea surface built from what the rod feels: wave height
             from the wave swing, wave speed from the wave period, small
             chop from the wind.
  3D WIND  - a flag on a pole: it hangs when calm and flies and flaps
             harder as the wind on the rod grows. (Strength only: the rod
             cannot tell where the wind comes from.)

    python3 add_battery_3d.py <sketch.ino>      (edits in place)
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


# =====================================================
# FIRMWARE: battery
# =====================================================

rep('''Preferences prefs;
''', '''Preferences prefs;

// Battery: BAT+ -> 100k -> D1 -> 100k -> GND (the XIAO has no battery
// sensing of its own). Nothing wired = "no battery" on the dashboard.
const int BAT_PIN = D1;
const unsigned long BAT_READ_MS = 2000;

float batVolts = 0;                     // smoothed, 0 = no reading
int batPercent = -1;                    // -1 = no battery seen
unsigned long lastBatRead = 0;
bool batLowSaid = false;
''')

rep('''void loop()
{''', '''// ---------- Battery ----------

// Resting LiPo voltage -> percent
int lipoPercent(float v)
{
  static const float V[] = { 3.27, 3.61, 3.69, 3.71, 3.73, 3.75, 3.77, 3.79, 3.80, 3.82, 3.84,
                             3.85, 3.87, 3.91, 3.95, 3.98, 4.02, 4.08, 4.11, 4.15, 4.20 };

  if(v <= V[0])
    return 0;

  for(int i = 1; i < 21; i++)
  {
    if(v < V[i])
      return (int)((i - 1 + (v - V[i - 1]) / (V[i] - V[i - 1])) * 5 + 0.5);
  }

  return 100;
}

void updateBattery()
{
  if(lastBatRead != 0 && millis() - lastBatRead < BAT_READ_MS)
    return;

  lastBatRead = millis();

  // 16 readings: WiFi makes the ADC a little noisy
  float sum = 0, lo = 99, hi = 0;

  for(int i = 0; i < 16; i++)
  {
    float v = analogReadMilliVolts(BAT_PIN) * 2 / 1000.0;   // x2: the divider halves it

    sum += v;

    if(v < lo) lo = v;
    if(v > hi) hi = v;
  }

  float v = sum / 16;

  // Nothing wired: the pin floats (jumps around) or reads near 0
  if(v < 2.8 || v > 4.6 || hi - lo > 0.4)
  {
    batVolts = 0;
    batPercent = -1;
    return;
  }

  batVolts = batVolts == 0 ? v : batVolts * 0.85 + v * 0.15;
  batPercent = lipoPercent(batVolts);

  if(batPercent < 15 && !batLowSaid)
  {
    batLowSaid = true;
    Serial.printf("BATTERY LOW: %d %% (%.2f V)\\n", batPercent, batVolts);
  }
  else if(batPercent > 25)
  {
    batLowSaid = false;
  }
}

void loop()
{''')

rep('''  maybeSaveTotalTime();
''', '''  maybeSaveTotalTime();

  updateBattery();
''')

rep('''  addField(json, "dropid", (long)dropSeenID);
''', '''  addField(json, "dropid", (long)dropSeenID);

  addField(json, "bat", (long)batPercent);
  addField(json, "batv", batVolts, 2);
''')

# =====================================================
# DASHBOARD: battery chip
# =====================================================

rep('''.timeChip.armed b {
  color:#22d3ff;
}''', '''.timeChip.armed b {
  color:#22d3ff;
}

.batIcon {
  position:relative;
  display:inline-block;
  width:20px;
  height:10px;
  border:1.5px solid #60779b;
  border-radius:3px;
  align-self:center;
}

.batIcon::after {
  content:"";
  position:absolute;
  right:-4px;
  top:2px;
  width:2px;
  height:4px;
  border-radius:0 1px 1px 0;
  background:#60779b;
}

#batFill {
  position:absolute;
  left:1px;
  top:1px;
  bottom:1px;
  width:0;
  border-radius:1px;
  background:#22d3ff;
}

.timeChip.batOk b { color:#22d3ff; }
.timeChip.batMid b { color:#ffba52; }
.timeChip.batMid #batFill { background:#ffba52; }
.timeChip.batLow { border-color:#ff4d5e; animation:batBlink 1s step-end infinite; }
.timeChip.batLow b { color:#ff4d5e; }
.timeChip.batLow #batFill { background:#ff4d5e; }

@keyframes batBlink {
  50% { border-color:#15274a; }
}''')

rep('''<span class="timeChip">&#9201; ON <b id="upTime">0:00:00</b></span>''',
    '''<span class="timeChip" id="batChip" title="Battery"><i class="batIcon"><i id="batFill"></i></i><b id="batText">&ndash;</b></span>
<span class="timeChip">&#9201; ON <b id="upTime">0:00:00</b></span>''')

rep('''  el("upTime").innerText = clock(d.up);''', '''  el("upTime").innerText = clock(d.up);

  if(d.bat >= 0)
  {
    el("batText").innerText = d.bat + "% · " + d.batv.toFixed(2) + " V";
    el("batFill").style.width = (d.bat * 0.17) + "px";
    el("batChip").className = "timeChip " + (d.bat < 15 ? "batLow" : d.bat < 35 ? "batMid" : "batOk");
    el("batChip").title = "Battery " + d.bat + "% (" + d.batv.toFixed(2) + " V)";
  }
  else
  {
    el("batText").innerText = "no battery";
    el("batFill").style.width = "0";
    el("batChip").className = "timeChip";
    el("batChip").title = "No battery reading (USB only, or the divider on D1 is not wired)";
  }''')

# =====================================================
# DASHBOARD: 3D WAVES and 3D WIND
# =====================================================

rep('''<button class="soundButton" id="view3d" onclick="setView('3d')">3D NOISE</button>''',
    '''<button class="soundButton" id="view3d" onclick="setView('3d')">3D NOISE</button>
<button class="soundButton" id="view3dwave" onclick="setView('3dwave')">3D WAVES</button>
<button class="soundButton" id="view3dwind" onclick="setView('3dwind')">3D WIND</button>''')

rep('''  noiseSensor = d.sname || "";''', '''  noiseSensor = d.sname || "";
  cond3d = d;''')

rep('''function noiseLoop()
{
  if(view === "3d")
    drawNoise3D();''', '''// =====================================================
// 3D WAVES AND 3D WIND
// =====================================================

let cond3d = null;               // last /data
let seaAmp = 0.05;               // smoothed, view units
let seaChop = 0.01;
let seaPhase = 0;
let flagWind = 0;                // smoothed 0..1
let flagPhase = 0;
let lastSceneTime = 0;
let windStreaks = [];

function sceneSetup()
{
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;

  ctx.clearRect(0, 0, w, h);

  const bg = ctx.createRadialGradient(w / 2, h / 2, 10, w / 2, h / 2, Math.max(w, h) * 0.7);
  bg.addColorStop(0, "#081631");
  bg.addColorStop(1, "#01040b");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);

  if(!dragging && Date.now() - lastDrag > 4000)
    yaw += 0.002;

  const now = performance.now() / 1000;
  const dt = lastSceneTime ? Math.min(0.1, now - lastSceneTime) : 0.016;
  lastSceneTime = now;

  const cx = w / 2;
  const cy = h / 2 + 10;
  const size = Math.min(w, h) * 0.42;
  const cyw = Math.cos(yaw), syw = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);

  function proj(p)
  {
    const x = p[0] * cyw - p[1] * syw;
    const y = p[0] * syw + p[1] * cyw;
    const z = p[2];

    const y2 = y * cp - z * sp;
    const z2 = y * sp + z * cp;

    const k = 4 / (4 + y2);

    return [cx + x * size * k, cy - z2 * size * k, y2];
  }

  return { w, h, dt, proj };
}

// Painter's order: far quads first
function drawQuads(quads)
{
  quads.sort((a, b) => b.depth - a.depth);

  for(const q of quads)
  {
    ctx.beginPath();
    ctx.moveTo(q.p[0][0], q.p[0][1]);

    for(let i = 1; i < 4; i++)
      ctx.lineTo(q.p[i][0], q.p[i][1]);

    ctx.closePath();
    ctx.fillStyle = q.fill;
    ctx.fill();

    if(q.stroke)
    {
      ctx.strokeStyle = q.stroke;
      ctx.lineWidth = 0.6;
      ctx.stroke();
    }
  }
}

function sceneText(lines, footer, h)
{
  ctx.textAlign = "left";
  ctx.textBaseline = "top";

  let y = 10;

  for(const l of lines)
  {
    ctx.font = l[2] || "bold 13px Arial";
    ctx.fillStyle = l[1];
    ctx.fillText(l[0], 12, y);
    y += 18;
  }

  ctx.font = "11px Arial";
  ctx.fillStyle = "#7b90b2";
  ctx.textBaseline = "bottom";
  ctx.fillText(footer, 12, h - 8);
}

function mix(a, b, t)
{
  return Math.round(a + (b - a) * t);
}

// ---------- WAVES ----------

function drawWaves3D()
{
  const S = sceneSetup();
  const d = cond3d;

  const swing = d ? d.waveswing : 0;
  const period = d && d.period > 0 ? d.period : 6;
  const windLevel = d ? d.wind : 0;

  // Rod swing 0..4 deg -> wave height on the screen; wind adds chop
  const ampTarget = 0.03 + Math.min(swing, 4) / 4 * 0.32;
  const chopTarget = 0.008 + windLevel * 0.012;

  seaAmp += (ampTarget - seaAmp) * Math.min(1, S.dt * 1.5);
  seaChop += (chopTarget - seaChop) * Math.min(1, S.dt * 1.5);
  seaPhase += S.dt * Math.PI * 2 / period;

  const t = performance.now() / 1000;

  // Waves roll in along +x (toward the shore and the rod)
  function height(x, y)
  {
    return seaAmp * (
        0.62 * Math.sin(x * 3.2 - seaPhase)
      + 0.25 * Math.sin(x * 5.1 + y * 1.4 - seaPhase * 1.35 + 1.1)
      + 0.13 * Math.sin(x * 2.0 - y * 2.3 - seaPhase * 0.8 + 2.4))
      + seaChop * (
        Math.sin(x * 19 + y * 7 - t * 5.3)
      + Math.sin(x * 11 - y * 17 - t * 6.1 + 1.7)) * 0.5;
  }

  const N = 26;
  const pts = [];

  for(let i = 0; i <= N; i++)
  {
    pts.push([]);

    for(let j = 0; j <= N; j++)
    {
      const x = -1 + 2 * i / N;
      const y = -1 + 2 * j / N;
      const z = height(x, y);

      pts[i].push({ z, q: S.proj([x, y, z]) });
    }
  }

  const quads = [];
  const zMax = seaAmp + seaChop;

  for(let i = 0; i < N; i++)
  {
    for(let j = 0; j < N; j++)
    {
      const a = pts[i][j], b = pts[i + 1][j], c = pts[i + 1][j + 1], e = pts[i][j + 1];
      const z = (a.z + b.z + c.z + e.z) / 4;
      const slope = (b.z + c.z - a.z - e.z) / 2 * N;          // facing the light = brighter
      const tH = Math.max(0, Math.min(1, (z / zMax + 1) / 2));
      const lit = Math.max(0, Math.min(1, 0.55 + slope * 0.25));

      let r = mix(4, 20, tH), g = mix(30, 150, tH), bl = mix(80, 230, tH);

      r = mix(r * 0.55, r * 1.25, lit);
      g = mix(g * 0.55, g * 1.25, lit);
      bl = mix(bl * 0.55, Math.min(255, bl * 1.2), lit);

      // Foam on the top of big waves
      if(tH > 0.86 && seaAmp > 0.14)
      {
        const f = (tH - 0.86) / 0.14;
        r = mix(r, 220, f); g = mix(g, 240, f); bl = mix(bl, 255, f);
      }

      quads.push({
        p: [a.q, b.q, c.q, e.q],
        depth: (a.q[2] + b.q[2] + c.q[2] + e.q[2]) / 4,
        fill: "rgb(" + r + "," + g + "," + bl + ")",
        stroke: "rgba(34,211,255,.10)"
      });
    }
  }

  drawQuads(quads);

  // Float bobbing in the middle
  const fz = height(0, 0);
  const top = S.proj([0, 0, fz + 0.09]);
  const base = S.proj([0, 0, fz]);

  ctx.strokeStyle = "#eaf3ff";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(base[0], base[1]);
  ctx.lineTo(top[0], top[1]);
  ctx.stroke();

  ctx.fillStyle = "#ff8a3d";
  ctx.beginPath();
  ctx.arc(top[0], top[1], 5, 0, Math.PI * 2);
  ctx.fill();

  const lines = d ?
  [
    ["WAVES  " + WAVE_NAMES[d.wave], "#22d3ff"],
    ["rod swings ±" + d.waveswing.toFixed(1) + "°" + (d.period > 0 ? " · a wave every " + d.period.toFixed(0) + " s" : " · wave time not known yet"), "#c3d2ea", "12px Arial"],
    ["small ripples = wind: " + WIND_NAMES[d.wind], "#7b90b2", "11px Arial"]
  ] :
  [
    ["WAVES", "#22d3ff"],
    ["waiting for data…", "#7b90b2", "12px Arial"]
  ];

  sceneText(lines, "Height = rod swing · speed = wave time · ripples = wind · drag to turn", S.h);
}

// ---------- WIND (flag) ----------

function drawWind3D()
{
  const S = sceneSetup();
  const d = cond3d;

  // Same 0..1 strength as the WIND bar on the left
  let target = 0;

  if(d)
  {
    target = d.dir
      ? Math.max(d.windswing / 4.0, d.windvib / 0.03)
      : d.windvib / 0.03;

    target = Math.max(0, Math.min(1, target));
  }

  flagWind += (target - flagWind) * Math.min(1, S.dt * 1.2);

  const t = performance.now() / 1000;

  // Gusts: the strength wobbles a little
  const gust = flagWind * (1 + 0.25 * Math.sin(t * 0.7) * Math.sin(t * 1.9 + 1));
  const s = Math.max(0, Math.min(1.2, gust));

  flagPhase += S.dt * (2.5 + 11 * s);

  const ground = -0.85;
  const poleTop = 0.95;
  const poleX = -0.45;

  // Ground rings
  ctx.strokeStyle = "rgba(255,255,255,.08)";
  ctx.lineWidth = 1;

  for(const r of [0.5, 1.0])
  {
    ctx.beginPath();

    for(let i = 0; i <= 48; i++)
    {
      const a = i / 48 * Math.PI * 2;
      const q = S.proj([r * Math.cos(a), r * Math.sin(a), ground]);

      if(i === 0) ctx.moveTo(q[0], q[1]);
      else ctx.lineTo(q[0], q[1]);
    }

    ctx.stroke();
  }

  // Wind streaks (more and faster with more wind)
  const want = Math.round(4 + 36 * Math.min(1, s));

  while(windStreaks.length < want)
    windStreaks.push({ x: -1.3 + Math.random() * 2.6, y: -1 + Math.random() * 2, z: ground + 0.15 + Math.random() * 1.7, l: 0.1 + Math.random() * 0.25 });

  windStreaks.length = want;

  ctx.lineWidth = 1.2;

  for(const p of windStreaks)
  {
    p.x += S.dt * (0.15 + 2.2 * s);

    if(p.x > 1.3)
    {
      p.x = -1.3;
      p.y = -1 + Math.random() * 2;
      p.z = ground + 0.15 + Math.random() * 1.7;
    }

    const q0 = S.proj([p.x, p.y, p.z]);
    const q1 = S.proj([p.x - p.l * (0.4 + s), p.y, p.z]);

    ctx.strokeStyle = "rgba(160,200,255," + (0.08 + 0.25 * Math.min(1, s)) + ")";
    ctx.beginPath();
    ctx.moveTo(q0[0], q0[1]);
    ctx.lineTo(q1[0], q1[1]);
    ctx.stroke();
  }

  // Flag: hangs down when calm, flies out flat in a strong wind
  const L = 1.05, H = 0.6;
  const angle = (0.12 + 0.88 * Math.sqrt(Math.min(1, s))) * Math.PI / 2;
  const dx = Math.sin(angle), dz = -Math.cos(angle);
  const flap = 0.03 + 0.2 * Math.min(1, s);
  const NU = 18, NV = 8;
  const pts = [];

  for(let i = 0; i <= NU; i++)
  {
    pts.push([]);

    for(let j = 0; j <= NV; j++)
    {
      const u = i / NU, v = j / NV;

      // Sideways ripple runs from the pole to the free end
      const side = flap * Math.pow(u, 1.2) * Math.sin(u * 7 - flagPhase + v * 0.7)
                 + 0.03 * u * Math.sin(u * 15 - flagPhase * 1.7) * Math.min(1, s);

      const p = [poleX + u * L * dx, side, poleTop - v * H + u * L * dz];

      pts[i].push({ side, q: S.proj(p) });
    }
  }

  const quads = [];

  for(let i = 0; i < NU; i++)
  {
    for(let j = 0; j < NV; j++)
    {
      const a = pts[i][j], b = pts[i + 1][j], c = pts[i + 1][j + 1], e = pts[i][j + 1];
      const slope = (b.side + c.side - a.side - e.side) / 2 * NU;
      const lit = Math.max(0, Math.min(1, 0.6 + slope * 0.9));
      const u = (i + 0.5) / NU;

      // Brand colours along the flag: electric blue -> cyan
      const r = mix(mix(30, 34, u) * 0.45, mix(30, 34, u) * 1.3, lit);
      const g = mix(mix(123, 211, u) * 0.45, Math.min(255, mix(123, 211, u) * 1.2), lit);
      const bl = mix(255 * 0.45, 255, lit);

      quads.push({
        p: [a.q, b.q, c.q, e.q],
        depth: (a.q[2] + b.q[2] + c.q[2] + e.q[2]) / 4,
        fill: "rgb(" + r + "," + g + "," + bl + ")",
        stroke: "rgba(0,10,30,.25)"
      });
    }
  }

  // Pole, then the flag in front of it
  const pb = S.proj([poleX, 0, ground]), pt = S.proj([poleX, 0, poleTop + 0.04]);

  ctx.strokeStyle = "#8399bb";
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.moveTo(pb[0], pb[1]);
  ctx.lineTo(pt[0], pt[1]);
  ctx.stroke();

  drawQuads(quads);

  ctx.fillStyle = "#c3d2ea";
  ctx.beginPath();
  ctx.arc(pt[0], pt[1], 4, 0, Math.PI * 2);
  ctx.fill();

  const lines = d ?
  [
    ["WIND  " + WIND_NAMES[d.wind], "#22d3ff"],
    [(d.dir ? "rod sways ±" + d.windswing.toFixed(1) + "° · " : "") + "shake " + d.windvib.toFixed(3) + " g · strength " + Math.round(flagWind * 100) + "%", "#c3d2ea", "12px Arial"],
    ["strength only: the rod cannot tell the wind direction", "#7b90b2", "11px Arial"]
  ] :
  [
    ["WIND", "#22d3ff"],
    ["waiting for data…", "#7b90b2", "12px Arial"]
  ];

  sceneText(lines, "Calm = flag hangs · strong = flies and flaps · drag to turn", S.h);
}

function draw3DView()
{
  if(view === "3dwave")
    drawWaves3D();
  else if(view === "3dwind")
    drawWind3D();
  else
    drawNoise3D();
}

function noiseLoop()
{
  if(view.startsWith("3d"))
    draw3DView();''')

rep('''  if(view !== "3d")
    return;''', '''  if(!view.startsWith("3d"))
    return;''')

rep('''  if(view === "3d")
  {
    drawNoise3D();
    return;
  }''', '''  if(view.startsWith("3d"))
  {
    draw3DView();
    return;
  }''')

rep('''  el("view3d").classList.toggle("enabled", v === "3d");
''', '''  el("view3d").classList.toggle("enabled", v === "3d");
  el("view3dwave").classList.toggle("enabled", v === "3dwave");
  el("view3dwind").classList.toggle("enabled", v === "3dwind");
''')

rep('''  canvas.style.touchAction = v === "3d" ? "none" : "auto";''',
    '''  canvas.style.touchAction = v.startsWith("3d") ? "none" : "auto";''')

rep('''  else if(v === "3d")
  {''', '''  else if(v === "3dwave")
  {
    el("graphTitle").innerText = "3D WAVES";
    el("graphSub").innerText = "The sea as the rod feels it · height = wave swing, speed = wave time, ripples = wind · drag to turn";
  }
  else if(v === "3dwind")
  {
    el("graphTitle").innerText = "3D WIND";
    el("graphSub").innerText = "Flag = wind strength on the rod · hangs when calm, flies and flaps when strong · drag to turn";
  }
  else if(v === "3d")
  {''')

open(path, 'w').write(s)
print("ok", path)

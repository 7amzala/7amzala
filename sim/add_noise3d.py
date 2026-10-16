#!/usr/bin/env python3
"""Adds the live "3D NOISE" view to a StrikeSense sketch's dashboard.

    python3 add_noise3d.py <sketch.ino>        (edits the file in place)

The rod sends the last vibration vectors (100 per second) with every
/data answer; the dashboard draws them as a 3D cloud with the knock
limit as a sphere. Plain canvas, no library (works offline).
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new):
    global s
    n = s.count(old)
    if n != 1:
        sys.exit(f"{path}: expected 1 x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# ---------- firmware: keep the last vibration vectors ----------
rep('''void updateSignals()
{''', '''// ---------- 3D noise view ----------
//
// The last vibration vectors (100 per second), sent to the dashboard
// with every /data answer. Units: 0.1 mg (0.0001 g).

const int VIB_RING = 64;

int16_t vibRing[VIB_RING][3];
unsigned long vibWritten = 0;
unsigned long vibSent = 0;

int16_t toTenthMg(float g)
{
  float v = g * 10000.0;

  if(v > 32000) v = 32000;
  if(v < -32000) v = -32000;

  return (int16_t)lround(v);
}

void storeVibration(Vec3 v)
{
  int i = vibWritten % VIB_RING;

  vibRing[i][0] = toTenthMg(v.x);
  vibRing[i][1] = toTenthMg(v.y);
  vibRing[i][2] = toTenthMg(v.z);

  vibWritten++;
}

void addVibrationSamples(String& json)
{
  unsigned long from = vibSent;

  if(vibWritten - from > VIB_RING)
    from = vibWritten - VIB_RING;

  json += ",\\"vib\\":[";

  for(unsigned long n = from; n < vibWritten; n++)
  {
    int i = n % VIB_RING;

    if(n != from) json += ",";

    json += vibRing[i][0];
    json += ",";
    json += vibRing[i][1];
    json += ",";
    json += vibRing[i][2];
  }

  json += "],\\"g\\":[";
  json += String(gravityUnit.x, 3);
  json += ",";
  json += String(gravityUnit.y, 3);
  json += ",";
  json += String(gravityUnit.z, 3);
  json += "]";

  vibSent = vibWritten;
}

void updateSignals()
{''')

rep('''  Vec3 vibration =
    vSub(rawAccel, slowPart);
''', '''  Vec3 vibration =
    vSub(rawAccel, slowPart);

  storeVibration(vibration);
''')

rep('''  addField(json, "peaktilt", strikePeakTilt, 2);
''', '''  addField(json, "peaktilt", strikePeakTilt, 2);

  addVibrationSamples(json);
''')

# ---------- dashboard: button ----------
rep('''<button class="soundButton" id="viewStrike" onclick="setView('strike')" disabled>LAST STRIKE</button>''',
    '''<button class="soundButton" id="viewStrike" onclick="setView('strike')" disabled>LAST STRIKE</button>
<button class="soundButton" id="view3d" onclick="setView('3d')">3D NOISE</button>''')

# ---------- dashboard: view switching ----------
rep('''  el("viewStrike").classList.toggle("enabled", v === "strike");
''', '''  el("viewStrike").classList.toggle("enabled", v === "strike");
  el("view3d").classList.toggle("enabled", v === "3d");

  canvas.style.touchAction = v === "3d" ? "none" : "auto";
''')

rep('''  else
  {
    el("graphTitle").innerText = "LIVE ROD ACTIVITY";''', '''  else if(v === "3d")
  {
    el("graphTitle").innerText = "3D NOISE";
    el("graphSub").innerText = "Each dot = how the tip shook (100 per second, last 3 s) · orange = knock limit · drag to turn";
  }
  else
  {
    el("graphTitle").innerText = "LIVE ROD ACTIVITY";''')

rep('''function redraw()
{
  if(view === "strike" && replay)''', '''// =====================================================
// 3D NOISE VIEW
// =====================================================

const NOISE_POINTS = 300;        // 3 s at 100 per second

let noiseCloud = [];             // [x, y, z] in g
let noiseDown = [0, 0, -1];      // gravity direction, sensor frame
let noiseThr = 0.01;
let noiseSensor = "";
let noiseRange = 0.02;           // g at the edge of the view (auto)
let yaw = 0.7;
let pitch = 0.45;
let dragging = false;
let dragX = 0;
let dragY = 0;
let lastDrag = 0;

function addNoisePoints(d)
{
  if(!d.vib)
    return;

  for(let i = 0; i + 2 < d.vib.length; i += 3)
    noiseCloud.push([d.vib[i] / 10000, d.vib[i + 1] / 10000, d.vib[i + 2] / 10000]);

  if(noiseCloud.length > NOISE_POINTS)
    noiseCloud.splice(0, noiseCloud.length - NOISE_POINTS);

  if(d.g)
    noiseDown = [-d.g[0], -d.g[1], -d.g[2]];

  noiseThr = d.knockthr;
  noiseSensor = d.sname || "";
}

function noiseRms()
{
  if(noiseCloud.length === 0)
    return 0;

  let sum = 0;

  for(const p of noiseCloud)
    sum += p[0] * p[0] + p[1] * p[1] + p[2] * p[2];

  return Math.sqrt(sum / noiseCloud.length);
}

function mg(g)
{
  const v = g * 1000;
  return (v < 10 ? v.toFixed(1) : v.toFixed(0)) + " mg";
}

function drawNoise3D()
{
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;

  ctx.clearRect(0, 0, w, h);

  const bg = ctx.createRadialGradient(w / 2, h / 2, 10, w / 2, h / 2, Math.max(w, h) * 0.7);
  bg.addColorStop(0, "#0d1a13");
  bg.addColorStop(1, "#040806");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);

  const rms = noiseRms();

  // Zoom follows the noise and the knock limit, smoothly
  const target = Math.max(noiseThr * 1.3, rms * 4, 0.001);
  noiseRange += (target - noiseRange) * 0.06;

  if(!dragging && Date.now() - lastDrag > 4000)
    yaw += 0.004;

  const cx = w / 2;
  const cy = h / 2 + 10;
  const size = Math.min(w, h) * 0.38;
  const cyw = Math.cos(yaw), syw = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);

  // p in "view units" (1 = edge of the view)
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

  function lineP(a, b, colour, width, dash)
  {
    const pa = proj(a), pb = proj(b);

    ctx.beginPath();
    ctx.moveTo(pa[0], pa[1]);
    ctx.lineTo(pb[0], pb[1]);
    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  function ring(r, plane, colour, width, dash)
  {
    ctx.beginPath();

    for(let i = 0; i <= 48; i++)
    {
      const t = i / 48 * Math.PI * 2;
      const a = r * Math.cos(t), b = r * Math.sin(t);
      const p = plane === 0 ? [a, b, 0] : plane === 1 ? [a, 0, b] : [0, a, b];
      const q = proj(p);

      if(i === 0) ctx.moveTo(q[0], q[1]);
      else ctx.lineTo(q[0], q[1]);
    }

    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  // Floor rings (scale), axes
  ring(0.5, 0, "rgba(255,255,255,.06)", 1);
  ring(1.0, 0, "rgba(255,255,255,.09)", 1);

  const axes = [[[1, 0, 0], "#ff6b6b", "X"], [[0, 1, 0], "#4cf294", "Y"], [[0, 0, 1], "#59a8ff", "Z"]];

  ctx.font = "bold 11px Arial";

  for(const a of axes)
  {
    lineP([0, 0, 0], a[0], a[1], 1.2);
    lineP([0, 0, 0], a[0].map(v => -v), "rgba(255,255,255,.08)", 1);

    const q = proj(a[0].map(v => v * 1.08));
    ctx.fillStyle = a[1];
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(a[2], q[0], q[1]);
  }

  // Which way is down
  const dn = noiseDown.map(v => v * 0.85);
  lineP([0, 0, 0], dn, "rgba(200,210,205,.35)", 2, [3, 4]);
  const qd = proj(dn);
  ctx.fillStyle = "rgba(200,210,205,.6)";
  ctx.fillText("down", qd[0], qd[1] + 10);

  // Knock limit sphere
  const rt = noiseThr / noiseRange;

  for(let pl = 0; pl < 3; pl++)
    ring(rt, pl, "rgba(255,186,82,.55)", 1.3, [5, 4]);

  // Points: older = fainter; outside the knock limit = orange
  const n = noiseCloud.length;

  for(let i = 0; i < n; i++)
  {
    const p = noiseCloud[i];
    const len = Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const q = proj(p.map(v => v / noiseRange));
    const age = i / Math.max(1, n - 1);
    const out = len > noiseThr;

    ctx.globalAlpha = 0.15 + 0.85 * age;
    ctx.fillStyle = out ? "#ffba52" : "#49ef92";
    ctx.beginPath();
    ctx.arc(q[0], q[1], i === n - 1 ? 3.5 : 2.2, 0, Math.PI * 2);
    ctx.fill();
  }

  ctx.globalAlpha = 1;

  // Numbers
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.font = "bold 13px Arial";
  ctx.fillStyle = "#49ef92";
  ctx.fillText("NOISE  " + mg(rms) + " rms", 12, 10);
  ctx.fillStyle = "#ffba52";
  ctx.fillText("KNOCK LIMIT  " + mg(noiseThr), 12, 28);
  ctx.fillStyle = "#7d9186";
  ctx.font = "11px Arial";
  ctx.fillText("limit is " + (rms > 0 ? (noiseThr / rms).toFixed(1) : "-") + "x the noise" + (noiseSensor ? " · " + noiseSensor : ""), 12, 46);
  ctx.textBaseline = "bottom";
  ctx.fillText("axis length = " + mg(noiseRange) + " · zoom follows the noise", 12, h - 8);
}

function noiseLoop()
{
  if(view === "3d")
    drawNoise3D();

  requestAnimationFrame(noiseLoop);
}

canvas.addEventListener("pointerdown", e =>
{
  if(view !== "3d")
    return;

  dragging = true;
  dragX = e.clientX;
  dragY = e.clientY;
  canvas.setPointerCapture(e.pointerId);
});

canvas.addEventListener("pointermove", e =>
{
  if(!dragging)
    return;

  yaw += (e.clientX - dragX) * 0.01;
  pitch = Math.max(-1.4, Math.min(1.4, pitch + (e.clientY - dragY) * 0.01));
  dragX = e.clientX;
  dragY = e.clientY;
  lastDrag = Date.now();
});

canvas.addEventListener("pointerup", () =>
{
  dragging = false;
  lastDrag = Date.now();
});

function redraw()
{
  if(view === "3d")
  {
    drawNoise3D();
    return;
  }

  if(view === "strike" && replay)''')

rep('''    liveEnergy.push(d.energy);''', '''    addNoisePoints(d);

    liveEnergy.push(d.energy);''')

rep('''resizeCanvas();
pollLoop();''', '''resizeCanvas();
pollLoop();
noiseLoop();''')

open(path, 'w').write(s)
print("ok", path)

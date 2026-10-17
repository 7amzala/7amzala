#!/usr/bin/env python3
"""LIVE ROD view: next to LIVE (which stays as it was), a picture of the
rod in a sand spike on the beach, moving with the real readings.

- Bend follows the tilt (+ toward water), drawn bigger than real so it
  can be seen (a real tip moves only 1-3 degrees on a bite).
- Knock: the tip shakes. Slow pull: bends toward the sea, line tight,
  orange. Drop-back: springs back, line slack. Strike: red and flashing.
- The sea follows the measured wave size and wave time.

    python3 add_rodview.py <sketch.ino>      (edits in place)
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


rep('''<button class="soundButton enabled" id="viewLive" onclick="setView('live')">LIVE</button>''',
    '''<button class="soundButton enabled" id="viewLive" onclick="setView('live')">LIVE</button>
<button class="soundButton" id="viewRod" onclick="setView('rod')">LIVE ROD</button>''')

rep('''  el("viewLive").classList.toggle("enabled", v === "live");''',
    '''  el("viewLive").classList.toggle("enabled", v === "live");
  el("viewRod").classList.toggle("enabled", v === "rod");''')

rep('''  else if(v === "3dwave")
  {''', '''  else if(v === "rod")
  {
    el("graphTitle").innerText = "LIVE ROD";
    el("graphSub").innerText = "Your rod on the beach, moving with the real readings · bend drawn bigger than real so you can see it";
  }
  else if(v === "3dwave")
  {''')

rep('''  cond3d = d;''', '''  cond3d = d;
  rodLiveData(d);''')

rep('''function noiseLoop()
{
  if(view.startsWith("3d"))
    draw3DView();''', '''function noiseLoop()
{
  // The canvas box can change size without a window resize (buttons
  // wrapping to two lines): keep the drawing size the same as the box
  if(view.startsWith("3d") || view === "rod")
  {
    const box = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;

    if(canvas.width !== Math.floor(box.width * dpr) || canvas.height !== Math.floor(box.height * dpr))
      resizeCanvas();
  }

  if(view.startsWith("3d"))
    draw3DView();
  else if(view === "rod")
    drawRodLive();''')

rep('''  if(view.startsWith("3d"))
  {
    draw3DView();
    return;
  }''', '''  if(view.startsWith("3d"))
  {
    draw3DView();
    return;
  }

  if(view === "rod")
  {
    drawRodLive();
    return;
  }''')

rep('''function draw3DView()
{''', r'''// =====================================================
// LIVE ROD VIEW
// =====================================================

let rodD = null;                 // last /data
let rodTilt = 0;                 // smoothed tilt shown
let rodKnock = 0;                // 1 = knock now, fades out
let rodKnockID = -1;
let rodLastFrame = 0;

function rodLiveData(d)
{
  if(rodKnockID >= 0 && d.knockid !== rodKnockID)
    rodKnock = 1;

  rodKnockID = d.knockid;

  if(d.energy > d.knockthr * 1.5)
    rodKnock = Math.max(rodKnock, Math.min(1, d.energy / d.knockthr / 4));

  rodD = d;
}

function drawRodLive()
{
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  const now = performance.now() / 1000;
  const dt = rodLastFrame ? Math.min(0.1, now - rodLastFrame) : 0.016;
  rodLastFrame = now;

  const d = rodD;
  let state = "armed";
  let label = el("mainStatus").innerText;

  if(d)
  {
    rodTilt += (d.tilt - rodTilt) * Math.min(1, dt * 8);

    if(d.state === "monitoring")
      label = "ROD ARMED";

    if(d.strike)
    {
      state = "strike";
      label = "STRIKE!  " + d.type;
    }
    else if(d.dropwatch)
    {
      state = "drop";
      label = "DROP-BACK – watching for a pull…";
    }
    else if(d.bucket >= 30)
    {
      state = "pull";
      label = "SLOW PULL " + Math.min(100, d.bucket) + "%";
    }
    else if(d.drop >= 30)
    {
      state = "drop";
      label = "DROP-BACK " + Math.min(100, d.drop) + "%";
    }
    else if(rodKnock > 0.3)
    {
      label = "KNOCK";
    }
  }

  drawRodScene(ctx, w, h,
  {
    t: now,
    tilt: rodTilt,
    knock: rodKnock,
    state: state,
    label: label,
    dir: d ? d.dir : true,
    waveswing: d ? d.waveswing : 0.5,
    period: d && d.period > 0 ? d.period : 6
  });

  rodKnock = Math.max(0, rodKnock - dt * 2);
}

// Surf rod in a sand spike, on the beach at night.
// s.tilt: + toward the water. s.state: armed / pull / drop / strike.
function drawRodScene(ctx, w, h, s)
{
  const t = s.t;
  const swing = s.waveswing;
  const horizon = h * 0.50;

  ctx.clearRect(0, 0, w, h);

  // ---------- sky ----------
  const sky = ctx.createLinearGradient(0, 0, 0, horizon);
  sky.addColorStop(0, "#030814");
  sky.addColorStop(0.65, "#0b1f45");
  sky.addColorStop(1, "#1d4f86");
  ctx.fillStyle = sky;
  ctx.fillRect(0, 0, w, horizon + 1);

  ctx.fillStyle = "#eaf3ff";

  for(let i = 0; i < 40; i++)
  {
    ctx.globalAlpha = 0.25 + 0.25 * (1 + Math.sin(t * 1.3 + i));
    ctx.fillRect((i * 97.3 % 1) * w, ((i * 57.7) % 1) * horizon * 0.7, 1.3, 1.3);
  }

  ctx.globalAlpha = 1;

  // moon
  const mx = w * 0.78, my = h * 0.20, mr = Math.min(w, h) * 0.035;
  let g = ctx.createRadialGradient(mx, my, 0, mx, my, mr * 6);
  g.addColorStop(0, "rgba(180,220,255,.35)");
  g.addColorStop(1, "rgba(180,220,255,0)");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(mx, my, mr * 6, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = "#e8f1ff";
  ctx.beginPath();
  ctx.arc(mx, my, mr, 0, Math.PI * 2);
  ctx.fill();

  // ---------- sea ----------
  const sea = ctx.createLinearGradient(0, horizon, 0, h);
  sea.addColorStop(0, "#1a5a9a");
  sea.addColorStop(0.5, "#0c3d78");
  sea.addColorStop(1, "#072a58");
  ctx.fillStyle = sea;
  ctx.fillRect(0, horizon, w, h - horizon);

  // moonlight on the water
  for(let i = 0; i < 14; i++)
  {
    const ww = (6 + i * 3) * (0.7 + 0.3 * Math.sin(t * 2 + i));
    ctx.fillStyle = "rgba(200,230,255," + (0.35 - i * 0.02) + ")";
    ctx.fillRect(mx - ww / 2 + Math.sin(t + i) * 3, horizon + 3 + i * i * h * 0.0018 + i * 2, ww, 1.5);
  }

  // swells rolling in: bigger and faster with the measured waves
  const period = s.period;

  for(let k = 0; k < 7; k++)
  {
    const f = ((k + t / period) % 7) / 7;            // 0 at the horizon, 1 at the shore
    const y = horizon + Math.pow(f, 1.8) * (h - horizon) * 0.95;
    const amp = (0.6 + swing * 1.2) * (0.3 + f * 1.4);

    ctx.strokeStyle = "rgba(127,227,255," + (0.12 + f * 0.35) + ")";
    ctx.lineWidth = 0.8 + f * 1.4;
    ctx.beginPath();

    for(let x = 0; x <= w; x += 6)
    {
      const yy = y + Math.sin(x * (0.03 - f * 0.015) + k * 2 + t * 0.6) * amp;

      if(x === 0) ctx.moveTo(x, yy);
      else ctx.lineTo(x, yy);
    }

    ctx.stroke();
  }

  // ---------- beach with surf ----------
  const surge = Math.sin(t * 2 * Math.PI / period) * (3 + swing * 4);
  const shoreEnd = w * 0.95;
  const shore = x => h * 0.70 + (x / w) * h * 0.26 - Math.sin(x * 0.02 + 1) * 3;

  ctx.beginPath();
  ctx.moveTo(0, h);
  for(let x = 0; x <= shoreEnd; x += 5) ctx.lineTo(x, shore(x) - surge - 6);
  ctx.lineTo(shoreEnd + 40, h);
  ctx.closePath();
  ctx.fillStyle = "#6b5a43";
  ctx.fill();

  ctx.strokeStyle = "rgba(240,248,255,.85)";
  ctx.lineWidth = 2.5;
  ctx.beginPath();

  for(let x = 0; x <= shoreEnd; x += 5)
  {
    const yy = shore(x) - surge - 6 + Math.sin(x * 0.15 + t * 3) * 1.2;

    if(x === 0) ctx.moveTo(x, yy);
    else ctx.lineTo(x, yy);
  }

  ctx.stroke();

  const sandTop = x => shore(x) + 2;

  ctx.beginPath();
  ctx.moveTo(0, h);
  for(let x = 0; x <= shoreEnd; x += 5) ctx.lineTo(x, sandTop(x));
  ctx.lineTo(shoreEnd + 40, h);
  ctx.closePath();
  const sand = ctx.createLinearGradient(0, h * 0.65, 0, h);
  sand.addColorStop(0, "#c9a878");
  sand.addColorStop(1, "#8a6c45");
  ctx.fillStyle = sand;
  ctx.fill();

  ctx.fillStyle = "rgba(60,40,20,.25)";

  for(let i = 0; i < 120; i++)
  {
    const gx = (i * 61.7 % 1) * shoreEnd;
    ctx.fillRect(gx, sandTop(gx) + ((i * 37.3) % 1) * (h - sandTop(gx)), 1.2, 1.2);
  }

  // ---------- sand spike ----------
  const lean = 12 * Math.PI / 180;                 // leans toward the sea
  const baseX = w * 0.13, baseY = sandTop(w * 0.13) + h * 0.10;
  const spikeLen = h * 0.30;
  const ux = Math.sin(lean), uy = -Math.cos(lean);  // up the spike
  const nx = Math.cos(lean), ny = Math.sin(lean);   // toward the sea
  const spikeTop = [baseX + ux * spikeLen, baseY + uy * spikeLen];

  ctx.fillStyle = "rgba(40,25,10,.35)";
  ctx.beginPath();
  ctx.ellipse(baseX + 14, baseY + 2, 22, 4, 0, 0, Math.PI * 2);
  ctx.fill();

  ctx.lineCap = "butt";
  ctx.strokeStyle = "#1f7bff";
  ctx.lineWidth = 5;
  ctx.beginPath();
  ctx.moveTo(baseX, baseY);
  ctx.lineTo(spikeTop[0], spikeTop[1]);
  ctx.stroke();
  ctx.strokeStyle = "rgba(255,255,255,.35)";
  ctx.lineWidth = 1.2;
  ctx.beginPath();
  ctx.moveTo(baseX - 1.5, baseY);
  ctx.lineTo(spikeTop[0] - 1.5, spikeTop[1]);
  ctx.stroke();

  // ---------- rod: butt in the lower cup, blank through the top cup ----------
  const buttCup = [baseX + ux * spikeLen * 0.22, baseY + uy * spikeLen * 0.22];
  const butt = [buttCup[0] + nx * 7, buttCup[1] + ny * 7];
  const L = h * 0.60;
  const a0 = -(Math.PI / 2 - lean);
  const B0 = 0.30;                                  // normal bend from the lead

  // Tilt -> bend, bigger than real so it can be seen
  let B = B0 + 0.6 * Math.tanh(s.tilt / 4);

  if(s.knock > 0)
    B += s.knock * 0.05 * Math.sin(t * 2 * Math.PI * 7);

  B = Math.max(0, Math.min(1.5, B));

  // Straight up to the top cup, bends above it
  const cupU = (spikeLen * 0.78) / L;
  const N = 48;

  function rodPoints(bend)
  {
    const pts = [];
    let x = butt[0], y = butt[1];

    for(let i = 0; i <= N; i++)
    {
      const u = i / N;
      const v = Math.max(0, (u - cupU) / (1 - cupU));
      const ang = a0 + bend * v * (2 - v);

      pts.push([x, y]);
      x += Math.cos(ang) * L / N;
      y += Math.sin(ang) * L / N;
    }

    return pts;
  }

  const pts = rodPoints(B);
  const tip = pts[N];

  // normal position (dashed)
  const rest = rodPoints(B0);
  ctx.setLineDash([4, 5]);
  ctx.strokeStyle = "rgba(234,243,255,.28)";
  ctx.lineWidth = 1.3;
  ctx.beginPath();
  rest.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
  ctx.stroke();
  ctx.setLineDash([]);

  const colour = s.state === "strike" ? "#ff4d5e" : s.state === "pull" || s.state === "drop" ? "#ffba52" : "#22d3ff";
  const flash = s.state === "strike" ? 0.55 + 0.45 * Math.sin(t * 14) : 1;

  // ---------- line: tight on a pull, slack on a drop-back ----------
  const endX = w * 0.97, endY = horizon + (h - horizon) * 0.12;
  const tension = Math.max(0, Math.min(1, (B - B0) / 0.25 + 0.55));
  const midX = (tip[0] + endX) / 2, midY = (tip[1] + endY) / 2;
  const cpX = midX + (tip[0] + (endX - tip[0]) * 0.2 - midX) * (1 - tension);
  const cpY = midY + (endY + h * 0.12 - midY) * (1 - tension);

  ctx.strokeStyle = s.state === "strike" ? "rgba(255,77,94," + flash + ")" : "rgba(234,243,255,.6)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(tip[0], tip[1]);
  ctx.quadraticCurveTo(cpX, cpY, endX, endY);
  ctx.stroke();

  // ---------- rod blank ----------
  ctx.lineCap = "round";

  for(let i = 0; i < N; i++)
  {
    const u = i / N;

    ctx.strokeStyle = u > 0.82 ? colour : u < 0.18 ? "#2a3550" : "#c3d2ea";
    ctx.globalAlpha = u > 0.82 ? flash : 1;
    ctx.lineWidth = 7.5 - 6.2 * u;
    ctx.beginPath();
    ctx.moveTo(pts[i][0], pts[i][1]);
    ctx.lineTo(pts[i + 1][0], pts[i + 1][1]);
    ctx.stroke();
  }

  ctx.globalAlpha = 1;

  // guides
  ctx.strokeStyle = "#8399bb";
  ctx.lineWidth = 1;

  for(const u of [0.36, 0.52, 0.65, 0.76, 0.85, 0.93])
  {
    const p = pts[Math.round(u * N)];
    ctx.beginPath();
    ctx.arc(p[0] + 4, p[1] + 1, 3 - 2 * u, 0, Math.PI * 2);
    ctx.stroke();
  }

  // reel
  const rp = pts[Math.round(0.24 * N)];
  ctx.fillStyle = "#3b4c6e";
  ctx.beginPath();
  ctx.ellipse(rp[0] + 9, rp[1] + 2, 6, 9, lean, 0, Math.PI * 2);
  ctx.fill();
  ctx.strokeStyle = "#c3d2ea";
  ctx.stroke();

  // cups
  ctx.fillStyle = "#11151d";
  ctx.save();
  ctx.translate(spikeTop[0] + nx * 4, spikeTop[1] + ny * 4);
  ctx.rotate(lean);
  ctx.fillRect(-9, -4, 18, 8);
  ctx.restore();
  ctx.save();
  ctx.translate(buttCup[0] + nx * 4, buttCup[1] + ny * 4);
  ctx.rotate(lean);
  ctx.fillRect(-8, -6, 16, 12);
  ctx.fillStyle = "#222";
  ctx.beginPath();
  ctx.arc(12, 0, 3, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();

  // XIAO box on the spike, cable up to the sensor
  const box = [baseX + ux * spikeLen * 0.08 - 2, baseY + uy * spikeLen * 0.08];
  const sp = pts[Math.round(0.88 * N)];

  ctx.strokeStyle = "rgba(34,211,255,.45)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(box[0] + 6, box[1] - 4);
  for(let i = Math.round(0.05 * N); i <= Math.round(0.88 * N); i++) ctx.lineTo(pts[i][0] - 2, pts[i][1] + 1);
  ctx.stroke();

  ctx.fillStyle = "#0f1d38";
  ctx.strokeStyle = "#22d3ff";
  ctx.lineWidth = 1.2;
  ctx.fillRect(box[0] - 8, box[1] - 7, 16, 13);
  ctx.strokeRect(box[0] - 8, box[1] - 7, 16, 13);
  ctx.fillStyle = "#22d3ff";
  ctx.fillRect(sp[0] - 3, sp[1] - 3, 6, 6);

  // tip glow
  g = ctx.createRadialGradient(tip[0], tip[1], 0, tip[0], tip[1], 20);
  g.addColorStop(0, colour);
  g.addColorStop(1, "rgba(0,0,0,0)");
  ctx.globalAlpha = 0.5 * flash;
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(tip[0], tip[1], 20, 0, Math.PI * 2);
  ctx.fill();
  ctx.globalAlpha = 1;

  // knock ripples
  if(s.knock > 0.2)
  {
    ctx.strokeStyle = "rgba(34,211,255," + (0.7 * s.knock) + ")";
    ctx.lineWidth = 1.2;

    for(const k of [9, 15, 21])
    {
      ctx.beginPath();
      ctx.arc(tip[0], tip[1], k, -2.6, -0.5);
      ctx.stroke();
    }
  }

  // text
  ctx.font = "bold 14px Arial";
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.fillStyle = colour;
  ctx.fillText(s.label, 12, 10);
  ctx.font = "11px Arial";
  ctx.fillStyle = "#8ea6c8";
  ctx.fillText("tip " + (s.tilt >= 0 ? "+" : "") + s.tilt.toFixed(1) + "°" + (s.dir ? " (+ = toward water)" : " (direction not learned yet)"), 12, 30);
}

function draw3DView()
{''')

open(path, 'w').write(s)
print("ok", path)

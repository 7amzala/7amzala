#!/usr/bin/env python3
"""Pull test pop-up and confirmation.

When the rod asks for the pull test, a pop-up shows an animation of how to
do it (hold the line in front of the tip, pull gently toward the sea, let
go), three short steps and a live progress bar. When the direction is
learned, a confirmation with an animated tick appears (or an explanation
if the pull test was skipped). Run after add_reelcast.py.

    python3 add_pulltest.py <sketch.ino>      (edits in place)
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


# ---------- CSS ----------
rep('''/* START-UP SPLASH */''', '''/* PULL TEST POP-UPS */

.ptModal {
  position:fixed;
  inset:0;
  z-index:40;
  display:none;
  align-items:center;
  justify-content:center;
  padding:16px;
  background:rgba(1,4,12,.72);
  backdrop-filter:blur(3px);
  -webkit-backdrop-filter:blur(3px);
}

.ptModal.show {
  display:flex;
  animation:ptIn .25s ease-out;
}

@keyframes ptIn {
  from { opacity:0; transform:scale(.97); }
  to   { opacity:1; transform:none; }
}

.ptCard {
  width:min(460px, 100%);
  max-height:calc(100vh - 32px);
  overflow:auto;
  padding:18px;
  border-radius:18px;
  background:linear-gradient(180deg, #0f1d38, #071026);
  border:1px solid rgba(34,211,255,.35);
  box-shadow:0 0 40px rgba(30,150,255,.25);
  color:#eaf3ff;
}

.ptTitle {
  font-size:20px;
  font-weight:900;
  color:#ffba52;
  letter-spacing:.4px;
}

.ptLead {
  margin-top:4px;
  font-size:13px;
  color:#8ea6c8;
}

#ptCanvas {
  display:block;
  width:100%;
  height:220px;
  margin-top:12px;
  border-radius:12px;
  border:1px solid #15274a;
}

.ptSteps {
  margin:12px 0 0;
  padding:0;
  list-style:none;
  font-size:14px;
}

.ptSteps li {
  display:flex;
  gap:10px;
  align-items:flex-start;
  padding:5px 0;
  color:#c3d2ea;
}

.ptSteps b {
  flex:0 0 22px;
  height:22px;
  border-radius:50%;
  background:#0d2a5c;
  color:#22d3ff;
  font-size:12px;
  display:flex;
  align-items:center;
  justify-content:center;
}

.ptProgress {
  margin-top:12px;
  padding:10px 12px;
  border-radius:12px;
  background:#050b18;
  border:1px solid #15274a;
}

.ptBar {
  height:10px;
  margin-top:7px;
  border-radius:5px;
  background:#13213f;
  overflow:hidden;
}

#ptFill {
  height:100%;
  width:0;
  border-radius:5px;
  background:linear-gradient(90deg, #1e7bff, #22d3ff);
  transition:width .15s linear;
}

#ptFill.full {
  background:linear-gradient(90deg, #22d3ff, #7fe3ff);
}

#ptMsg {
  font-size:13px;
  font-weight:700;
  color:#eaf3ff;
}

.ptButtons {
  display:flex;
  gap:8px;
  margin-top:14px;
}

.ptButtons button {
  flex:1;
  padding:11px;
  border-radius:10px;
  border:1px solid #22406e;
  background:#0b1630;
  color:#eaf3ff;
  font-weight:900;
  font-size:13px;
  cursor:pointer;
}

.ptButtons button.main {
  background:#2f6bff;
  border-color:#2f6bff;
}

.ptCard.done {
  text-align:center;
}

.ptTick {
  width:86px;
  height:86px;
  margin:6px auto 8px;
  display:block;
}

.ptTick circle,
.ptTick path {
  fill:none;
  stroke:#22d3ff;
  stroke-width:3.5;
  stroke-linecap:round;
  stroke-linejoin:round;
}

.ptTick circle {
  stroke-dasharray:151;
  stroke-dashoffset:151;
  animation:ptDraw .6s ease-out forwards;
}

.ptTick path {
  stroke-dasharray:48;
  stroke-dashoffset:48;
  animation:ptDraw .35s .55s ease-out forwards;
}

.ptCard.skipped .ptTick circle,
.ptCard.skipped .ptTick path {
  stroke:#ffba52;
}

@keyframes ptDraw {
  to { stroke-dashoffset:0; }
}

#ptDoneTitle {
  font-size:21px;
  font-weight:900;
  color:#22d3ff;
}

.ptCard.skipped #ptDoneTitle {
  color:#ffba52;
}

#ptDoneText {
  margin-top:6px;
  font-size:14px;
  color:#c3d2ea;
  line-height:1.45;
}

/* START-UP SPLASH */''')

# ---------- HTML ----------
rep('''<div id="splash">''', '''<div id="ptModal" class="ptModal">
<div class="ptCard">
<div class="ptTitle">PULL TEST</div>
<div class="ptLead">The rod needs to learn which way is "toward the water". Do this once:</div>
<canvas id="ptCanvas"></canvas>
<ol class="ptSteps">
<li><b>1</b><span>Hold the line in front of the rod tip.</span></li>
<li><b>2</b><span>Pull it <strong>gently</strong> toward the sea for about 1 second, until the bar is full.</span></li>
<li><b>3</b><span>Let go. The rod does the rest.</span></li>
</ol>
<div class="ptProgress">
<div id="ptMsg">Waiting for your pull…</div>
<div class="ptBar"><div id="ptFill"></div></div>
</div>
<div class="ptButtons">
<button onclick="skipPull()">SKIP</button>
<button class="main" onclick="hidePullTest()">OK, I'M DOING IT</button>
</div>
</div>
</div>

<div id="ptDone" class="ptModal">
<div class="ptCard done" id="ptDoneCard">
<svg class="ptTick" viewBox="0 0 52 52"><circle cx="26" cy="26" r="24"/><path id="ptDonePath" d="M14 27l8 8 16-17"/></svg>
<div id="ptDoneTitle">DIRECTION LEARNED</div>
<div id="ptDoneText"></div>
<div class="ptButtons"><button class="main" onclick="closePullDone()">OK</button></div>
</div>
</div>

<div id="splash">''')

# ---------- JS ----------
rep('''    showState(d);
    showButtons(d);''', '''    showState(d);
    showButtons(d);
    pullTestPopup(d);''')

rep('''async function skipPull()
{''', r'''// =====================================================
// PULL TEST POP-UPS
// =====================================================

let ptPrev = "";
let ptOpen = false;
let ptHidden = false;
let ptDoneTimer = 0;
const ptCanvas = el("ptCanvas");
const ptCtx = ptCanvas.getContext("2d");

function pullTestPopup(d)
{
  if(d.state === "pulltest")
  {
    if(ptPrev !== "pulltest")
    {
      ptHidden = false;
      tone(880, 120, 0);
      tone(1175, 160, 160);
    }

    if(!ptHidden && !ptOpen)
      openPullTest();

    const frac = d.ptneed > 0 ? Math.min(1, Math.max(0, d.ptdeg) / d.ptneed) : 0;

    el("ptFill").style.width = (frac * 100) + "%";
    el("ptFill").classList.toggle("full", frac >= 1);
    el("ptMsg").innerText = frac >= 1
      ? "Good – now let go"
      : "Tip moved " + Math.max(0, d.ptdeg).toFixed(1) + "° of " + d.ptneed.toFixed(1) + "°";
  }
  else if(ptPrev === "pulltest")
  {
    closePullTest();
    showPullDone(d.dir && d.dirage >= 0 && d.dirage < 15);
  }

  ptPrev = d.state;
}

function openPullTest()
{
  ptOpen = true;
  el("ptModal").classList.add("show");
  ptLoop();
}

function hidePullTest()
{
  ptHidden = true;
  closePullTest();
}

function closePullTest()
{
  ptOpen = false;
  el("ptModal").classList.remove("show");
}

function showPullDone(ok)
{
  el("ptDoneCard").classList.toggle("skipped", !ok);
  el("ptDonePath").setAttribute("d", ok ? "M14 27l8 8 16-17" : "M26 14v15 M26 37v1");
  el("ptDoneTitle").innerText = ok ? "DIRECTION LEARNED" : "PULL TEST SKIPPED";
  el("ptDoneText").innerText = ok
    ? "The rod now knows which way is toward the water. Slow pulls and drop-backs are watched. Rod armed."
    : "No direction yet. Knocks and strong hits are still detected, but slow pulls and drop-backs need the pull test. Do it after the next READY.";

  // replay the drawing animation
  const svg = el("ptDoneCard").querySelector(".ptTick");
  svg.replaceWith(svg.cloneNode(true));

  el("ptDone").classList.add("show");

  if(ok)
  {
    tone(1047, 120, 0);
    tone(1319, 120, 130);
    tone(1568, 220, 260);
  }

  clearTimeout(ptDoneTimer);
  ptDoneTimer = setTimeout(closePullDone, ok ? 6000 : 15000);
}

function closePullDone()
{
  el("ptDone").classList.remove("show");
}

// How-to animation: hold the line, pull gently toward the sea, let go
function ptLoop()
{
  if(!ptOpen)
    return;

  const box = ptCanvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;

  if(ptCanvas.width !== Math.floor(box.width * dpr) || ptCanvas.height !== Math.floor(box.height * dpr))
  {
    ptCanvas.width = Math.floor(box.width * dpr);
    ptCanvas.height = Math.floor(box.height * dpr);
  }

  ptCtx.setTransform(dpr, 0, 0, dpr, 0, 0);

  const w = box.width, h = box.height;
  const now = performance.now() / 1000;
  const c = now % 4.2;
  const ease = f => (1 - Math.cos(Math.max(0, Math.min(1, f)) * Math.PI)) / 2;

  let pull = 0, hand = 1, step = "";

  if(c < 0.6)       { hand = c / 0.6; step = "1  Hold the line"; }
  else if(c < 1.9)  { pull = ease((c - 0.6) / 1.3); step = "2  Pull gently toward the sea"; }
  else if(c < 2.5)  { pull = 1; step = "2  Pull gently toward the sea"; }
  else if(c < 2.8)  { pull = 1 - (c - 2.5) / 0.3; hand = 1 - (c - 2.5) / 0.3; step = "3  Let go"; }
  else              { hand = 0; step = "3  Let go ✓"; }

  let tilt = pull * 3;

  if(c >= 2.5)
    tilt += 1.2 * Math.exp(-(c - 2.5) * 3) * Math.sin((c - 2.5) * 14);

  const r = drawRodScene(ptCtx, w, h,
  {
    t: now, tilt: tilt, knock: 0, state: pull > 0.05 ? "pull" : "armed", label: "",
    dir: true, waveswing: 0.6, period: 6, wind: 0, allow: 0, bucket: 0, drop: 0,
    mode: "normal", anim: 0, hideLine: hand > 0, noHud: true
  });

  const tip = r.tip, end = r.end;
  const dx = end[0] - tip[0], dy = end[1] - tip[1];
  const len = Math.hypot(dx, dy);
  const ux = dx / len, uy = dy / len;
  const p0 = [tip[0] + dx * 0.22, tip[1] + dy * 0.22];
  const hp = [p0[0] + (ux * 0.6 + 0.4) * pull * 26, p0[1] + (uy * 0.6 + 0.8) * pull * 26];

  if(hand > 0)
  {
    ptCtx.strokeStyle = "rgba(234,243,255,.75)";
    ptCtx.lineWidth = 1.2;
    ptCtx.beginPath();
    ptCtx.moveTo(tip[0], tip[1]);
    ptCtx.lineTo(hp[0], hp[1]);
    ptCtx.lineTo(end[0], end[1]);
    ptCtx.stroke();

    // arrow: which way to pull
    if(c > 0.6 && c < 2.5)
    {
      const a0 = [p0[0] + 16, p0[1] + 4], a1 = [p0[0] + 16 + 34, p0[1] + 4 + 30];

      ptCtx.strokeStyle = "#22d3ff";
      ptCtx.fillStyle = "#22d3ff";
      ptCtx.lineWidth = 2.5;
      ptCtx.beginPath(); ptCtx.moveTo(a0[0], a0[1]); ptCtx.lineTo(a1[0], a1[1]); ptCtx.stroke();
      ptCtx.beginPath(); ptCtx.moveTo(a1[0] + 3, a1[1] + 3); ptCtx.lineTo(a1[0] - 9, a1[1] - 1); ptCtx.lineTo(a1[0] - 1, a1[1] - 9); ptCtx.closePath(); ptCtx.fill();
      ptCtx.font = "bold 12px Arial";
      ptCtx.textAlign = "left";
      ptCtx.textBaseline = "middle";
      ptCtx.fillText("gently", a1[0] + 6, a1[1] + 4);
    }

    // hand gripping the line
    const z = Math.max(1.4, w / 280);
    const hx = hp[0], hy = hp[1];

    ptCtx.globalAlpha = hand;
    ptCtx.fillStyle = "#1e7bff";                                                                         // sleeve
    ptCtx.beginPath(); ptCtx.ellipse(hx + 13 * z, hy + 17 * z, 7 * z, 10 * z, -0.6, 0, Math.PI * 2); ptCtx.fill();
    ptCtx.fillStyle = "#f2c6a0";
    ptCtx.strokeStyle = "#8a5a3c";
    ptCtx.lineWidth = 1.2;
    ptCtx.beginPath(); ptCtx.ellipse(hx + 5 * z, hy + 6 * z, 7.5 * z, 9 * z, -0.6, 0, Math.PI * 2); ptCtx.fill(); ptCtx.stroke();    // fist

    for(let i = 0; i < 4; i++)                                                                            // fingers round the line
    {
      ptCtx.beginPath();
      ptCtx.ellipse(hx + (1 + i * 2.6) * z, hy + (-1 + i * 3.2) * z, 2.6 * z, 2 * z, -0.6, 0, Math.PI * 2);
      ptCtx.fill(); ptCtx.stroke();
    }

    ptCtx.beginPath(); ptCtx.ellipse(hx - 2 * z, hy + 5 * z, 2.4 * z, 5 * z, 0.5, 0, Math.PI * 2); ptCtx.fill(); ptCtx.stroke();     // thumb
    ptCtx.globalAlpha = 1;
  }

  ptCtx.font = "bold 15px Arial";
  ptCtx.textAlign = "left";
  ptCtx.textBaseline = "top";
  ptCtx.fillStyle = "rgba(4,10,24,.6)";
  ptCtx.fillRect(8, 8, ptCtx.measureText(step).width + 16, 26);
  ptCtx.fillStyle = c < 2.5 ? "#ffba52" : "#22d3ff";
  ptCtx.fillText(step, 16, 13);

  requestAnimationFrame(ptLoop);
}

async function skipPull()
{''')

open(path, 'w').write(s)
print("ok", path)

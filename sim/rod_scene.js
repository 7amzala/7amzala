// Surf rod in a sand spike, on the beach at night (v3, detailed).
// s: t, tilt (+ toward water), knock 0..1, state (armed/pull/drop/strike),
//    label, dir, waveswing, period, wind (0..3), allow, bucket, drop,
//    mode (normal / reel / out / cast) and anim 0..1 for reel in and cast
function drawRodScene(ctx, w, h, s)
{
  const t = s.t;
  const k = Math.max(0.6, Math.min(h, w * 0.9) / 520);   // size scale
  const swing = s.waveswing;
  const period = s.period;
  const horizon = h * 0.47;
  const TAU = Math.PI * 2;

  const colour = s.state === "strike" ? "#ff4d5e" : s.state === "pull" || s.state === "drop" ? "#ffba52" : "#22d3ff";
  const rgb = s.state === "strike" ? "255,77,94" : s.state === "pull" || s.state === "drop" ? "255,186,82" : "34,211,255";
  const flash = s.state === "strike" ? 0.55 + 0.45 * Math.sin(t * 14) : 1;

  ctx.clearRect(0, 0, w, h);

  // ================= SKY =================
  let g = ctx.createLinearGradient(0, 0, 0, horizon);
  g.addColorStop(0, "#020611");
  g.addColorStop(0.55, "#0a1a3c");
  g.addColorStop(0.9, "#173f73");
  g.addColorStop(1, "#2a5f94");
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, w, horizon + 1);

  // stars
  for(let i = 0; i < 90; i++)
  {
    const sx = (i * 97.31 % 1) * w;
    const sy = Math.pow((i * 57.73) % 1, 1.4) * horizon * 0.85;
    const r = (i % 7 === 0 ? 1.4 : 0.8) * k;

    ctx.globalAlpha = 0.25 + 0.35 * (1 + Math.sin(t * (0.8 + i % 5 * 0.4) + i));
    ctx.fillStyle = i % 9 === 0 ? "#bfe3ff" : "#ffffff";
    ctx.beginPath();
    ctx.arc(sx, sy, r, 0, TAU);
    ctx.fill();
  }

  ctx.globalAlpha = 1;

  // moon
  const mr = 26 * k;
  const mx = w * 0.58;
  const my = h * 0.22;

  g = ctx.createRadialGradient(mx, my, mr * 0.5, mx, my, mr * 7);
  g.addColorStop(0, "rgba(170,210,255,.30)");
  g.addColorStop(1, "rgba(170,210,255,0)");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(mx, my, mr * 7, 0, TAU);
  ctx.fill();

  g = ctx.createRadialGradient(mx - mr * 0.35, my - mr * 0.35, mr * 0.1, mx, my, mr);
  g.addColorStop(0, "#ffffff");
  g.addColorStop(1, "#cfdcef");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(mx, my, mr, 0, TAU);
  ctx.fill();

  ctx.fillStyle = "rgba(120,140,170,.25)";

  for(const c of [[-0.3, -0.2, 0.22], [0.25, 0.1, 0.16], [-0.05, 0.4, 0.12], [0.35, -0.35, 0.1]])
  {
    ctx.beginPath();
    ctx.arc(mx + c[0] * mr, my + c[1] * mr, c[2] * mr, 0, TAU);
    ctx.fill();
  }

  // clouds drift with the wind
  const windSpeed = (4 + (s.wind || 0) * 14) * k;

  for(let i = 0; i < 5; i++)
  {
    const cw = (160 + i * 40) * k;
    const cx = ((i * 0.27 * w + t * windSpeed * (0.6 + i * 0.15)) % (w + cw * 2)) - cw;
    const cy = horizon * (0.18 + i * 0.13);

    ctx.fillStyle = "rgba(150,180,220," + (0.05 + (i % 2) * 0.03) + ")";

    for(let j = 0; j < 4; j++)
    {
      ctx.beginPath();
      ctx.ellipse(cx + j * cw * 0.22, cy + Math.sin(j * 2.1) * 6 * k, cw * 0.3, 12 * k * (1 + (j % 2) * 0.4), 0, 0, TAU);
      ctx.fill();
    }
  }

  // ================= SEA =================
  g = ctx.createLinearGradient(0, horizon, 0, h);
  g.addColorStop(0, "#22629f");
  g.addColorStop(0.35, "#0f4583");
  g.addColorStop(1, "#06234c");
  ctx.fillStyle = g;
  ctx.fillRect(0, horizon, w, h - horizon);

  // horizon haze
  g = ctx.createLinearGradient(0, horizon - 10 * k, 0, horizon + 14 * k);
  g.addColorStop(0, "rgba(120,170,230,0)");
  g.addColorStop(0.5, "rgba(120,170,230,.25)");
  g.addColorStop(1, "rgba(120,170,230,0)");
  ctx.fillStyle = g;
  ctx.fillRect(0, horizon - 10 * k, w, 24 * k);

  // moonlight path
  for(let i = 0; i < 26; i++)
  {
    const f = i / 26;
    const yy = horizon + 2 + Math.pow(f, 1.6) * (h - horizon) * 0.75;
    const ww = (4 + f * 70 * k) * (0.5 + 0.5 * Math.abs(Math.sin(t * 1.7 + i * 1.3)));

    ctx.fillStyle = "rgba(210,232,255," + (0.42 - f * 0.32) + ")";
    ctx.fillRect(mx - ww / 2 + Math.sin(t * 0.9 + i) * 6 * k * f, yy, ww, (1 + f * 2) * k);
  }

  // swells: darker troughs and light crests, rolling toward the shore
  for(let j = 0; j < 9; j++)
  {
    const f = ((j + t / period) % 9) / 9;            // 0 at the horizon, 1 at the shore
    const y = horizon + Math.pow(f, 2) * (h - horizon);
    const amp = (0.5 + swing * 1.3) * (0.3 + f * 2) * k;

    ctx.beginPath();

    for(let x = 0; x <= w; x += 8)
    {
      const yy = y + Math.sin(x * 0.012 / (0.3 + f) + j * 1.9 + t * 0.4) * amp;

      if(x === 0) ctx.moveTo(x, yy);
      else ctx.lineTo(x, yy);
    }

    ctx.strokeStyle = "rgba(160,225,255," + (0.08 + f * 0.30) + ")";
    ctx.lineWidth = (0.7 + f * 2.2) * k;
    ctx.stroke();

    ctx.lineTo(w, y + amp + 6 * f * k);
    ctx.lineTo(0, y + amp + 6 * f * k);
    ctx.closePath();
    ctx.fillStyle = "rgba(2,15,40," + (0.06 + f * 0.10) + ")";
    ctx.fill();
  }

  // ================= BEACH =================
  const shoreY = x => h * 0.68 + (x / w) * h * 0.24 - Math.sin(x * 0.012 + 1) * 6 * k;
  const surge = (Math.sin(t * TAU / period) * 0.5 + 0.5) * (6 + swing * 9) * k;   // swash up and down the beach

  // breaking wave just off the beach
  const breakPhase = (t / period) % 1;
  const breakY = x => shoreY(x) - (26 - breakPhase * 18) * k;

  ctx.beginPath();

  for(let x = 0; x <= w; x += 6)
  {
    const yy = breakY(x) + Math.sin(x * 0.05 + t * 2) * 2 * k;

    if(x === 0) ctx.moveTo(x, yy);
    else ctx.lineTo(x, yy);
  }

  ctx.strokeStyle = "rgba(235,245,255," + (0.25 + 0.5 * Math.sin(breakPhase * Math.PI) * Math.min(1, 0.3 + swing / 2)) + ")";
  ctx.lineWidth = (2 + swing * 1.5) * k;
  ctx.stroke();

  // wet sand (shines)
  ctx.beginPath();
  ctx.moveTo(0, h);
  for(let x = 0; x <= w; x += 6) ctx.lineTo(x, shoreY(x) - surge - 4 * k);
  ctx.lineTo(w, h);
  ctx.closePath();
  g = ctx.createLinearGradient(0, h * 0.6, 0, h);
  g.addColorStop(0, "#5d5040");
  g.addColorStop(1, "#3d3226");
  ctx.fillStyle = g;
  ctx.fill();

  // moon shine on the wet sand
  ctx.fillStyle = "rgba(200,225,255,.10)";
  ctx.beginPath();
  ctx.ellipse(mx * 0.9, shoreY(mx * 0.9) + 6 * k, 60 * k, 5 * k, 0.2, 0, TAU);
  ctx.fill();

  // swash foam: lacy edge
  for(let layer = 0; layer < 3; layer++)
  {
    ctx.beginPath();

    for(let x = 0; x <= w; x += 5)
    {
      const yy = shoreY(x) - surge - 4 * k - layer * 4 * k + Math.sin(x * (0.09 + layer * 0.05) + t * (2 + layer)) * 1.6 * k;

      if(x === 0) ctx.moveTo(x, yy);
      else ctx.lineTo(x, yy);
    }

    ctx.strokeStyle = "rgba(245,250,255," + (0.85 - layer * 0.28) + ")";
    ctx.lineWidth = (2.4 - layer * 0.6) * k;
    ctx.stroke();
  }

  // dry sand / dune
  const sandY = x => shoreY(x) + 10 * k - Math.max(0, 1 - x / (w * 0.5)) * 30 * k;

  ctx.beginPath();
  ctx.moveTo(0, h);
  for(let x = 0; x <= w; x += 6) ctx.lineTo(x, sandY(x));
  ctx.lineTo(w, h);
  ctx.closePath();
  g = ctx.createLinearGradient(0, h * 0.55, 0, h);
  g.addColorStop(0, "#d8b98a");
  g.addColorStop(0.5, "#b8946a");
  g.addColorStop(1, "#7d6040");
  ctx.fillStyle = g;
  ctx.fill();

  // moonlit rim on the dune
  ctx.beginPath();
  for(let x = 0; x <= w; x += 6) x ? ctx.lineTo(x, sandY(x)) : ctx.moveTo(x, sandY(x));
  ctx.strokeStyle = "rgba(255,240,215,.35)";
  ctx.lineWidth = 1.5 * k;
  ctx.stroke();

  // sand grain and ripples
  for(let i = 0; i < 220; i++)
  {
    const gx = (i * 61.73 % 1) * w;
    const gy = sandY(gx) + 4 * k + ((i * 37.31) % 1) * (h - sandY(gx));

    ctx.fillStyle = i % 3 ? "rgba(70,45,20,.22)" : "rgba(255,240,210,.18)";
    ctx.fillRect(gx, gy, 1.3 * k, 1.3 * k);
  }

  ctx.strokeStyle = "rgba(90,60,30,.18)";
  ctx.lineWidth = 1 * k;

  for(let i = 0; i < 9; i++)
  {
    const rx = (i * 0.11 + 0.02) * w, ry = sandY(rx) + (20 + (i % 3) * 18) * k;

    ctx.beginPath();
    ctx.ellipse(rx, ry, 26 * k, 3 * k, 0.15, Math.PI * 1.1, Math.PI * 1.9);
    ctx.stroke();
  }

  // ================= SAND SPIKE =================
  const lean = 12 * Math.PI / 180;
  const ux = Math.sin(lean), uy = -Math.cos(lean);      // up the spike
  const nx = Math.cos(lean), ny = Math.sin(lean);       // toward the sea
  const baseX = Math.max(w * 0.17, 70 * k);
  const baseY = sandY(baseX) + 34 * k;
  const spikeLen = Math.min(h * 0.33, 190 * k * 1.1);
  const spikeTop = [baseX + ux * spikeLen, baseY + uy * spikeLen];

  // shadow (moon is up-right, so it falls down-left)
  ctx.fillStyle = "rgba(40,25,10,.30)";
  ctx.beginPath();
  ctx.moveTo(baseX, baseY);
  ctx.lineTo(baseX - spikeLen * 0.55, baseY + 10 * k);
  ctx.lineTo(baseX - spikeLen * 0.55, baseY + 14 * k);
  ctx.lineTo(baseX + 4 * k, baseY + 4 * k);
  ctx.closePath();
  ctx.fill();

  // sand mound where it goes in
  ctx.fillStyle = "#c2a074";
  ctx.beginPath();
  ctx.ellipse(baseX, baseY, 16 * k, 5 * k, 0, Math.PI, TAU);
  ctx.fill();

  function tube(x0, y0, x1, y1, width, dark, light)
  {
    const gg = ctx.createLinearGradient(x0 - nx * width, y0 - ny * width, x0 + nx * width, y0 + ny * width);
    gg.addColorStop(0, dark);
    gg.addColorStop(0.45, light);
    gg.addColorStop(1, dark);
    ctx.strokeStyle = gg;
    ctx.lineWidth = width;
    ctx.lineCap = "round";
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.stroke();
  }

  tube(baseX, baseY - 3 * k, spikeTop[0], spikeTop[1], 7 * k, "#0b3c8c", "#4aa3ff");

  // ================= ROD =================
  const buttCup = [baseX + ux * spikeLen * 0.2, baseY + uy * spikeLen * 0.2];
  const butt = [buttCup[0] + nx * 9 * k, buttCup[1] + ny * 9 * k];
  const L = Math.min(h * 0.6, spikeLen * 2.2);
  const a0 = -(Math.PI / 2 - lean);
  const B0 = 0.30;

  let B = B0 + 0.6 * Math.tanh(s.tilt / 4);

  if(s.knock > 0)
    B += s.knock * 0.05 * Math.sin(t * TAU * 7);

  B = Math.max(0, Math.min(1.5, B));

  const mode = s.mode || "normal";
  const p = s.anim || 0;
  const ease = f => (1 - Math.cos(Math.max(0, Math.min(1, f)) * Math.PI)) / 2;
  let swingRot = 0;                                   // whole rod turns about the butt (cast)

  if(mode === "reel")
  {
    B = B0 + 0.22 + 0.05 * Math.sin(t * 9);           // loaded by the lead coming in
  }
  else if(mode === "out")
  {
    B = 0.06;                                         // only the lead hanging
  }
  else if(mode === "cast")
  {
    if(p < 0.25)                                      // back over the shoulder, tip loads backward
    {
      swingRot = -0.55 * ease(p / 0.25);
      B = 0.06 - 0.35 * ease(p / 0.25);
    }
    else if(p < 0.38)                                 // whip forward
    {
      const f = (p - 0.25) / 0.13;
      swingRot = -0.55 + 0.7 * f;
      B = -0.3 + 1.2 * f;
    }
    else if(p < 0.6)                                  // follow through, the rod shivers
    {
      const f = (p - 0.38) / 0.22;
      swingRot = 0.15 * (1 - f);
      B = 0.1 + 0.8 * (1 - f) + 0.15 * Math.sin(f * 20) * (1 - f);
    }
    else                                              // line tightens, back to normal
    {
      B = 0.1 + (B0 - 0.1) * ease((p - 0.6) / 0.4);
    }
  }

  const cupU = (spikeLen * 0.8) / L;
  const N = 64;

  function rodPoints(bend, rot)
  {
    const pts = [];
    let x = butt[0], y = butt[1];

    for(let i = 0; i <= N; i++)
    {
      const u = i / N;
      const v = Math.max(0, (u - cupU) / (1 - cupU));
      const ang = a0 + rot + bend * v * (2 - v);

      pts.push([x, y, ang]);
      x += Math.cos(ang) * L / N;
      y += Math.sin(ang) * L / N;
    }

    return pts;
  }

  const pts = rodPoints(B, swingRot);
  const tip = pts[N];
  const at = u => pts[Math.min(N, Math.round(u * N))];
  const side = (p, d) => [p[0] + Math.sin(p[2]) * -d, p[1] + Math.cos(p[2]) * d];   // + d = underside (toward the sea)

  // normal position (dashed)
  const rest = rodPoints(B0, 0);
  if(mode === "normal")
  {
  ctx.setLineDash([5 * k, 6 * k]);
  ctx.strokeStyle = "rgba(234,243,255,.22)";
  ctx.lineWidth = 1.2 * k;
  ctx.beginPath();
  rest.forEach((q, i) => i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]));
  ctx.stroke();
  ctx.setLineDash([]);
  }

  const width = u => (9 - 7.6 * Math.pow(u, 0.8)) * k;

  // blank as a filled taper
  function blank(u0, u1, fill)
  {
    const a = Math.round(u0 * N), b = Math.round(u1 * N);

    ctx.beginPath();

    for(let i = a; i <= b; i++)
    {
      const q = side(pts[i], -width(i / N) / 2);
      if(i === a) ctx.moveTo(q[0], q[1]);
      else ctx.lineTo(q[0], q[1]);
    }

    for(let i = b; i >= a; i--)
    {
      const q = side(pts[i], width(i / N) / 2);
      ctx.lineTo(q[0], q[1]);
    }

    ctx.closePath();
    ctx.fillStyle = fill;
    ctx.fill();
  }

  // ---- line from the reel, through the guides, to the tip ----
  const reelU = 0.21;
  const rp = at(reelU);
  const spool = side(rp, 22 * k);
  const guides = [0.34, 0.47, 0.58, 0.68, 0.76, 0.83, 0.89, 0.945];
  const ringAt = u => side(at(u), (13 - 10 * u) * k);

  ctx.strokeStyle = "rgba(234,243,255,.55)";
  ctx.lineWidth = 0.8 * k;
  ctx.beginPath();
  ctx.moveTo(spool[0], spool[1]);
  for(const u of guides) { const q = ringAt(u); ctx.lineTo(q[0], q[1]); }
  ctx.lineTo(tip[0], tip[1]);
  ctx.stroke();

  // ---- handle (EVA), reel seat, blank ----
  blank(0, 0.17, "#20262f");
  blank(0.17, 0.25, "#55657c");
  blank(0.25, 1, "#1b2436");

  // gloss along the blank
  ctx.strokeStyle = "rgba(200,225,255,.55)";
  ctx.lineWidth = 1.4 * k;
  ctx.beginPath();
  for(let i = Math.round(0.25 * N); i <= N - 2; i++) { const q = side(pts[i], -width(i / N) * 0.22); i === Math.round(0.25 * N) ? ctx.moveTo(q[0], q[1]) : ctx.lineTo(q[0], q[1]); }
  ctx.stroke();

  // EVA grip texture
  ctx.strokeStyle = "rgba(255,255,255,.07)";
  ctx.lineWidth = 1 * k;

  for(let i = 1; i < Math.round(0.17 * N); i += 2)
  {
    const a = side(pts[i], -width(i / N) / 2), b = side(pts[i], width(i / N) / 2);
    ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
  }

  // tip section in the state colour (painted tip, like a real surf rod)
  ctx.globalAlpha = flash;
  blank(0.86, 1, colour);
  ctx.globalAlpha = 1;

  // whippings next to the guides (brand cyan)
  for(const u of guides)
  {
    const p = at(u - 0.008), q = at(u + 0.008);
    const wd = width(u) * 0.9;

    ctx.strokeStyle = "#1fb6ff";
    ctx.lineWidth = wd;
    ctx.lineCap = "butt";
    ctx.beginPath(); ctx.moveTo(p[0], p[1]); ctx.lineTo(q[0], q[1]); ctx.stroke();
  }

  // guides: foot + ring on the underside
  for(const u of guides)
  {
    const p = at(u), q = ringAt(u);
    const r = (6.5 - 5 * u) * k;

    ctx.strokeStyle = "#9fb3cf";
    ctx.lineWidth = 1.2 * k;
    ctx.beginPath(); ctx.moveTo(p[0], p[1]); ctx.lineTo(q[0], q[1]); ctx.stroke();
    ctx.lineWidth = 1.6 * k;
    ctx.beginPath(); ctx.arc(q[0], q[1], r, 0, TAU); ctx.stroke();
  }

  // ---- spinning reel ----
  ctx.save();
  ctx.translate(rp[0], rp[1]);
  ctx.rotate(rp[2] + Math.PI / 2);                    // x across the rod (toward the sea), y along it

  ctx.fillStyle = "#7c8aa3";                          // stem
  ctx.fillRect(2 * k, -2 * k, 10 * k, 4 * k);

  g = ctx.createLinearGradient(10 * k, 0, 30 * k, 0);  // body
  g.addColorStop(0, "#2b3446");
  g.addColorStop(0.5, "#56657f");
  g.addColorStop(1, "#1d2433");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.ellipse(21 * k, 4 * k, 10 * k, 13 * k, 0, 0, TAU);
  ctx.fill();

  ctx.fillStyle = "#c7d3e6";                          // spool with line
  ctx.beginPath();
  ctx.ellipse(21 * k, -12 * k, 9 * k, 5 * k, 0, 0, TAU);
  ctx.fill();
  ctx.fillStyle = "#1fb6ff";
  ctx.beginPath();
  ctx.ellipse(21 * k, -12 * k, 6.5 * k, 3.4 * k, 0, 0, TAU);
  ctx.fill();

  ctx.strokeStyle = "#a9b8cf";                         // handle arm + knob
  ctx.lineWidth = 2 * k;
  const crankA = mode === "reel" ? t * 14 : 0.67;      // the handle turns while reeling
  const knob = [30 * k + Math.cos(crankA) * 13 * k, 6 * k + Math.sin(crankA) * 13 * k];
  ctx.beginPath(); ctx.moveTo(30 * k, 6 * k); ctx.lineTo(knob[0], knob[1]); ctx.stroke();
  ctx.fillStyle = "#11151d";
  ctx.beginPath(); ctx.ellipse(knob[0] + 1 * k, knob[1] + 2 * k, 3 * k, 4.5 * k, 0, 0, TAU); ctx.fill();
  ctx.restore();

  // ---- cups on the spike ----
  function cup(p, wd, ht)
  {
    ctx.save();
    ctx.translate(p[0], p[1]);
    ctx.rotate(lean);
    ctx.fillStyle = "#0e1117";
    ctx.beginPath();
    ctx.moveTo(-wd / 2, -ht / 2);
    ctx.lineTo(wd / 2 + 8 * k, -ht / 2);
    ctx.lineTo(wd / 2 + 8 * k, ht / 2);
    ctx.lineTo(-wd / 2, ht / 2);
    ctx.closePath();
    ctx.fill();
    ctx.strokeStyle = "rgba(255,255,255,.15)";
    ctx.lineWidth = 1 * k;
    ctx.stroke();
    ctx.restore();
  }

  cup([spikeTop[0] + nx * 2 * k, spikeTop[1] + ny * 2 * k], 12 * k, 10 * k);
  cup([buttCup[0] + nx * 2 * k, buttCup[1] + ny * 2 * k], 12 * k, 16 * k);

  ctx.fillStyle = "#1b1f27";                          // thumb screw
  ctx.beginPath();
  ctx.arc(buttCup[0] - nx * 9 * k, buttCup[1] - ny * 9 * k, 3.5 * k, 0, TAU);
  ctx.fill();

  // ---- StrikeSense box on the spike, cable up to the sensor ----
  const box = [baseX + ux * spikeLen * 0.07 - nx * 4 * k, baseY + uy * spikeLen * 0.07 - ny * 4 * k];
  const sp = at(0.88);

  ctx.strokeStyle = "rgba(34,211,255,.55)";
  ctx.lineWidth = 1 * k;
  ctx.beginPath();
  ctx.moveTo(box[0], box[1] - 7 * k);
  for(let i = Math.round(0.05 * N); i <= Math.round(0.88 * N); i++) { const q = side(pts[i], -width(i / N) / 2 - 1.2 * k); ctx.lineTo(q[0], q[1]); }
  ctx.stroke();

  ctx.save();
  ctx.translate(box[0], box[1]);
  ctx.rotate(lean);
  ctx.fillStyle = "#0b1630";
  ctx.strokeStyle = "#22d3ff";
  ctx.lineWidth = 1.3 * k;
  ctx.beginPath();
  ctx.roundRect(-10 * k, -8 * k, 20 * k, 16 * k, 3 * k);
  ctx.fill();
  ctx.stroke();
  ctx.fillStyle = "rgba(" + rgb + "," + flash + ")";
  ctx.beginPath();
  ctx.arc(5 * k, -3 * k, 1.8 * k, 0, TAU);                 // status LED
  ctx.fill();
  ctx.restore();

  ctx.fillStyle = "#22d3ff";                               // sensor
  ctx.save();
  ctx.translate(sp[0], sp[1]);
  ctx.rotate(sp[2]);
  ctx.fillRect(-4 * k, -4 * k, 8 * k, 8 * k);
  ctx.restore();

  // ================= LINE TO THE SEA =================
  const endX = w * 0.93, endY = horizon + (h - horizon) * 0.14;

  // lead + hook
  function lead(x, y)
  {
    const z = 1.7 * k;                              // drawn a bit big so it can be seen
    ctx.strokeStyle = "rgba(234,243,255,.6)";
    ctx.lineWidth = 0.8 * z;
    ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + 5 * z, y + 9 * z); ctx.stroke();      // snood
    ctx.strokeStyle = "#c3d2ea";
    ctx.lineWidth = 1.2 * z;
    ctx.beginPath(); ctx.arc(x + 5 * z, y + 12 * z, 2.6 * z, -Math.PI / 2, Math.PI * 0.9); ctx.stroke();  // hook
    ctx.fillStyle = "#8a96ab";
    ctx.beginPath(); ctx.ellipse(x, y + 4 * z, 3.2 * z, 5.5 * z, 0, 0, TAU); ctx.fill();   // lead
    ctx.fillStyle = "rgba(255,255,255,.35)";
    ctx.beginPath(); ctx.ellipse(x - 1 * z, y + 2.5 * z, 1 * z, 2 * z, 0, 0, TAU); ctx.fill();
  }

  function straightLine(x, y, alpha)
  {
    ctx.strokeStyle = "rgba(234,243,255," + alpha + ")";
    ctx.lineWidth = 1 * k;
    ctx.beginPath(); ctx.moveTo(tip[0], tip[1]); ctx.lineTo(x, y); ctx.stroke();
  }

  const hang = [tip[0] + Math.sin(t * 1.8) * 6 * k, tip[1] + 34 * k];   // lead hanging from the tip
  const inX = w * 0.44, inY = shoreY(w * 0.44) - surge - 10 * k;      // where the rig comes out of the surf

  if(mode === "reel")
  {
    if(p < 0.75)                                      // rig comes in through the waves
    {
      const f = ease(p / 0.75);
      const x = endX + (inX - endX) * f, y = endY + (inY - endY) * f;

      straightLine(x, y, 0.8);

      ctx.strokeStyle = "rgba(220,240,255,.45)";      // V wake behind it
      ctx.lineWidth = 1 * k;
      ctx.beginPath();
      ctx.moveTo(x + 22 * k, y - 4 * k); ctx.lineTo(x, y); ctx.lineTo(x + 22 * k, y + 5 * k);
      ctx.stroke();
    }
    else                                              // out of the water, up to the tip
    {
      const f = ease((p - 0.75) / 0.25);
      const x = inX + (hang[0] - inX) * f, y = inY + (hang[1] - inY) * f;

      straightLine(x, y, 0.8);
      lead(x, y);

      ctx.fillStyle = "rgba(200,230,255,.7)";         // drips
      for(let i = 0; i < 3; i++) ctx.fillRect(x - 1 * k, y + (14 + i * 6 + ((t * 40) % 6)) * k, 1.2 * k, 2.5 * k);
    }
  }
  else if(mode === "out")
  {
    straightLine(hang[0], hang[1], 0.7);
    lead(hang[0], hang[1]);
  }
  else if(mode === "cast" && p < 0.83)
  {
    const R = rodPoints(B0, 0)[N];                    // release point

    if(p < 0.33)                                      // lead swings with the rod
    {
      const x = tip[0] - Math.sin(swingRot) * 18 * k, y = tip[1] + 24 * k;
      straightLine(x, y, 0.7);
      lead(x, y);
    }
    else                                              // flying out over the sea
    {
      const f = Math.min(1, (p - 0.33) / 0.5);
      const H = Math.min(h * 0.32, Math.max(10 * k, (R[1] + endY) / 2 - h * 0.14));   // arc stays in view
      const fx = R[0] + (endX - R[0]) * f;
      const fy = R[1] + (endY - R[1]) * f - H * 4 * f * (1 - f);

      ctx.strokeStyle = "rgba(234,243,255,.6)";
      ctx.lineWidth = 1 * k;
      ctx.beginPath();
      ctx.moveTo(tip[0], tip[1]);
      ctx.quadraticCurveTo((tip[0] + fx) / 2, Math.min(tip[1], fy) - 10 * k, fx, fy);
      ctx.stroke();

      for(let i = 1; i <= 6; i++)                     // trail
      {
        const g2 = Math.max(0, f - i * 0.025);
        ctx.fillStyle = "rgba(200,230,255," + (0.35 - i * 0.05) + ")";
        ctx.beginPath();
        ctx.arc(R[0] + (endX - R[0]) * g2, R[1] + (endY - R[1]) * g2 - H * 4 * g2 * (1 - g2), 1.4 * k, 0, TAU);
        ctx.fill();
      }

      lead(fx, fy);
    }
  }
  else
  {
    const tension = Math.max(0, Math.min(1, (B - B0) / 0.25 + 0.55));
    const midX = (tip[0] + endX) / 2, midY = (tip[1] + endY) / 2;
    const cpX = midX + (tip[0] + (endX - tip[0]) * 0.2 - midX) * (1 - tension);
    const cpY = midY + (endY + h * 0.12 - midY) * (1 - tension);

    ctx.strokeStyle = s.state === "strike" ? "rgba(255,77,94," + flash + ")" : "rgba(234,243,255," + (0.35 + tension * 0.35) + ")";
    ctx.lineWidth = (s.state === "strike" ? 1.6 : 1) * k;
    ctx.beginPath();
    ctx.moveTo(tip[0], tip[1]);
    ctx.quadraticCurveTo(cpX, cpY, endX, endY);
    ctx.stroke();
  }

  // where the line goes in: rings; a splash on a strike or when the lead lands
  const lineIn = mode === "normal" || (mode === "cast" && p >= 0.83);

  for(let i = 0; i < 3 && lineIn; i++)
  {
    const f = ((t * 0.5 + i / 3) % 1);

    ctx.strokeStyle = "rgba(200,235,255," + (0.5 * (1 - f)) + ")";
    ctx.lineWidth = 1 * k;
    ctx.beginPath();
    ctx.ellipse(endX, endY, (4 + f * 18) * k, (1.2 + f * 4) * k, 0, 0, TAU);
    ctx.stroke();
  }

  const landing = mode === "cast" && p >= 0.83;

  if(lineIn && (s.state === "strike" || s.knock > 0.4 || landing))
  {
    const big = s.state === "strike" || landing ? 1 : 0.4;

    for(let i = 0; i < 14; i++)
    {
      const ph = (t * 1.6 + i * 0.13) % 1;
      const a = -Math.PI / 2 + (i / 13 - 0.5) * 1.8;
      const dist = ph * 26 * k * big;

      ctx.fillStyle = "rgba(230,245,255," + (1 - ph) + ")";
      ctx.beginPath();
      ctx.arc(endX + Math.cos(a) * dist, endY + Math.sin(a) * dist + ph * ph * 18 * k * big, 1.6 * k, 0, TAU);
      ctx.fill();
    }
  }

  // ================= TIP LIGHT =================
  const glowR = (s.state === "strike" ? 34 : 22) * k;

  g = ctx.createRadialGradient(tip[0], tip[1], 0, tip[0], tip[1], glowR);
  g.addColorStop(0, "rgba(" + rgb + "," + (0.9 * flash) + ")");
  g.addColorStop(0.3, "rgba(" + rgb + "," + (0.35 * flash) + ")");
  g.addColorStop(1, "rgba(" + rgb + ",0)");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(tip[0], tip[1], glowR, 0, TAU);
  ctx.fill();

  ctx.fillStyle = "#ffffff";
  ctx.beginPath();
  ctx.arc(tip[0], tip[1], 2.2 * k, 0, TAU);
  ctx.fill();

  if(s.knock > 0.15)
  {
    ctx.strokeStyle = "rgba(34,211,255," + (0.8 * s.knock) + ")";
    ctx.lineWidth = 1.5 * k;

    for(const r of [12, 19, 26])
    {
      ctx.beginPath();
      ctx.arc(tip[0], tip[1], r * k * (1.2 - s.knock * 0.2), -2.7, -0.4);
      ctx.stroke();
    }
  }

  // ================= VIGNETTE (+ red pulse on a strike) =================
  g = ctx.createRadialGradient(w / 2, h / 2, Math.min(w, h) * 0.35, w / 2, h / 2, Math.max(w, h) * 0.75);
  g.addColorStop(0, "rgba(0,0,0,0)");
  g.addColorStop(1, s.state === "strike" ? "rgba(255,40,60," + (0.35 * flash) + ")" : "rgba(0,0,0,.45)");
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, w, h);

  // ================= HUD =================
  const kh = Math.max(0.8, k);               // readable on a phone too
  const pw = Math.min(300 * kh, w - 24), ph = 96 * kh;
  const px = w - pw - 12, py = w < 600 ? h - ph - 12 : 12;     // phone: over the sand

  ctx.fillStyle = "rgba(4,10,24,.62)";
  ctx.strokeStyle = "rgba(" + rgb + ",.45)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.roundRect(px, py, pw, ph, 10 * kh);
  ctx.fill();
  ctx.stroke();

  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.font = "bold " + Math.round(15 * kh) + "px Arial";
  ctx.fillStyle = colour;
  ctx.fillText(s.label, px + 12 * kh, py + 10 * kh);

  ctx.font = Math.round(11 * kh) + "px Arial";
  ctx.fillStyle = "#8ea6c8";
  ctx.fillText("tip " + (s.tilt >= 0 ? "+" : "") + s.tilt.toFixed(1) + "°" + (s.dir ? "  (+ toward water)" : "  (direction not learned)"), px + 12 * kh, py + 30 * kh);

  // bend gauge: -4° .. +6°, 0 = normal, orange tick = pull allow
  const gx0 = px + 12 * kh, gx1 = px + pw - 12 * kh, gy = py + 56 * kh;
  const gpos = v => gx0 + (Math.max(-4, Math.min(6, v)) + 4) / 10 * (gx1 - gx0);

  ctx.fillStyle = "rgba(255,255,255,.08)";
  ctx.beginPath();
  ctx.roundRect(gx0, gy - 3 * kh, gx1 - gx0, 6 * kh, 3 * kh);
  ctx.fill();

  g = ctx.createLinearGradient(gpos(0), 0, gpos(s.tilt), 0);
  g.addColorStop(0, "rgba(" + rgb + ",.25)");
  g.addColorStop(1, "rgba(" + rgb + ",1)");
  ctx.fillStyle = g;
  ctx.fillRect(Math.min(gpos(0), gpos(s.tilt)), gy - 3 * kh, Math.abs(gpos(s.tilt) - gpos(0)), 6 * kh);

  ctx.fillStyle = "#eaf3ff";
  ctx.fillRect(gpos(0) - 0.75, gy - 6 * kh, 1.5, 12 * kh);

  if(s.allow > 0)
  {
    ctx.fillStyle = "#ffba52";
    ctx.fillRect(gpos(s.allow) - 1, gy - 6 * kh, 2, 12 * kh);
  }

  ctx.fillStyle = "#ffffff";
  ctx.beginPath();
  ctx.arc(gpos(s.tilt), gy, 4.5 * kh, 0, TAU);
  ctx.fill();

  ctx.font = Math.round(9 * kh) + "px Arial";
  ctx.fillStyle = "#5f7799";
  ctx.textAlign = "left";
  ctx.fillText("back", gx0, gy + 9 * kh);
  ctx.textAlign = "center";
  ctx.fillText("normal", gpos(0), gy + 9 * kh);
  ctx.textAlign = "right";
  ctx.fillText("toward water", gx1, gy + 9 * kh);

  // slow pull / drop-back mini bars
  const by = py + ph - 12 * kh;
  const bw = (gx1 - gx0 - 10 * kh) / 2;

  for(const [i, name, val] of [[0, "PULL", s.bucket || 0], [1, "DROP", s.drop || 0]])
  {
    const bx = gx0 + i * (bw + 10 * kh);

    ctx.fillStyle = "rgba(255,255,255,.08)";
    ctx.fillRect(bx + 32 * kh, by - 2 * kh, bw - 32 * kh, 4 * kh);
    ctx.fillStyle = val >= 70 ? "#ff4d5e" : val >= 30 ? "#ffba52" : "#22d3ff";
    ctx.fillRect(bx + 32 * kh, by - 2 * kh, (bw - 32 * kh) * Math.min(100, val) / 100, 4 * kh);
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    ctx.fillStyle = "#8ea6c8";
    ctx.fillText(name, bx, by);
  }
}

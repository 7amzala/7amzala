#!/usr/bin/env python3
"""Laarif StrikeSense brand theme for the rod dashboard.

- Colours from the logo: black / navy, electric blue, cyan, silver text
  (red = strike and orange = warning are kept).
- Logo in the header, as the browser icon, and as a short start-up
  splash. The logo (WebP, ~17 KB) is stored in the sketch itself and
  served at /logo.webp (cached by the phone).

    python3 add_brand_theme.py <sketch.ino> <logo.webp>     (edits in place)
"""
import re
import sys

path, logo_path = sys.argv[1], sys.argv[2]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# ---------- colours (only inside the web page) ----------
COLOURS = {
    # accent green -> cyan
    '#4cf294': '#22d3ff', '#45ef8d': '#22d3ff', '#52f298': '#22d3ff', '#49ef92': '#22d3ff',
    '#47ee8d': '#22d3ff', '#48ef91': '#22d3ff', '#40efa1': '#22d3ff',
    '#45e98a': '#1fb6ff', '#001b0c': '#001226', '#3aa269': '#1f8fd6', '#10261a': '#082440',
    # tilt blue -> violet (stays different from the cyan knock line)
    '#59a8ff': '#9b8cff', '#5da8ff': '#9b8cff',
    # surfaces: dark green -> navy
    '#142019': '#0f1d38', '#070d0a': '#050b18', '#0a120e': '#071026', '#101a15': '#0b1630',
    '#1a2a22': '#15274a', '#294036': '#22406e', '#17211b': '#12203d', '#1d2a23': '#13213f',
    '#060b08': '#030814', '#0b1510': '#07122a', '#050906': '#02060f', '#0d1a13': '#081631',
    '#040806': '#01040b', '#22332a': '#1b2b4a',
    '#153a2c': '#0d2a5c', '#09110d': '#040b1c', '#030504': '#000206',
    # greenish greys -> blue greys, silver text
    '#c2cec8': '#c3d2ea', '#60756a': '#5f7799', '#edf5f1': '#eaf3ff', '#52665b': '#4f6588',
    '#82968b': '#8399bb', '#81968a': '#8298ba', '#74887d': '#7189ad', '#7d9186': '#7b90b2',
    '#62766b': '#60779b', '#4f6358': '#4c6286', '#70847a': '#6f86a8', '#74887e': '#7189ad',
    '#5d7066': '#5a7296', '#64776e': '#62799d', '#8fa398': '#8ea6c8',
    # buttons
    '#3e72ed': '#2f6bff', '#3e8cff': '#2f7bff', '#ebefed': '#e8f1ff',
    # LoRa "fair" bars (yellow-green) -> light cyan
    '#c8e86a': '#7fe3ff',
}
RGBA = {
    'rgba(10,17,13,.95)': 'rgba(6,13,30,.92)',
    'rgba(120,180,145,.12)': 'rgba(90,150,255,.16)',
    'rgba(120,180,145,.18)': 'rgba(90,150,255,.22)',
    'rgba(76,242,148,.45)': 'rgba(34,211,255,.45)',
    'rgba(8,14,11,.97)': 'rgba(4,10,24,.97)',
    'rgba(73,239,146,.13)': 'rgba(34,211,255,.13)',
    'rgba(89,168,255,.12)': 'rgba(155,140,255,.12)',
    'rgba(200,210,205,.35)': 'rgba(200,215,240,.35)',
    'rgba(200,210,205,.6)': 'rgba(200,215,240,.6)',
}

i = s.index('R"rawliteral(')
j = s.index(')rawliteral"', i)
page = s[i:j]


def recolour(m):
    c = m.group(0)
    return COLOURS.get(c.lower(), c)


page = re.sub(r'(?<!&)#[0-9a-fA-F]{6}\b', recolour, page)
for a, b in RGBA.items():
    page = page.replace(a, b)
s = s[:i] + page + s[j:]

# ---------- head: title, icon ----------
rep('<title>StrikeSense</title>', '''<title>Laarif StrikeSense</title>

<link rel="icon" href="/logo.webp">''')

# ---------- header: logo + name ----------
rep('''.brand {
  font-size:25px;
  font-weight:900;
  letter-spacing:-.7px;
}''', '''.brand {
  display:flex;
  align-items:center;
  gap:12px;
  font-size:25px;
  font-weight:900;
  letter-spacing:-.7px;
}

.brandLogo {
  width:58px;
  height:58px;
  border-radius:50%;
  flex:0 0 auto;
  box-shadow:0 0 18px rgba(30,150,255,.55), 0 0 0 1px rgba(90,170,255,.35);
}

.brandName b {
  background:linear-gradient(90deg,#1e7bff,#22d3ff);
  -webkit-background-clip:text;
  background-clip:text;
  color:transparent;
}

/* START-UP SPLASH */

#splash {
  position:fixed;
  inset:0;
  z-index:50;
  display:flex;
  align-items:center;
  justify-content:center;
  background:radial-gradient(circle at 50% 45%, #0d2a5c 0, #020611 55%, #000 100%);
  transition:opacity .6s ease;
}

#splash img {
  width:min(78vw, 360px);
  height:auto;
  border-radius:50%;
  box-shadow:0 0 60px rgba(30,150,255,.45);
  animation:splashGlow 1.6s ease-in-out infinite alternate;
}

#splash.hide {
  opacity:0;
  pointer-events:none;
}

@keyframes splashGlow {
  from { box-shadow:0 0 30px rgba(30,150,255,.30); }
  to   { box-shadow:0 0 70px rgba(34,211,255,.60); }
}''')

rep('''<div class="brand">
Laarif StrikeSense
<small>LIVE ROD MONITOR</small>
</div>''', '''<div class="brand">
<img class="brandLogo" src="/logo.webp" alt="">
<div class="brandName">
Laarif <b>StrikeSense</b>
<small>SMART FISHING STRIKE DETECTOR</small>
</div>
</div>''')

rep('''<body>

<div class="app">''', '''<body>

<div id="splash"><img src="/logo.webp" alt="Laarif StrikeSense"></div>

<div class="app">''')

# splash: away after the first data and at least 1.8 s
rep('''resizeCanvas();
pollLoop();''', '''const splashShownAt = Date.now();
let splashDone = false;

function hideSplash()
{
  if(splashDone)
    return;

  splashDone = true;

  setTimeout(() =>
  {
    el("splash").classList.add("hide");
    setTimeout(() => el("splash").remove(), 700);
  }, Math.max(0, 1800 - (Date.now() - splashShownAt)));
}

setTimeout(hideSplash, 6000);   // never stay longer than this

resizeCanvas();
pollLoop();''')

rep('''    el("connState").innerHTML = "&#9679; CONNECTED";
    el("connState").className = "connected";''', '''    el("connState").innerHTML = "&#9679; CONNECTED";
    el("connState").className = "connected";

    hideSplash();''')

# ---------- logo on the web server ----------
data = open(logo_path, 'rb').read()
rows = []
for k in range(0, len(data), 24):
    rows.append('  ' + ','.join(f'0x{b:02x}' for b in data[k:k + 24]))
array = ',\n'.join(rows)

rep('''void handleRoot()
{''', '''void handleLogo();

void handleRoot()
{''')

rep('''  server.on("/", handleRoot);''', '''  server.on("/", handleRoot);
  server.on("/logo.webp", handleLogo);''')

s = s.rstrip('\n') + f'''


// =====================================================
// LOGO (WebP, {len(data)} bytes) - served at /logo.webp
// =====================================================

const uint8_t LOGO_WEBP[] PROGMEM =
{{
{array}
}};

void handleLogo()
{{
  // The phone keeps it for a week, so it is only sent once
  server.sendHeader("Cache-Control", "max-age=604800");
  server.send_P(200, "image/webp", (const char*)LOGO_WEBP, sizeof(LOGO_WEBP));
}}
'''

open(path, 'w').write(s)
print("ok", path, len(data), "bytes of logo")

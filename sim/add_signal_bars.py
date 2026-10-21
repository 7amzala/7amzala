#!/usr/bin/env python3
"""Phone app: modern Bluetooth signal bars in the dashboard header
(4 bars, green / orange / red, + dBm) instead of grey text bars.
The app calls setLink(ready, rssi) every second (MainActivity.java).

    python3 add_signal_bars.py <sketch.ino>      (edits in place)
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


rep('''.connected {
  color:#22d3ff;
}
''', '''.connected {
  color:#22d3ff;
}

#linkInfo {
  display:inline-flex;
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

.sigBars {
  display:inline-flex;
  align-items:flex-end;
  gap:2px;
  height:14px;
}

.sigBars i {
  display:block;
  width:4px;
  border-radius:2px;
  background:#2a3956;
}

.sigBars i:nth-child(1) { height:5px; }
.sigBars i:nth-child(2) { height:8px; }
.sigBars i:nth-child(3) { height:11px; }
.sigBars i:nth-child(4) { height:14px; }

.sigDbm {
  color:#8fa6c9;
  font-size:10px;
  font-weight:600;
}
''')

rep('''<span id="linkInfo">Bluetooth</span>''', '''<span id="linkInfo"><span id="linkName">Bluetooth</span><span class="sigBars" id="sigBars"><i></i><i></i><i></i><i></i></span><span class="sigDbm" id="sigDbm"></span></span>
<script>
// Called by the phone app every second: ready = linked to the rod, rssi = dBm (0 = unknown)
function setLink(ready, rssi)
{
  var n = ready && rssi ? (rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1) : 0;
  var c = n >= 3 ? '#3ddc84' : n == 2 ? '#ffba52' : '#ff4d5e';
  var b = document.querySelectorAll('#sigBars i');

  for(var i = 0; i < b.length; i++)
    b[i].style.background = i < n ? c : '';

  document.getElementById('linkName').textContent = ready ? 'Bluetooth' : 'Reconnecting\\u2026';
  document.getElementById('linkName').style.color = ready ? '' : '#ffba52';
  document.getElementById('sigDbm').textContent = ready && rssi ? rssi + ' dBm' : '';
}
</script>''')

open(path, 'w').write(s)
print("ok", path)

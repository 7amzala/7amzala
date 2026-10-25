# The app's watch line over LoRa: which channel (500 m / 1 km) and how busy
# (Clear / Good / Busy / Congested), and the advice in a crowd.
#
#   python3 sim/add_app_lora_label_ui.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]


def patch(path):
    s = open(path).read()

    if "crowdTip" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep("""  via.textContent = bt ? 'Bluetooth' : lora ? 'LoRa' : 'not connected';""",
        """  var QUAL = ['Clear', 'Good', 'Busy', 'Congested'];
  via.textContent = bt ? 'Bluetooth' : lora ? 'LoRa \\u00b7 ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined ? ' \\u00b7 ' + QUAL[d.lqual] : '') : 'not connected';

  // many rods on the channel: Bluetooth gives the fastest alerts
  var tip = document.getElementById('crowdTip');
  tip.style.display = lora && d.lqual == 3 ? '' : 'none';
  tip.textContent = 'Many devices nearby (' + (d.lrods || 0) + ' rods). For the fastest alerts use Bluetooth only (Settings).';""")

    rep("""<span id="watchInfo"><span>Watch</span><span id="watchVia">&ndash;</span><span class="sigBars" id="wBars"><i></i><i></i><i></i><i></i></span></span>
""", """<span id="watchInfo"><span>Watch</span><span id="watchVia">&ndash;</span><span class="sigBars" id="wBars"><i></i><i></i><i></i><i></i></span></span>
<div id="crowdTip" style="display:none;margin:5px auto 0;max-width:330px;color:#ffba52;font-size:11px;font-weight:700"></div>
""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

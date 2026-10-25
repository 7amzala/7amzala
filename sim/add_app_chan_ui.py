# Settings > LoRa CHANNEL in the app: Auto / 500 m / 1 km A / 1 km B, the
# same choice as on the watch's SETUP page (kept on the rod: /lorachan).
#
#   python3 sim/add_app_chan_ui.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

SECTION = r'''<div class="setHelp" id="lmHelp">&ndash;</div>

<div class="setGroup" id="chGroup">LoRa CHANNEL</div>
<div class="whoSeg" id="chSeg">
<button id="ch_0" onclick="setChan(0)">Auto</button>
<button id="ch_1" onclick="setChan(1)">500 m</button>
<button id="ch_2" onclick="setChan(2)">1 km A</button>
<button id="ch_3" onclick="setChan(3)">1 km B</button>
</div>
<div class="setHelp" id="chHelp">&ndash;</div>

<script>
// rod 2.4: "lchm" (choice), "lchan" (now), "lqual", "lrods" in /data
var CH_HELP = [
  'Starts on 500 m (fast: the ring moves 3-4 times a second), moves to 1 km by itself when the watch is far, back when near.',
  '500 m, fast. Stay within about 500 m of the rod.',
  '1 km, about one update a second. Pick A or B so friends nearby use different ones.',
  '1 km, about one update a second. Pick A or B so friends nearby use different ones.'
];
var chWanted = -1, chWantedAt = 0;

function showChan(d)
{
  var show = d.lchm !== undefined;
  el('chGroup').style.display = el('chSeg').style.display = el('chHelp').style.display = show ? '' : 'none';

  if(!show)
    return;

  var m = chWanted >= 0 && Date.now() - chWantedAt < 3000 ? chWanted : d.lchm;

  for(var k = 0; k < 4; k++)
    el('ch_' + k).className = k == m ? 'on' : '';

  var QUAL = ['Clear', 'Good', 'Busy', 'Congested'];
  el('chHelp').innerText = CH_HELP[m] + '  Now: ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined ? ' · ' + QUAL[d.lqual] : '') + (d.lrods ? ' (' + d.lrods + ' rods near)' : '');
}

async function setChan(k)
{
  chWanted = k;
  chWantedAt = Date.now();

  for(var i = 0; i < 4; i++)
    el('ch_' + i).className = i == k ? 'on' : '';

  try
  {
    await fetch('/lorachan?m=' + ['auto', 'fast', 'long1', 'long2'][k]);
  }
  catch(e)
  {
  }
}
</script>'''


def patch(path):
    s = open(path).read()

    if "function setChan(" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep('<div class="setHelp" id="lmHelp">&ndash;</div>', SECTION)
    rep("""  showLinkMode(d);
""", """  showLinkMode(d);
  showChan(d);
""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

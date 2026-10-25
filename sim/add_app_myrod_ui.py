# Settings > MY ROD: the rod's ID, how many devices are paired, and
# "Forget my devices" (a new phone or watch pairs again with the PIN).
#
#   python3 sim/add_app_myrod_ui.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

SECTION = r'''<div class="setGroup" id="rodGroup">MY ROD</div>
<div class="setHelp" id="rodInfo">&ndash;</div>
<button class="soundButton appOnly" id="forgetBtn" onclick="forgetRod()">FORGET MY DEVICES</button>

<script>
// rod 2.3+: "rodid", "paired", "pairopen" in /data
function showRod(d)
{
  var show = d.rodid !== undefined;
  el('rodGroup').style.display = el('rodInfo').style.display = show ? '' : 'none';

  if(!show)
    return;

  el('rodInfo').innerHTML = '<b style="font-size:15px;color:#eaf3ff">' + d.rodid + ' &#128274;</b><br>' +
    d.paired + ' paired device' + (d.paired == 1 ? '' : 's') + ' (phone, watch) &middot; encrypted &middot; ' +
    (d.pairopen ? '<span style="color:#ffba52">open for pairing now</span>' : 'locked');
}

function forgetRod()
{
  if(!window.StrikeApp || !StrikeApp.forgetRod)
    return;

  if(!confirm('Forget my devices?\n\nThe rod forgets this phone AND the watch. To use them again, pair each one with the PIN on the rod box (in the 3 minutes after switching the rod on).'))
    return;

  closeSettings();
  StrikeApp.forgetRod();
}
</script>

<div class="setGroup">MY SETUPS</div>'''


def patch(path):
    s = open(path).read()

    if "function forgetRod()" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep('<div class="setGroup">MY SETUPS</div>', SECTION)
    rep("""  showLinkMode(d);
""", """  showLinkMode(d);
  showRod(d);
""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

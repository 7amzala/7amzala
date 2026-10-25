# Dashboard for the app with bottom tabs (Rod / My strikes / Map / Settings):
# the WATCH LINK choice (Auto / Bluetooth only / LoRa only) at the top of the
# settings, the header MY STRIKES button goes (the tabs have it), and the
# app is told when the settings close (the Settings tab lights off).
#
#   python3 sim/add_app_tabs_linkmode_ui.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

SECTION = r'''<div class="setGroup" id="lmGroup">WATCH LINK</div>

<div class="whoSeg" id="lmSeg">
<button id="lm_auto" onclick="setLinkMode('auto')">Auto</button>
<button id="lm_bt" onclick="setLinkMode('bt')">Bluetooth only</button>
<button id="lm_lora" onclick="setLinkMode('lora')">LoRa only</button>
</div>
<div class="setHelp" id="lmHelp">&ndash;</div>

<script>
// How the watch talks to the rod (rod 2.3+: "lmode", "haslora" in /data)
var LM_HELP = {
  auto: 'Bluetooth near the rod (smooth live graph), LoRa when far (up to ~1 km). Best for most.',
  bt: 'LoRa off: saves rod and watch battery. Stay within ~30 m of the rod.',
  lora: 'The watch leaves Bluetooth: updates 1-2 times a second, up to ~1 km. If LoRa goes quiet for 1 min the watch tries Bluetooth again.'
};
var lmWanted = '', lmWantedAt = 0;

function showLinkMode(d)
{
  var show = d.lmode !== undefined;
  el('lmGroup').style.display = el('lmSeg').style.display = el('lmHelp').style.display = show ? '' : 'none';

  if(!show)
    return;

  // just tapped: show the choice until the rod confirms it
  var m = lmWanted && Date.now() - lmWantedAt < 3000 ? lmWanted : d.lmode;

  ['auto', 'bt', 'lora'].forEach(function(k)
  {
    var b = el('lm_' + k);
    b.className = k == m ? 'on' : '';
    b.disabled = k != 'auto' && !d.haslora;
    b.style.opacity = b.disabled ? .35 : 1;
  });

  el('lmHelp').innerText = d.haslora ? LM_HELP[m] || ''
    : 'This rod has no LoRa (the rod sketch without LoRa): the watch uses Bluetooth.';
}

async function setLinkMode(m)
{
  lmWanted = m;
  lmWantedAt = Date.now();
  el('lmHelp').innerText = LM_HELP[m];
  ['auto', 'bt', 'lora'].forEach(function(k) { el('lm_' + k).className = k == m ? 'on' : ''; });

  try
  {
    await fetch('/linkmode?m=' + m);
  }
  catch(e)
  {
  }
}
</script>

<div class="setGroup">MY SETUPS</div>'''


def patch(path):
    s = open(path).read()

    if "function setLinkMode(" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep('<div class="setGroup">MY SETUPS</div>', SECTION)

    rep("""<button class="soundButton appOnly" onclick="openStrikes()">&#128205; MY STRIKES</button>
""", "")

    rep("""function showTimes(d)
{
  el("upTime").innerText = clock(d.up);
  showWatch(d);
""", """function showTimes(d)
{
  el("upTime").innerText = clock(d.up);
  showWatch(d);
  showLinkMode(d);
""")

    rep("""function closeSettings()
{
  el("drawer").classList.remove("open");
}""", """function closeSettings()
{
  el("drawer").classList.remove("open");

  if(window.StrikeApp && StrikeApp.settingsClosed)
    StrikeApp.settingsClosed();
}""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

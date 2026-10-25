# Settings > ACCOUNT (phone app only): who is signed in (the strikes go to
# the StrikeSense sheet with this name), SIGN IN / LOG OUT.
#
#   python3 sim/add_app_account_ui.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

SECTION = r'''<button class="soundButton appOnly" id="forgetBtn" onclick="forgetRod()">FORGET MY DEVICES</button>

<div id="accBox" style="display:none">
<div class="setGroup">ACCOUNT</div>
<div class="setHelp" id="accInfo">&ndash;</div>
<button class="soundButton" id="accBtn" onclick="accountBtn()">SIGN IN</button>
</div>

<script>
// the phone app's account (not on the rod)
function showAccount()
{
  if(!window.StrikeApp || !StrikeApp.accountName)
    return;

  var name = StrikeApp.accountName();
  el('accBox').style.display = '';
  el('accInfo').innerHTML = name ? '<b style="font-size:15px;color:#eaf3ff">' + name + '</b><br>strikes and fishing spots saved online automatically'
                                 : 'Not signed in: strikes stay on this phone.';
  el('accBtn').innerText = name ? 'LOG OUT' : 'SIGN IN';
}

function accountBtn()
{
  if(!window.StrikeApp)
    return;

  if(StrikeApp.accountName())
  {
    if(confirm('Log out? Strikes already saved stay online. New strikes stay on this phone until you sign in again.'))
    {
      closeSettings();
      StrikeApp.logOut();
    }
  }
  else
  {
    closeSettings();
    StrikeApp.openAccount();
  }
}
</script>'''


def patch(path):
    s = open(path).read()

    if "function showAccount(" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep('<button class="soundButton appOnly" id="forgetBtn" onclick="forgetRod()">FORGET MY DEVICES</button>', SECTION)
    rep("""function openSettings()
{
  el("drawer").classList.add("open");""", """function openSettings()
{
  el("drawer").classList.add("open");
  showAccount();""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

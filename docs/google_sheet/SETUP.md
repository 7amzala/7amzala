# StrikeSense strikes database: Google Sheet setup (once, ~10 minutes)

Easiest on a computer (Chrome), signed in to **your** Google account.
The testers never do this: they only register in the app.

## 1. Make the sheet
1. Open **https://sheets.new** (a new empty Google Sheet).
2. Top left, click *Untitled spreadsheet* and name it **StrikeSense strikes**.
3. Menu **File → Settings → Time zone**: pick yours (e.g. *(GMT+00:00) London*). The strike
   times themselves come from each tester's phone, in their own local time.

## 2. Paste the code
1. Menu **Extensions → Apps Script**. A new tab opens.
2. Delete everything in the editor (`function myFunction() {...}`).
3. Paste **all** of `Code.gs` (the file I sent you).
4. Optional: to stop strangers registering, put a tester code on line 17, e.g.
   `const INVITE_CODE = 'BEACH2026';` and give it only to your testers.
5. Click **💾 Save** (or Ctrl+S). Name the project **StrikeSense**.

## 3. Publish it as a web app
1. Top right: **Deploy → New deployment**.
2. Click the ⚙ next to *Select type* → **Web app**.
3. Fill in:
   - Description: `StrikeSense`
   - Execute as: **Me**
   - Who has access: **Anyone**  ← important (the app has no Google login)
4. Click **Deploy**.
5. Google asks for permission: **Authorize access** → pick your account →
   *Google hasn't verified this app* → **Advanced** → **Go to StrikeSense (unsafe)** → **Allow**.
   (It says "unsafe" only because it is your own script, not published by Google.)
6. Copy the **Web app URL**. It looks like
   `https://script.google.com/macros/s/AKfy...long...xyz/exec`

## 4. Check it works
Paste the link in a new browser tab. You should see:
`{"ok":true,"app":"StrikeSense","note":"working"}`

## 5. Send me the link
Send me the `/exec` link (and the tester code if you set one). I build it into the app.

---

### What you will see in the sheet
- **Strikes** tab: User · Date · Time · Country · City/beach · 📍 Spot (tap = Google Maps) ·
  Type · Pull · Fight · Knocks · Biggest knock · Sea · Wind · **Graph** (small line in the cell) · Rod.
  The title row has **filter buttons**: e.g. Country → Morocco, or User → one tester.
- **Users** tab: every tester, when they registered, last seen, number of strikes.
  Passwords are never stored (only a scrambled check value).

### Good to know
- If you ever change the code: **Deploy → Manage deployments → ✏ Edit → Version: New version → Deploy**.
  The link stays the same. (A *New deployment* would make a new link.)
- Don't rename the tabs *Strikes* and *Users*. You can add your own tabs, charts and colours.
- Google's free limits are far above 10 testers.

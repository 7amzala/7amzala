// =====================================================
// LAARIF STRIKESENSE - strikes database in Google Sheets
//
// Paste all of this in: Extensions > Apps Script (replace what is there).
// First time: Deploy > New deployment > Web app (Execute as: Me, Who has
// access: Anyone). The app sends every strike here with the username.
//
// UPDATING (keeps the same link, so the app keeps working):
//   Deploy > Manage deployments > pencil (Edit) > Version: New version >
//   Deploy. Google asks once to allow Google Drive (for the catch photos).
//
// Tabs it makes by itself:
//   Strikes - one row per strike: user, date, time, country, city, spot
//             (tap to open the map), type, pull, fight, knocks, sea,
//             a small graph in the cell; the catch: result, REEL IN to
//             FISH OUT time, fish, kg, cm and links to the 3 photos
//             (hook in the mouth, weight, length)
//
// Photos: Google Drive folder "StrikeSense photos", one folder per tester.
// Camera only (no gallery): the app checks each photo is new; the cell and
// the Drive file say when (and where) it was taken.
//   Users   - every registered tester (passwords are never stored,
//             only a scrambled check value)
// =====================================================

// Optional: testers must type this code to register ('' = anyone with the app)
const INVITE_CODE = '';

const STRIKE_COLS = ['User', 'Date', 'Time', 'Country', 'City / beach', 'Spot', 'Type', 'Pull (deg)', 'Fight (m:ss)',
                     'Knocks', 'Biggest knock', 'Sea', 'Wind', 'Graph', 'Tilt data', 'Rod', 'Strike ID', 'Full data',
                     // the catch (added at the end: older rows and the columns above stay where they are)
                     'Result', 'Reel in to out (m:ss)', 'Fish', 'kg', 'cm', 'Photo: hook', 'Photo: weight', 'Photo: length'];
const PHOTO_SLOTS = ['hook', 'weight', 'length'];
const PHOTO_FOLDER = 'StrikeSense photos';
const USER_COLS = ['Username', 'Registered', 'Last seen', 'Strikes', 'Salt', 'Check', 'Token'];

const WAVES = ['flat', 'small waves', 'waves', 'big waves'];
const WINDS = ['calm', 'light wind', 'windy', 'strong wind'];

function doPost(e) {
  const lock = LockService.getScriptLock();
  lock.waitLock(20000);
  try {
    const req = JSON.parse(e.postData.contents);
    switch (req.action) {
      case 'register': return out_(register_(req));
      case 'login':    return out_(login_(req));
      case 'add':      return out_(addStrikes_(req));
      case 'list':     return out_(listStrikes_(req));
      case 'photo':    return out_(addPhoto_(req));
      case 'ping':     return out_({ ok: true });
    }
    return out_({ ok: false, error: 'unknown action' });
  } catch (err) {
    return out_({ ok: false, error: String(err) });
  } finally {
    lock.releaseLock();
  }
}

// Opening the link in a browser: just says it works
function doGet() {
  return out_({ ok: true, app: 'StrikeSense', note: 'working' });
}

// ---------- users ----------

function register_(req) {
  const name = cleanName_(req.username);
  if (!name) return { ok: false, error: 'Username: 3-20 letters or numbers' };
  if (!req.password || String(req.password).length < 6) return { ok: false, error: 'Password: at least 6 characters' };
  if (INVITE_CODE && req.invite !== INVITE_CODE) return { ok: false, error: 'Wrong tester code' };

  const sh = tab_('Users', USER_COLS);
  if (findUser_(sh, name)) return { ok: false, error: 'This username is taken' };

  const salt = Utilities.getUuid();
  const token = Utilities.getUuid();
  sh.appendRow([name, new Date(), new Date(), 0, salt, hash_(salt, req.password), token]);
  return { ok: true, username: name, token: token };
}

function login_(req) {
  const name = cleanName_(req.username);
  const sh = tab_('Users', USER_COLS);
  const u = name && findUser_(sh, name);
  if (!u || hash_(u.row[4], req.password) !== u.row[5]) return { ok: false, error: 'Wrong username or password' };
  sh.getRange(u.at, 3).setValue(new Date());
  return { ok: true, username: name, token: u.row[6] };
}

// the app sends username + token with every strike
function checkToken_(req) {
  const sh = tab_('Users', USER_COLS);
  const u = findUser_(sh, cleanName_(req.username));
  if (!u || !req.token || u.row[6] !== req.token) return null;
  return { sh: sh, at: u.at, name: u.row[0] };
}

function findUser_(sh, name) {
  if (sh.getLastRow() < 2) return null;
  const rows = sh.getRange(2, 1, sh.getLastRow() - 1, USER_COLS.length).getValues();
  for (let i = 0; i < rows.length; i++)
    if (String(rows[i][0]).toLowerCase() === String(name).toLowerCase()) return { at: i + 2, row: rows[i] };
  return null;
}

function cleanName_(n) {
  n = String(n || '').trim();
  return /^[A-Za-z0-9_.-]{3,20}$/.test(n) ? n : '';
}

function hash_(salt, password) {
  const bytes = Utilities.computeDigest(Utilities.DigestAlgorithm.SHA_256, salt + '|' + password, Utilities.Charset.UTF_8);
  return bytes.map(b => ('0' + (b & 255).toString(16)).slice(-2)).join('');
}

// ---------- strikes ----------

function addStrikes_(req) {
  const user = checkToken_(req);
  if (!user) return { ok: false, error: 'Please log in again' };

  const sh = tab_('Strikes', STRIKE_COLS);
  const n = sh.getLastRow() - 1;
  const ids = n > 0 ? sh.getRange(2, 17, n, 1).getValues().map(r => String(r[0])) : [];
  let added = 0;

  (req.strikes || []).forEach(s => {
    const row = strikeRow_(user.name, s);
    const at = ids.indexOf(String(s.uid));
    let r;
    if (at >= 0) {
      r = at + 2;
      sh.getRange(r, 1, 1, row.length).setValues([row]);
    } else {
      sh.appendRow(row);
      r = sh.getLastRow();
      ids.push(String(s.uid));
      added++;
    }
    // the spot: tap to open Google Maps; the graph: a small line in the cell
    if (s.lat !== undefined && s.lat !== '')
      sh.getRange(r, 6).setFormula('=HYPERLINK("https://www.google.com/maps?q=' + s.lat + ',' + s.lon + '","📍 open map")');
    // the photos: tap to open
    // the label: when the photo was taken (camera only, in the app)
    PHOTO_SLOTS.forEach((k, i) => {
      const url = s.photos && s.photos[k];
      const info = s.photoinfo && s.photoinfo[k];
      const at = info && info.local ? String(info.local).slice(11, 16) : k;
      if (url) sh.getRange(r, 24 + i).setFormula('=HYPERLINK("' + String(url).replace(/"/g, '') + '","📷 ' + at.replace(/"/g, '') + '")');
    });
    sh.getRange(r, 14).setFormulaR1C1('=IF(R[0]C[1]="","",SPARKLINE(SPLIT(R[0]C[1],","),{"color","#1e7bff";"linewidth",2}))');
  });

  // the tester's strike count and last seen
  user.sh.getRange(user.at, 3, 1, 2).setValues([[new Date(), countUser_(sh, user.name)]]);
  return { ok: true, added: added };
}

function strikeRow_(name, s) {
  // the phone's local date and time at the beach (Spain, Morocco, UK...)
  const d = new Date(Number(s.time));
  const tz = Session.getScriptTimeZone();
  const local = String(s.local || '');                       // "2026-10-23 06:40" from the app
  const g = s.graphdata || null;
  const tilt = g && g.t ? g.t.map(v => Math.round(v) / 100).join(',') : '';
  return [
    name,
    local.length >= 16 ? local.slice(0, 10) : Utilities.formatDate(d, tz, 'yyyy-MM-dd'),
    local.length >= 16 ? local.slice(11, 16) : Utilities.formatDate(d, tz, 'HH:mm'),
    s.country || '',
    s.place || '',
    '',                                           // spot (formula above)
    s.type || '',
    s.peak !== undefined ? Math.round(s.peak * 10) / 10 : '',
    s.fight !== undefined ? Math.floor(s.fight / 60) + ':' + ('0' + (s.fight % 60)).slice(-2) : '',
    s.knocks !== undefined ? s.knocks : '',
    s.knock !== undefined ? Math.round(s.knock * 10) / 10 : '',
    s.wave >= 0 ? WAVES[s.wave] || '' : '',
    s.wind >= 0 ? WINDS[s.wind] || '' : '',
    '',                                           // graph (formula above)
    tilt,
    s.rod || '',
    String(s.uid),
    JSON.stringify(s),                            // everything, for the app and for tuning the detection
    s.result === 'fish' ? 'FISH OUT' : s.result === 'lost' ? 'lost it' : '',
    s.land ? Math.floor(s.land / 60) + ':' + ('0' + (s.land % 60)).slice(-2) : '',
    s.fish || '',
    s.kg !== undefined && s.kg !== '' ? Number(s.kg) : '',
    s.cm !== undefined && s.cm !== '' ? Number(s.cm) : '',
    '', '', ''                                    // photos (formulas above)
  ];
}

// ---------- catch photos ----------

// One photo (JPEG, made small on the phone) into the tester's Drive folder.
// Answers its link; the app then sends the strike again with the link.
function addPhoto_(req) {
  const user = checkToken_(req);
  if (!user) return { ok: false, error: 'Please log in again' };
  const slot = String(req.slot || '');
  if (PHOTO_SLOTS.indexOf(slot) < 0) return { ok: false, error: 'unknown photo' };
  if (!req.data || String(req.data).length > 8000000) return { ok: false, error: 'photo missing or too big' };

  const uid = String(req.uid || '').replace(/[^A-Za-z0-9_-]/g, '');
  const name = uid + '_' + slot + '.jpg';
  const folder = subFolder_(subFolder_(DriveApp.getRootFolder(), PHOTO_FOLDER), user.name);

  // the same photo taken again: replace it
  const old = folder.getFilesByName(name);
  while (old.hasNext()) old.next().setTrashed(true);

  const blob = Utilities.newBlob(Utilities.base64Decode(req.data), 'image/jpeg', name);
  const file = folder.createFile(blob);
  // when and where it was taken (camera only, checked by the app)
  const t = req.taken || {};
  file.setDescription('StrikeSense ' + user.name + ' · ' + slot + ' · taken ' + (t.local || '?') +
                      (t.lat !== undefined ? ' · ' + t.lat + ',' + t.lon : ''));
  return { ok: true, url: file.getUrl() };
}

function subFolder_(parent, name) {
  const it = parent.getFoldersByName(name);
  return it.hasNext() ? it.next() : parent.createFolder(name);
}

function countUser_(sh, name) {
  const n = sh.getLastRow() - 1;
  if (n < 1) return 0;
  return sh.getRange(2, 1, n, 1).getValues().filter(r => r[0] === name).length;
}

// a tester's own strikes (new phone / app reinstalled)
function listStrikes_(req) {
  const user = checkToken_(req);
  if (!user) return { ok: false, error: 'Please log in again' };
  const sh = tab_('Strikes', STRIKE_COLS);
  const n = sh.getLastRow() - 1;
  if (n < 1) return { ok: true, strikes: [] };
  const rows = sh.getRange(2, 1, n, 18).getValues();
  const list = [];
  rows.forEach(r => {
    if (r[0] !== user.name) return;
    try { const s = JSON.parse(r[17]); s.done = true; list.push(s); } catch (x) {}
  });
  return { ok: true, strikes: list };
}

// ---------- tabs ----------

function tab_(name, cols) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sh = ss.getSheetByName(name);
  // an older sheet: add the new column titles at the end
  if (sh && sh.getLastColumn() < cols.length) {
    if (sh.getMaxColumns() < cols.length) sh.insertColumnsAfter(sh.getMaxColumns(), cols.length - sh.getMaxColumns());
    const from = sh.getLastColumn() + 1;
    sh.getRange(1, from, 1, cols.length - from + 1).setValues([cols.slice(from - 1)])
      .setFontWeight('bold').setBackground('#0b1428').setFontColor('#eaf3ff');
  }
  if (!sh) {
    sh = ss.insertSheet(name);
    sh.appendRow(cols);
    sh.setFrozenRows(1);
    sh.getRange(1, 1, 1, cols.length).setFontWeight('bold').setBackground('#0b1428').setFontColor('#eaf3ff');
    if (name === 'Strikes') {
      sh.setColumnWidth(14, 160);
      sh.hideColumns(15);                         // tilt numbers (the graph uses them)
      sh.hideColumns(17, 2);                      // strike ID, full data
      sh.getRange(1, 1, sh.getMaxRows(), cols.length).createFilter();   // filter buttons on the titles
    }
    if (name === 'Users') sh.hideColumns(5, 3);   // salt, check value, token
  }
  return sh;
}

function out_(o) {
  return ContentService.createTextOutput(JSON.stringify(o)).setMimeType(ContentService.MimeType.JSON);
}

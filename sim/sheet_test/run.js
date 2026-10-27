// The Google Sheet script (docs/google_sheet/Code.gs) on the PC: a fake
// sheet + Drive. Old sheet (18 columns) -> new columns added, a strike with
// the catch, its 3 photos, the list back for a new phone.
//   node sim/sheet_test/run.js
const fs = require('fs');
const path = require('path');
const src = fs.readFileSync(path.join(__dirname, '../../docs/google_sheet/Code.gs'), 'utf8');

function Sheet(name) {
  this.name = name; this.cells = []; this.maxCols = 26; this.formulas = {};
}
Sheet.prototype = {
  getLastRow() { return this.cells.length; },
  getLastColumn() { return this.cells.reduce((m, r) => Math.max(m, r.length), 0); },
  getMaxColumns() { return this.maxCols; },
  getMaxRows() { return 1000; },
  insertColumnsAfter(a, n) { this.maxCols += n; },
  appendRow(row) { if (row.length > this.maxCols) this.maxCols = row.length; this.cells.push(row.slice()); },
  setFrozenRows() {}, setColumnWidth() {}, hideColumns() {},
  getRange(r, c, nr, nc) {
    const sh = this; nr = nr || 1; nc = nc || 1;
    if (c + nc - 1 > sh.maxCols) throw new Error('range outside the sheet: column ' + (c + nc - 1));
    const rg = {
      getValues() { const out = []; for (let i = 0; i < nr; i++) { const row = sh.cells[r - 1 + i] || []; const o = []; for (let j = 0; j < nc; j++) o.push(row[c - 1 + j] === undefined ? '' : row[c - 1 + j]); out.push(o); } return out; },
      setValues(v) { for (let i = 0; i < nr; i++) { while (sh.cells.length < r + i) sh.cells.push([]); for (let j = 0; j < nc; j++) sh.cells[r - 1 + i][c - 1 + j] = v[i][j]; } return rg; },
      setValue(v) { return rg.setValues([[v]]); },
      setFormula(f) { sh.formulas[r + ',' + c] = f; return rg; },
      setFormulaR1C1(f) { return rg.setFormula(f); },
      setFontWeight() { return rg; }, setBackground() { return rg; }, setFontColor() { return rg; }, createFilter() { return rg; }
    };
    return rg;
  }
};
const sheets = {};
const drive = { folders: {}, files: [] };
function Folder(p) { this.p = p; }
Folder.prototype = {
  getFoldersByName(n) { const k = this.p + '/' + n; const has = !!drive.folders[k]; return { hasNext: () => has, next: () => new Folder(k) }; },
  createFolder(n) { drive.folders[this.p + '/' + n] = true; return new Folder(this.p + '/' + n); },
  getFilesByName(n) { const list = drive.files.filter(f => f.dir == this.p && f.name == n && !f.trashed); let i = 0;
    return { hasNext: () => i < list.length, next: () => { const f = list[i++]; return { setTrashed(t) { f.trashed = t; } }; } }; },
  createFile(b) { const f = { dir: this.p, name: b.name, bytes: b.bytes.length, id: drive.files.length + 1 }; drive.files.push(f);
    return { getUrl: () => 'https://drive.google.com/file/d/F' + f.id + '/view', setDescription(d) { f.desc = d; } }; }
};
const g = {
  SpreadsheetApp: { getActiveSpreadsheet: () => ({ getSheetByName: n => sheets[n] || null, insertSheet: n => (sheets[n] = new Sheet(n)) }) },
  DriveApp: { getRootFolder: () => new Folder('') },
  LockService: { getScriptLock: () => ({ waitLock() {}, releaseLock() {} }) },
  ContentService: { createTextOutput: t => ({ setMimeType() { return { text: t }; } }), MimeType: { JSON: 'json' } },
  Session: { getScriptTimeZone: () => 'Europe/Madrid' },
  Utilities: {
    getUuid: () => 'u' + Math.random().toString(16).slice(2),
    computeDigest: (a, t) => Array.from(require('crypto').createHash('sha256').update(t).digest()).map(b => b > 127 ? b - 256 : b),
    DigestAlgorithm: { SHA_256: 1 }, Charset: { UTF_8: 1 },
    formatDate: (d, tz, f) => f == 'HH:mm' ? '10:00' : '2026-10-27',
    base64Decode: s => Buffer.from(s, 'base64'),
    newBlob: (bytes, type, name) => ({ bytes, type, name })
  }
};
const api = new Function(...Object.keys(g), src + '\nreturn { doPost, tab_, STRIKE_COLS };')(...Object.values(g));
const post = o => JSON.parse(api.doPost({ postData: { contents: JSON.stringify(o) } }).text);

let fails = 0;
const check = (ok, what) => { if (!ok) fails++; console.log((ok ? '  ok   ' : '  FAIL ') + what); };

// an older sheet: 18 columns, one strike already in it
const old = sheets.Strikes = new Sheet('Strikes');
old.appendRow(api.STRIKE_COLS.slice(0, 18));
old.appendRow(['ali', '2026-10-20', '06:00', 'Spain', 'X', '', 'SLOW PULL', 2, '0:30', 1, 1, '', '', '', '', 'ROD-1', 'old-1', JSON.stringify({ uid: 'old-1', type: 'SLOW PULL' })]);

const reg = post({ action: 'register', username: 'ali', password: 'secret1' });
check(reg.ok, 'register');
const u = { username: 'ali', token: reg.token };

const strike = { uid: '1761555000-321', time: 1761555000000, local: '2026-10-27 06:50', type: 'SLOW PULL', peak: 3.1, fight: 48, knocks: 2, knock: 1.4,
                 wave: 1, wind: 0, rod: 'ROD-1', done: true, result: 'fish', land: 252, fish: 'Sea bass', kg: '1.8', cm: '52', photos: { hook: '', weight: '' } };
check(post({ action: 'add', ...u, strikes: [strike] }).ok, 'strike with the catch (photos not online yet)');
check(old.cells[0].length == api.STRIKE_COLS.length && old.cells[0][18] == 'Result' && old.cells[0][25] == 'Photo: length', 'older sheet: the 8 new column titles added at the end');
const row = old.cells[2];
check(row[16] == strike.uid && row[18] == 'FISH OUT' && row[19] == '4:12' && row[20] == 'Sea bass' && row[21] == 1.8 && row[22] == 52, 'row: FISH OUT, 4:12, Sea bass, 1.8 kg, 52 cm');

const jpg = Buffer.alloc(250000, 7).toString('base64');
const p1 = post({ action: 'photo', ...u, uid: strike.uid, slot: 'hook', data: jpg });
check(p1.ok && /^https:\/\/drive/.test(p1.url), 'photo: hook saved in Drive, link back');
const taken = { at: 1761556000000, local: '2026-10-27 06:52', lat: 37.82, lon: -0.78 };
const p2 = post({ action: 'photo', ...u, uid: strike.uid, slot: 'hook', data: jpg, taken });
check(/taken 2026-10-27 06:52 · 37.82,-0.78/.test(drive.files[drive.files.length - 1].desc || ''), 'Drive photo says when and where it was taken');
check(drive.files.filter(f => !f.trashed).length == 1, 'the same photo again: replaces the old one');
check(drive.folders['/StrikeSense photos/ali'] === true, 'in Drive: StrikeSense photos / ali');
check(!post({ action: 'photo', ...u, uid: strike.uid, slot: 'selfie', data: jpg }).ok, 'unknown photo name refused');
check(!post({ action: 'photo', username: 'ali', token: 'bad', uid: strike.uid, slot: 'hook', data: jpg }).ok, 'wrong token refused');

strike.photos = { hook: p2.url, weight: '' };
strike.photoinfo = { hook: taken };
check(post({ action: 'add', ...u, strikes: [strike] }).ok, 'strike sent again with the photo link');
check(old.cells.length == 3, 'same row updated (no second row)');
check(/HYPERLINK\(".*drive.*","📷 06:52"\)/.test(old.formulas['3,24'] || ''), 'Photo: hook column = a link to the photo, shows when it was taken');
check(!old.formulas['3,25'], 'no weight photo online yet: empty');

const back = post({ action: 'list', ...u });
const mine = back.strikes.find(s => s.uid == strike.uid);
check(back.ok && mine && mine.result == 'fish' && mine.photos.hook == p2.url && mine.land == 252, 'new phone: the catch comes back with the photo link');
check(back.strikes.some(s => s.uid == 'old-1'), 'new phone: the older strikes too');

// a new sheet (no Strikes tab yet)
delete sheets.Strikes;
check(post({ action: 'add', ...u, strikes: [strike] }).ok && sheets.Strikes.cells[0].length == api.STRIKE_COLS.length, 'new sheet: all columns from the start');

console.log(fails ? `FAIL: ${fails} wrong` : 'PASS: Google Sheet script, catch + photos');
process.exit(fails ? 1 : 0);

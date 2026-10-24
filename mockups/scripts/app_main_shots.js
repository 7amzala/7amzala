const { chromium } = require('playwright');
const inject = (watchOnly) => {
  document.querySelectorAll('[id*=plash],[class*=plash]').forEach(e => e.remove());
  try { setLink(true, -58); } catch (e) {}
  const css = document.createElement('style');
  css.textContent = `
  .newmark{outline:2px dashed #ff5cf0;outline-offset:3px;border-radius:12px;position:relative}
  .newtag{position:absolute;top:-11px;right:-8px;background:#ff5cf0;color:#1a0018;font:900 10px Arial;padding:2px 6px;border-radius:6px;letter-spacing:.5px}
  .wpill{display:inline-flex;align-items:center;gap:6px;margin-top:6px;padding:5px 12px;border-radius:12px;background:rgba(10,18,36,.9);border:1px solid #2a3a5a;font-weight:700;font-size:15px}
  .wbars{display:inline-flex;gap:2px;align-items:flex-end}.wbars i{display:block;width:4px;border-radius:1px;background:#ffba52}
  .segw{display:flex;margin:8px auto 4px;max-width:330px;background:#0b1428;border:1px solid #24324f;border-radius:12px;padding:3px}
  .segw div{flex:1;text-align:center;padding:8px 4px;border-radius:9px;font:700 13px Arial;color:#8ea6c8}
  .segw .on{background:linear-gradient(90deg,#1e7bff,#22d3ff);color:#04121a}
  .segnote{font-size:11.5px;color:#a9bbd6;max-width:330px;margin:0 auto;line-height:1.35}
  .wobanner{background:#3a2a08;border:1px solid #ffba52;color:#ffd38a;border-radius:12px;padding:8px 10px;font:700 12.5px Arial;margin:8px auto;max-width:360px;text-align:left}`;
  document.head.appendChild(css);
  const link = document.getElementById('linkInfo');
  const w = document.createElement('div');
  w.innerHTML = watchOnly
    ? `<span class="wpill" style="opacity:.7">Watch <span style="color:#8ea6c8;font-size:12px">connected to the rod (last seen: LoRa)</span></span>`
    : `<span class="wpill">Watch <span style="color:#ffba52">LoRa</span><span class="wbars"><i style="height:5px"></i><i style="height:8px"></i><i style="height:11px"></i><i style="height:14px"></i></span><span style="color:#8ea6c8;font-size:12px">180 m</span></span>`;
  w.className = 'newmark'; w.style.display = 'inline-block'; w.innerHTML += '<span class="newtag">NEW</span>';
  link.parentNode.insertBefore(w, link.nextSibling);
  link.parentNode.insertBefore(document.createElement('br'), w);
  const sc = document.querySelector('.soundControls');
  const b = document.createElement('button'); b.className = 'soundButton newmark'; b.style.marginLeft = '8px';
  b.innerHTML = '&#128205; MY STRIKES<span class="newtag">NEW</span>'; sc.appendChild(b);
  const seg = document.createElement('div'); seg.className = 'newmark'; seg.style.margin = '10px auto 2px'; seg.style.maxWidth = '350px'; seg.style.padding = '2px 4px 6px';
  seg.innerHTML = `<div class="segw"><div class="${watchOnly ? '' : 'on'}">Phone + watch</div><div class="${watchOnly ? 'on' : ''}">Watch only</div></div>
   <div class="segnote">Watch only saves phone battery and a little rod battery. Live data and alarms are just as fast in both.</div><span class="newtag">NEW</span>`;
  sc.parentNode.insertBefore(seg, sc.nextSibling);
  if (watchOnly) {
    const ban = document.createElement('div'); ban.className = 'wobanner';
    ban.innerHTML = '&#128245; Phone disconnected: WATCH ONLY. No alarms on the phone. The phone checks every 2 min that the watch is still there.';
    seg.parentNode.insertBefore(ban, seg);
    // the phone is off the rod: freeze the page in a "watch only" look
    window.__wo = () => {
      const ms = document.getElementById('mainStatus'); ms.textContent = 'WATCH ONLY'; ms.style.color = '#ffba52';
      const cs = document.getElementById('connState'); cs.innerHTML = '&#9679; PHONE OFF THE ROD'; cs.style.color = '#8ea6c8';
      document.getElementById('linkInfo').style.display = 'none';
      document.querySelectorAll('.card,.panel,section').forEach(e => e.style.opacity = '.35');
    };
    window.__wo(); setInterval(window.__wo, 50);
  }
};
(async () => {
  const b = await chromium.launch();
  for (const wo of [false, true]) {
    const p = await b.newPage({ viewport: { width: 412, height: 860 }, deviceScaleFactor: 2 });
    await p.goto('http://localhost:8821/'); await p.waitForTimeout(3500);
    await p.evaluate(inject, wo); await p.waitForTimeout(1500);
    await p.screenshot({ path: wo ? 'app_wo.png' : 'app_main.png' });
  }
  await b.close();
})();

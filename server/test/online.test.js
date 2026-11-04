// two players on the real page (site/ via wrangler dev): rooms, the host's approval, WebRTC, the set-up talk, both games told to start
const { chromium } = require('playwright');
const URL = process.argv[2] || 'http://localhost:8787/';
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed++; };

async function player(browser, name) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  page.on('console', m => { if (m.type() === 'error') console.log(`[${name} console] ${m.text()}`); });
  page.on('pageerror', e => console.log(`[${name} error] ${e.message}`));
  await page.goto(URL);
  await page.waitForFunction(() => self.uplink !== undefined);              // the page's modules are running
  await page.evaluate(n => {
    uplink.app.gameData = { version: 'test', files: {} }; uplink.app.gameDataState = 'ready';   // stand-in for the streamed game data
    window.started = null;
    uplink.game.start = async o => { window.on = o; window.started = { role: o.net.role, delay: o.net.delay, card: o.card ? o.card.length : null, gfx: o.gfx, ring: !!o.net.inbound }; return true; };
    document.getElementById('on-name').value = n;
  }, name);
  await page.click('#tab-online');                                             // the online panel is a tab
  return page;
}
const stateOf = p => p.evaluate(() => document.getElementById('on-state').textContent);
const waitState = (p, re, ms = 15000) => p.waitForFunction(r => new RegExp(r).test(document.getElementById('on-state').textContent), re.source, { timeout: ms });

(async () => {
  const browser = await chromium.launch({ args: ['--disable-features=WebRtcHideLocalIpsWithMdns'] });
  const host = await player(browser, 'Nemo'), guest = await player(browser, 'Rena');
  check(await host.evaluate(() => self.crossOriginIsolated), 'the page is cross-origin isolated (headers from _headers)');

  // a memory card on the host's side: it must reach the guest byte for byte
  await host.evaluate(async () => { const c = new Uint8Array(131072); for (let i = 0; i < c.length; i++) c[i] = (i * 7 + 3) & 255; c[0] = 0x4D; c[1] = 0x43; await uplink.cardPut(0, c); });
  // private room, joined with the code
  await host.click('#on-create');
  await host.waitForFunction(() => /^NEMO-/.test(document.getElementById('on-roomcode').textContent));
  const code = await host.evaluate(() => document.getElementById('on-roomcode').textContent);
  check(true, `room ${code}`);
  await guest.fill('#on-code', code.slice(5).toLowerCase());
  await guest.click('#on-join');
  await host.waitForFunction(() => /Rena asks to join \(with your code\)/.test(document.getElementById('on-requests').textContent));
  check(await guest.evaluate(() => /Asking the host/.test(document.getElementById('on-state').textContent)), 'joining by code: the guest waits for the host');
  await host.click('#on-requests button:has-text("Accept")');
  await waitState(guest, /Waiting for Nemo to start/);
  check(true, 'guest: connected directly, checks passed, ready');
  await waitState(host, /Rena is ready/);
  check(await host.isEnabled('#on-go'), 'host: Start the game is enabled');
  await host.click('#on-go');
  await host.waitForFunction(() => window.started); await guest.waitForFunction(() => window.started);
  const hs = await host.evaluate(() => window.started), gs = await guest.evaluate(() => window.started);
  check(hs.role === 'host' && gs.role === 'guest', 'both games started, as host and guest');
  check(hs.delay === gs.delay && hs.delay >= 3, `the same input delay (${hs.delay} fields)`);
  check(JSON.stringify(hs.gfx) === JSON.stringify(gs.gfx), `the guest uses the host's graphics (${JSON.stringify(gs.gfx)})`);
  check(hs.card === gs.card, `the same memory card (${hs.card === null ? 'none' : hs.card + ' bytes'})`);
  check(await guest.evaluate(() => { const c = window.on.card; for (let i = 2; i < c.length; i++) if (c[i] !== ((i * 7 + 3) & 255)) return false; return c.length === 131072; }), 'the card arrives byte for byte');
  // the game's records go across the data channel, both ways
  await host.evaluate(() => window.on.onMessage({ type: 'net-out', data: new Int32Array([1, 5, 7, 9, 2, 60, 11, 12]).buffer }));
  await guest.waitForFunction(() => new Int32Array(window.on.net.inbound)[0] === 2, null, { timeout: 5000 });
  check(await guest.evaluate(() => { const r = new Int32Array(window.on.net.inbound); return [r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]].join(); }) === '1,5,7,9,2,60,11,12', 'host -> guest: game records arrive in the guest game\'s ring');
  await guest.evaluate(() => window.on.onMessage({ type: 'net-out', data: new Int32Array([1, 6, 1, 2]).buffer }));
  await host.waitForFunction(() => new Int32Array(window.on.net.inbound)[0] === 1, null, { timeout: 5000 });
  check(true, 'guest -> host as well');
  // the host stops: the guest's game is told (it stops waiting for the host)
  await host.evaluate(() => window.on.onStop());
  await guest.waitForFunction(() => new Int32Array(window.on.net.inbound)[0] >= 3, null, { timeout: 5000 });
  check(await guest.evaluate(() => new Int32Array(window.on.net.inbound)[9] === 3), "the host stopped: the guest's game gets the goodbye");

  // public room: listed, joining asks the host
  const h2 = await player(browser, 'Abyss'), g2 = await player(browser, 'Kei');
  await h2.uncheck('#on-private');
  await h2.click('#on-create');
  await h2.waitForFunction(() => /^NEMO-/.test(document.getElementById('on-roomcode').textContent));
  await g2.click('#on-browse');
  await g2.waitForFunction(() => [...document.querySelectorAll('#on-rooms li')].some(li => /Abyss/.test(li.textContent)));
  check(true, 'the public room is listed');
  await g2.click('#on-rooms li button');
  await h2.waitForFunction(() => /Kei asks to join/.test(document.getElementById('on-requests').textContent));
  check(true, 'the host is asked');
  await h2.click('#on-requests button:has-text("Accept")');
  await waitState(g2, /Waiting for Abyss to start/);
  await waitState(h2, /Kei is ready/);
  check(true, 'accepted: connected and ready');
  const g3 = await player(browser, 'Yuang');
  await g3.click('#on-browse');
  await g3.waitForFunction(() => !document.getElementById('on-rooms-tools').hidden);
  check(!/Abyss/.test(await g3.textContent('#on-rooms')), 'a full room is not listed');
  await g2.click('#on-leave');
  await waitState(h2, /disconnected|left/);
  check(true, 'the guest left: the host is told');
  await g3.click('#on-refresh');
  await g3.waitForFunction(() => /Abyss/.test(document.getElementById('on-rooms').textContent), null, { timeout: 5000 });
  check(true, 'Refresh: the room is open again and listed');

  await browser.close();
  console.log(failed ? `${failed} FAILED` : 'all passed');
  process.exit(failed ? 1 : 0);
})().catch(async e => { console.log('FAIL ' + e.message.split('\n')[0]); process.exit(1); });

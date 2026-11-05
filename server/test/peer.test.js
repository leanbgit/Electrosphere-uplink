// server/test/peer.test.js -- what one player's page accepts from the other's
// (web/page/online.js; security audit H1). Two players on the real page in
// headless Chromium, one of them modified, against a running server:
//   npx wrangler dev   then   node test/peer.test.js [http://localhost:8787/]
// (needs playwright: npm i playwright)
const { chromium } = require('playwright');
const URL = process.argv[2] || 'http://localhost:8787/';
let failed = 0;
const check = (ok, what) => {  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed++; };

async function player(browser, name) {
  const ctx = await browser.newContext(); const page = await ctx.newPage();
  page.on('pageerror', e => console.log(`[${name} error] ${e.message}`));
  // keep a handle on the page's data channel, to send what a modified page could send
  await page.addInitScript(() => {
    const send = RTCDataChannel.prototype.send;
    RTCDataChannel.prototype.send = function (d) { self.__dc = this; return send.call(this, d); };
  });
  await page.goto(URL);
  await page.waitForFunction(() => self.uplink !== undefined);
  await page.evaluate(n => {
    const disc = { size: 700e6 }; uplink.have.disc[1] = { file: disc, src: 'selected' }; uplink.have.disc[2] = { file: disc, src: 'selected' }; uplink.renderReady();   // stand-in for the player's discs
    window.started = null;
    uplink.game.start = async o => { window.on = o; window.started = { role: o.net.role, delay: o.net.delay, card: o.card ? [o.card.length, o.card[2]] : null, gfx: o.gfx }; return true; };
    document.getElementById('on-name').value = n;
  }, name);
  await page.click('#tab-online');
  return page;
}
const stateOf = p => p.evaluate(() => document.getElementById('on-state').textContent);
const waitState = (p, re, ms = 15000) => p.waitForFunction(r => new RegExp(r).test(document.getElementById('on-state').textContent), re.source, { timeout: ms })
  .catch(async e => { console.log(`  waiting for ${re}: state "${await stateOf(p)}"; log: ${await p.evaluate(() => [...document.querySelectorAll('#log > div')].slice(-4).map(d => d.textContent).join(' | '))}`); throw e; });
const card = fill => `(() => { const c = new Uint8Array(131072); c[0] = 0x4D; c[1] = 0x43; c.fill(${fill}, 2); let s = ''; for (let i = 0; i < c.length; i += 0x8000) s += String.fromCharCode(...c.subarray(i, i + 0x8000)); return btoa(s); })()`;
const sleep = ms => new Promise(r => setTimeout(r, ms));

// a room with the host's own card (bytes 0x11), the guest admitted and ready
async function pair(browser, hostName, guestName, opts = {}) {
  const host = await player(browser, hostName), guest = await player(browser, guestName);
  await host.evaluate(async () => { const c = new Uint8Array(131072); c[0] = 0x4D; c[1] = 0x43; c.fill(0x11, 2); await uplink.cardPut(0, c); });
  if (opts.hostBefore) await host.evaluate(opts.hostBefore);
  await host.click('#on-create');
  await host.waitForFunction(() => /^NEMO-/.test(document.getElementById('on-roomcode').textContent));
  await guest.fill('#on-code', await host.evaluate(() => document.getElementById('on-roomcode').textContent));
  await guest.click('#on-join');
  await host.waitForFunction(() => document.querySelector('#on-requests button'));
  await host.click('#on-requests button:has-text("Accept")');
  return { host, guest };
}

(async () => {
  const browser = await chromium.launch({ args: ['--disable-features=WebRtcHideLocalIpsWithMdns'] });

  // ---- a modified guest page tries to give the host its card, then start the host's game
  {
    const { host, guest } = await pair(browser, 'Host', 'Evil');
    await waitState(guest, /Waiting for Host to start/);
    await guest.evaluate(`self.__dc.send(JSON.stringify({ t: 'setup', gfx: { scale: 6, bits: 0 }, card: ${card(0xEE)} }))`);
    await guest.evaluate(() => self.__dc.send(JSON.stringify({ t: 'go', delay: 15 })));
    await sleep(1500);
    check(await host.evaluate(() => window.started === null), "a guest's 'go' does not start the host's game");
    await host.click('#on-go');
    await host.waitForFunction(() => window.started); await guest.waitForFunction(() => window.started);
    const hs = await host.evaluate(() => window.started), gs = await guest.evaluate(() => window.started);
    check(hs.card && hs.card[0] === 131072 && hs.card[1] === 0x11, "the host's game uses the host's own card, not the guest's");
    check(gs.card && gs.card[1] === 0x11, "and so does the guest's");
    check(hs.gfx.scale !== 6 || (await host.evaluate(() => +document.getElementById('gfx').value)) === 6, "the host keeps its own graphics");
    check(hs.delay === gs.delay, `both start with the host's delay (${hs.delay})`);
    await host.context().close(); await guest.context().close();
  }

  // ---- a modified host page sends bad set-ups: the guest refuses each, and never starts
  const bad = [
    ['a card of the wrong size', `({ t: 'setup', gfx: { scale: 0, bits: 0 }, card: btoa('MC' + 'x'.repeat(1000)) })`],
    ['a card that is not base64', `({ t: 'setup', gfx: { scale: 0, bits: 0 }, card: '%%%not base64%%%' })`],
    ['a graphics scale the page does not offer', `({ t: 'setup', gfx: { scale: 99, bits: 0 }, card: null })`],
    ['unknown option bits', `({ t: 'setup', gfx: { scale: 0, bits: 1 << 20 }, card: null })`],
  ];
  for (const [what, msg] of bad) {
    const { host, guest } = await pair(browser, 'Bad', 'Guest', {
      // the host page sends its bad set-up in place of the real one
      hostBefore: `(() => { const send = RTCDataChannel.prototype.send; RTCDataChannel.prototype.send = function (d) {
        if (typeof d === 'string' && JSON.parse(d).t === 'setup') d = JSON.stringify(${msg}); return send.call(this, d); }; })()`,
    });
    await waitState(guest, /invalid/).catch(async e => { console.log(`  [${what}] guest: ${await stateOf(guest)} | host: ${await stateOf(host)}`); throw e; });
    await host.evaluate(() => self.__dc.send(JSON.stringify({ t: 'go', delay: 4 })));
    await sleep(800);
    check(await guest.evaluate(() => window.started === null) && /cannot play|could not use/.test(await stateOf(host)), `the guest refuses ${what}, and does not start`);
    await host.context().close(); await guest.context().close();
  }
  // ---- a valid set-up, but an input delay outside 3-15
  {
    const { host, guest } = await pair(browser, 'Odd', 'Guest');
    await waitState(guest, /Waiting for Odd to start/);
    await host.evaluate(() => self.__dc.send(JSON.stringify({ t: 'go', delay: 100000 })));
    await sleep(800);
    check(await guest.evaluate(() => window.started === null) && /invalid input delay/.test(await stateOf(guest)), 'the guest refuses an input delay outside 3-15');
    await host.context().close(); await guest.context().close();
  }
  // ---- a host without a memory card (a first session): still fine
  {
    const { host, guest } = await pair(browser, 'New', 'Guest', { hostBefore: () => new Promise(r => { const q = indexedDB.open('ac3online'); q.onsuccess = () => { const t = q.result.transaction('cards', 'readwrite'); t.objectStore('cards').delete(0); t.oncomplete = () => { q.result.close(); r(); }; }; }) });
    await waitState(host, /Guest is ready/);
    await host.click('#on-go');
    await guest.waitForFunction(() => window.started);
    check((await guest.evaluate(() => window.started)).card === null, 'a host with no card yet: the guest starts with no card');
    await host.context().close(); await guest.context().close();
  }
  await browser.close();
  console.log(failed ? `${failed} FAILED` : 'all passed');
  process.exit(failed ? 1 : 0);
})().catch(async e => { console.log('FAIL ' + e.message.split('\n')[0]); process.exit(1); });

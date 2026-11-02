// Start a new game via the menus, walk/turn in the level, CPU-profile it, screenshot.
// usage: node ingame.js <url> <out.png> [menuKeys]   menuKeys: comma list, default Space,Enter,Enter,Enter,Enter
const { chromium } = require('playwright-core');
(async () => {
  const [url, out, menuKeys = 'Space,Enter,Enter,Enter,Enter'] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: +(process.env.VW || 1100), height: +(process.env.VH || 760) } });
  const t0 = Date.now(); const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(5);
  page.on('console', m => { const t = m.text(); if (/YIELDSTATS|ERROR|EMINE|voxworld|WARN/.test(t)) console.log(ts(), t.slice(0, 200)); });
  page.on('pageerror', e => console.log(ts(), '[pageerror]', e.message));
  await page.goto(url);
  await page.waitForTimeout(4000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(500);
  const key = async (k) => { await page.keyboard.down(k); await page.waitForTimeout(120); await page.keyboard.up(k); await page.waitForTimeout(900); };
  for (const k of menuKeys.split(',').filter(Boolean)) await key(k);
  console.log(ts(), '--- in level');
  await page.waitForTimeout(2000);
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Profiler.enable');
  await cdp.send('Profiler.setSamplingInterval', { interval: 200 });
  await cdp.send('Profiler.start');
  await page.keyboard.down('ArrowUp'); await page.waitForTimeout(2500); await page.keyboard.up('ArrowUp');
  await page.keyboard.down('ArrowLeft'); await page.waitForTimeout(3000); await page.keyboard.up('ArrowLeft');
  await page.waitForTimeout(1500);
  const { profile } = await cdp.send('Profiler.stop');
  const byId = new Map(profile.nodes.map(n => [n.id, n]));
  const parent = new Map();
  for (const n of profile.nodes) for (const c of (n.children || [])) parent.set(c, n.id);
  const self = new Map(), incl = new Map(); let tot = 0;
  profile.samples.forEach((id, i) => {
    const d = profile.timeDeltas[i] || 0; tot += d;
    const k0 = byId.get(id).callFrame.functionName || '(anon)';
    self.set(k0, (self.get(k0) || 0) + d);
    const seen = new Set(); let cur = id;
    while (cur) { const k = byId.get(cur).callFrame.functionName || '(anon)'; if (!seen.has(k)) { incl.set(k, (incl.get(k) || 0) + d); seen.add(k); } cur = parent.get(cur); }
  });
  const top = (m, n) => [...m].sort((a, b) => b[1] - a[1]).slice(0, n)
    .map(([k, v]) => (100 * v / tot).toFixed(1).padStart(5) + '%  ' + k).join('\n');
  console.log('PROFILE total ' + (tot / 1e6).toFixed(1) + 's');
  console.log('SELF:\n' + top(self, 18));
  console.log('INCL:\n' + top(incl, 25));
  await page.screenshot({ path: out });
  process.exit(0);
})();

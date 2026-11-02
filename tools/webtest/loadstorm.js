// Repro attempt: keys / clicks / resizes while the level is loading. Does loading finish?
const { chromium } = require('playwright-core');
(async () => {
  const [url, out, mode = 'all'] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: +(process.env.VW||1100), height: +(process.env.VH||620) } });
  const t0 = Date.now(); const ts = () => ((Date.now() - t0) / 1000).toFixed(1);
  page.on('console', m => { const t = m.text(); if (/video mode|Cache time|still locked|voxworld: 256|abort|RuntimeError/i.test(t)) console.log(ts(), t.slice(0, 120)); });
  page.on('pageerror', e => console.log(ts(), '[pageerror]', e.message));
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const tap = async (k) => { await page.keyboard.down(k); await page.waitForTimeout(120); await page.keyboard.up(k); await page.waitForTimeout(900); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter']) await tap(k);
  await page.keyboard.down('Enter'); await page.waitForTimeout(100); await page.keyboard.up('Enter');  // starts loading
  console.log(ts(), '--- level load started, storming:', mode);
  for (let i = 0; i < 25; i++) {
    if (mode === 'all' || mode === 'keys') { await page.keyboard.down('w'); await page.waitForTimeout(30); await page.keyboard.up('w'); }
    if (mode === 'all' || mode === 'clicks') await page.mouse.click(400 + i * 7, 300);
    if ((mode === 'all' || mode === 'resize') && i === 3) await page.setViewportSize({ width: +(process.env.RW||1500), height: +(process.env.RH||700) });
    await page.waitForTimeout(60);
  }
  const probe = () => Promise.race([page.evaluate(() => 'ok'), new Promise(r => setTimeout(() => r('BLOCKED'), 3000))]);
  for (let s = 0; s < 6; s++) { await page.waitForTimeout(2000); console.log(ts(), 'main thread:', await probe()); }
  await Promise.race([page.screenshot({ path: out + '.png' }), new Promise(r => setTimeout(r, 4000))]);
  process.exit(0);
})();

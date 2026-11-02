// Play for a while (move, turn, shoot) and log per-second frame cost, to spot things that grow over time.
// usage: node soak.js <url> <seconds>
const { chromium } = require('playwright-core');
(async () => {
  const [url, secs = '150'] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--autoplay-policy=no-user-gesture-required'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  const t0 = Date.now(); const ts = () => ((Date.now() - t0) / 1000).toFixed(0).padStart(4);
  let n = 0;
  page.on('console', m => { const t = m.text(); if (/EMINE fps/.test(t) && (n++ % 10) == 0) console.log(ts(), t); if (/voxworld: col/.test(t)) console.log(ts(), t.slice(t.indexOf('voxworld'))); });
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const tap = async (k, ms = 120) => { await page.keyboard.down(k); await page.waitForTimeout(ms); await page.keyboard.up(k); await page.waitForTimeout(500); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) { await tap(k); await page.waitForTimeout(400); }
  for (const c of 'dnkroz') { await page.keyboard.press(c); await page.waitForTimeout(60); }
  const end = Date.now() + (+secs) * 1000;
  let i = 0;
  while (Date.now() < end) {
    await page.keyboard.down('ArrowUp'); await page.waitForTimeout(700); await page.keyboard.up('ArrowUp');
    await page.keyboard.down(i % 2 ? 'ArrowLeft' : 'ArrowRight'); await page.waitForTimeout(400); await page.keyboard.up(i % 2 ? 'ArrowLeft' : 'ArrowRight');
    await page.mouse.down(); await page.waitForTimeout(60); await page.mouse.up(); await page.waitForTimeout(300);
    i++;
  }
  process.exit(0);
})();

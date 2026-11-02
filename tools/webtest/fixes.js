// Checks: resize follows window, hit puffs on block faces, sky when looking up. usage: node fixes.js <url> <outPrefix>
const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  page.on('console', m => { const t = m.text(); if (/video mode|voxworld:|vwdbg|abort|RuntimeError/i.test(t)) console.log(t.slice(0, 140)); });
  page.on('pageerror', e => console.log('[pageerror]', e.message));
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const tap = async (k, ms = 120) => { await page.keyboard.down(k); await page.waitForTimeout(ms); await page.keyboard.up(k); await page.waitForTimeout(500); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) { await tap(k); await page.waitForTimeout(400); }
  await page.waitForFunction(() => true); await page.waitForTimeout(9000);
  await page.screenshot({ path: out + '_start.png' });
  console.log('--- resize to 1600x600 (wider, like going fullscreen)');
  await page.setViewportSize({ width: 1600, height: 600 }); await page.waitForTimeout(1500);
  await page.screenshot({ path: out + '_resized.png' });
  console.log('--- aim down + shoot');
  await tap('End', 700); await page.waitForTimeout(200);
  await page.mouse.down(); await page.waitForTimeout(60); await page.mouse.up();
  for (let i = 0; i < 4; i++) { await page.waitForTimeout(40); await page.screenshot({ path: out + '_shot' + i + '.png' }); }
  console.log('--- look up');
  await tap('Home', 1500); await page.waitForTimeout(300);
  await page.screenshot({ path: out + '_up.png' });
  process.exit(0);
})();

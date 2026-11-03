// Kick test: kick the ground in front until it breaks. usage: node kick.js <url> <outPrefix>
const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  page.on('console', m => { const t = m.text(); if (/abort|RuntimeError|EMDBG/i.test(t)) console.log(t.slice(0, 160)); });
  page.on('pageerror', e => console.log('[pageerror]', e.message));
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const tap = async (k, ms = 120) => { await page.keyboard.down(k); await page.waitForTimeout(ms); await page.keyboard.up(k); await page.waitForTimeout(400); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) { await tap(k); await page.waitForTimeout(500); }
  await page.waitForTimeout(8000);
  for (const c of 'dnkroz') await tap(c, 80);
  await page.waitForTimeout(500);
  await tap('End', 500); await page.waitForTimeout(300);
  await page.screenshot({ path: out + '_0start.png' });
  const fire = async () => { await tap("q", 100); await page.waitForTimeout(500); };
  await fire();
  await page.screenshot({ path: out + '_1damaged.png' });
  for (let i = 0; i < 10; i++) await fire();
  await page.waitForTimeout(150);
  await page.screenshot({ path: out + '_2broken.png' });
  await page.waitForTimeout(1500);
  await page.screenshot({ path: out + '_3after.png' });
  process.exit(0);
})();

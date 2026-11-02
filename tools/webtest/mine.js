// Mining test: shoot the block in front until it breaks, then fire a rocket. usage: node mine.js <url> <outPrefix>
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
  const fire = async () => { await page.mouse.down(); await page.waitForTimeout(60); await page.mouse.up(); await page.waitForTimeout(450); };
  await fire();
  await page.screenshot({ path: out + '_1damaged.png' });
  for (let i = 0; i < 6; i++) await fire();
  await page.waitForTimeout(150);
  await page.screenshot({ path: out + '_2broken.png' });
  await page.waitForTimeout(1500);
  await page.screenshot({ path: out + '_3after.png' });
  // all weapons + rocket launcher, aim down a bit, fire
  for (const c of 'dnstuff') await tap(c, 80);
  await page.waitForTimeout(400);
  await tap('5'); await page.waitForTimeout(1800); await tap('Home', 250); await page.waitForTimeout(300);
  await page.screenshot({ path: out + '_3after.png' });
  await page.mouse.down(); await page.waitForTimeout(150); await page.mouse.up(); await page.waitForTimeout(700);
  await page.screenshot({ path: out + '_4boom.png' });
  await page.waitForTimeout(2500);
  await page.screenshot({ path: out + '_5crater.png' });
  process.exit(0);
})();

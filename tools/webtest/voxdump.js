// Dump the block world's debug map via the console command r_voxdump, save it as <out>.ppm.
// usage: node voxdump.js <url> <out>
const { chromium } = require('playwright-core');
const fs = require('fs');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  page.on('console', m => { const t = m.text(); if (/voxworld|r_voxdump|abort|RuntimeError/i.test(t)) console.log(t.slice(0, 200)); });
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const hold = async (k, ms = 120) => { await page.keyboard.down(k); await page.waitForTimeout(ms); await page.keyboard.up(k); await page.waitForTimeout(300); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) { await hold(k); await page.waitForTimeout(500); }
  await page.waitForTimeout(8000);
  await hold('Backquote');
  await page.keyboard.type('r', { delay: 60 }); await page.keyboard.press('Shift+Minus'); await page.keyboard.type('voxdump /voxmap.ppm', { delay: 60 });
  await page.screenshot({ path: out + '_console.png' });
  await hold('Enter');
  await page.waitForTimeout(1000);
  const data = await page.evaluate(() => Array.from(Module.FS.readFile('/voxmap.ppm')));
  fs.writeFileSync(out + '.ppm', Buffer.from(data));
  console.log('saved', out + '.ppm', data.length);
  process.exit(0);
})();

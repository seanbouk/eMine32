// In-level: resize the window and screenshot every ~50ms to catch transient glitches. usage: node resizeburst.js <url> <outPrefix>
const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  page.on('console', m => { const t = m.text(); if (/video mode|eMine32 build/i.test(t)) console.log(t.slice(0, 120)); });
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const tap = async (k) => { await page.keyboard.down(k); await page.waitForTimeout(120); await page.keyboard.up(k); await page.waitForTimeout(900); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) await tap(k);
  await page.waitForTimeout(9000);
  await page.setViewportSize({ width: 1500, height: 700 });
  for (let i = 0; i < 12; i++) { await page.screenshot({ path: `${out}_${i}.png` }); await page.waitForTimeout(40); }
  process.exit(0);
})();

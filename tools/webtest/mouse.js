// Repro: click around before the level, start it, then move the mouse (no Esc). Does the view turn?
const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(2500);
  for (let i = 0; i < 4; i++) { await page.mouse.click(500 + i * 20, 300); await page.waitForTimeout(150); }
  if (process.env.BLUR) { await page.evaluate(() => window.dispatchEvent(new FocusEvent('blur'))); console.log('sent blur'); await page.waitForTimeout(300); }
  const tap = async (k) => { await page.keyboard.down(k); await page.waitForTimeout(120); await page.keyboard.up(k); await page.waitForTimeout(900); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) await tap(k);
  await page.waitForTimeout(8000);
  const state = await page.evaluate(() => ({ locked: document.pointerLockElement === document.getElementById('canvas') }));
  console.log('before move: pointer locked =', state.locked);
  await page.screenshot({ path: out + '_a.png' });
  for (let i = 0; i < 20; i++) { await page.mouse.move(550 + i * 15, 310); await page.waitForTimeout(30); }
  await page.waitForTimeout(400);
  await page.screenshot({ path: out + '_b.png' });
  const state2 = await page.evaluate(() => ({ locked: document.pointerLockElement === document.getElementById('canvas') }));
  console.log('after move: pointer locked =', state2.locked);
  process.exit(0);
})();

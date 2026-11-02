// Enter the level, then take screenshots while turning / looking around. usage: node tour.js <url> <outPrefix>
const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  page.on('console', m => { const t = m.text(); if (/voxworld|ERROR|abort/i.test(t) && !/0 error/.test(t)) console.log(t.slice(0, 160)); });
  page.on('pageerror', e => console.log('[pageerror]', e.message));
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const tap = async (k, ms = 120) => { await page.keyboard.down(k); await page.waitForTimeout(ms); await page.keyboard.up(k); await page.waitForTimeout(500); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) { await tap(k); await page.waitForTimeout(400); }
  await page.waitForTimeout(1500);
  // god mode so the enemies don't kill the tour: Duke cheat "dnkroz"
  for (const c of 'dnkroz') { await page.keyboard.press(c); await page.waitForTimeout(60); }
  await page.waitForTimeout(500);
  const shots = process.env.SHOTS ? process.env.SHOTS.split(';') : ['0', 'R:700', 'R:700', 'R:700', 'PageDown:600', 'PageUp:1200'];
  let n = 0;
  for (const s of shots) {
    const [k, ms] = s.split(':');
    if (k === 'R') await tap('ArrowRight', +ms);
    else if (k === 'L') await tap('ArrowLeft', +ms);
    else if (k === 'F') await tap('ArrowUp', +ms);
    else if (k !== '0') await tap(k, +ms);
    await page.waitForTimeout(300);
    await page.screenshot({ path: `${out}_${n++}.png` });
  }
  process.exit(0);
})();

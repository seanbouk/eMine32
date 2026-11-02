// Stress-test window resizes (as F11 / dragging the window does) in title, menu and in-game states.
// usage: node resize.js <url> <outPrefix>
const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1720, height: 720 } });
  const t0 = Date.now(); const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(5);
  let errors = 0;
  page.on('console', m => { const t = m.text(); if (/EMINE|ERROR|abort|RuntimeError/i.test(t)) console.log(ts(), t.slice(0, 160)); if (/ERROR|abort|RuntimeError/i.test(t)) errors++; });
  page.on('pageerror', e => { errors++; console.log(ts(), '[pageerror]', e.message); });
  page.on('crash', () => { errors++; console.log(ts(), '[TAB CRASHED]'); });
  const sizes = [[1720, 720], [1100, 760], [3440, 1440], [800, 600], [2560, 1080], [1720, 720]];
  const storm = async (label) => {
    console.log(ts(), '--- resize storm:', label);
    for (let i = 0; i < 24; i++) { const [w, h] = sizes[i % sizes.length]; await page.setViewportSize({ width: w, height: h }); await page.waitForTimeout(37 + (i * 53) % 200); }
    await page.setViewportSize({ width: 1720, height: 720 }); await page.waitForTimeout(1500);
    await page.screenshot({ path: `${out}_${label}.png` });
  };
  const key = async (k) => { await page.keyboard.down(k); await page.waitForTimeout(120); await page.keyboard.up(k); await page.waitForTimeout(900); };
  await page.goto(url);
  await page.waitForTimeout(3000);
  await page.click('#overlay');
  await page.waitForTimeout(600);
  await storm('intro');            // logos / title, before the game loop proper
  await page.click('canvas'); await page.waitForTimeout(300);
  await key('Space');
  await storm('menu');
  for (const k of ['Enter', 'Enter', 'Enter', 'Enter']) await key(k);
  await page.waitForTimeout(1500);
  await page.keyboard.down('ArrowUp');
  await storm('ingame');
  await page.keyboard.up('ArrowUp');
  await page.waitForTimeout(2000);
  console.log('errors:', errors);
  process.exit(0);
})();

// Intro map views: start a new game (the intro is level 1), screenshot the start, then stand at each look-dev camera.
// usage: node intro.js http://localhost:8032/ <outdir> [view ...]   view = name:x:y:ang (plan hundredths, Build angle) or :<console cmd>
//        or +k<key>[@ms] (Home looks up, End down) / +w<ms> / +W<ms> / +S<name> (key, walk forward, wait, screenshot)
const { chromium } = require('playwright-core');
(async () => {
  const [url, out, ...views] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  page.on('console', m => { const t = m.text(); if (/emine_|abort|RuntimeError|error|warn|INTRO|E1L1|map/i.test(t)) console.log(t.slice(0, 240)); });
  page.on('pageerror', e => console.log('[pageerror]', e.message));
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('#canvas'); await page.waitForTimeout(400);
  const hold = async (k, ms = 120) => { await page.keyboard.down(k); await page.waitForTimeout(ms); await page.keyboard.up(k); await page.waitForTimeout(250); };
  const type = async s => { for (const c of s) { if (c === '_') await page.keyboard.press('Shift+Minus'); else await page.keyboard.type(c); await page.waitForTimeout(40); } };
  const con = async cmd => { await hold('Backquote'); await type(cmd); await hold('Enter'); await hold('Backquote'); };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) { await hold(k); await page.waitForTimeout(500); }
  await page.waitForTimeout(1200); await page.screenshot({ path: `${out}/title.png` }); await page.waitForTimeout(5800);
  await page.screenshot({ path: `${out}/start.png` });
  for (const v of views) {
    if (v[0] === ':') { await con(v.slice(1)); await page.waitForTimeout(500); continue; }
    if (v.startsWith('+k')) { const [k, ms] = v.slice(2).split('@'); await hold(k, ms ? +ms : 120); continue; }
    if (v.startsWith('+w')) { await hold('w', +v.slice(2)); continue; }
    if (v.startsWith('+W')) { await page.waitForTimeout(+v.slice(2)); continue; }
    if (v.startsWith('+S')) { await page.screenshot({ path: `${out}/${v.slice(2)}.png` }); continue; }
    const [name, x, y, ang] = v.split(':');
    await con(`emine_tp ${x} ${y} ${ang}`);
    await page.waitForTimeout(900);
    await page.screenshot({ path: `${out}/${name}.png` });
  }
  process.exit(0);
})();

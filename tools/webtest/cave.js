// Cave test: run console commands (e.g. emine_cave to teleport into a cave), then look/walk and screenshot.
// usage: node cave.js <url> <outPrefix> "<console cmd>;<console cmd>" [step...]
//   step: <ms> forward, B<ms> back, L/R<ms> turn, S/D<ms> strafe left/right, U/N<ms> look up/down, J/C<ms> jump/crouch
//         (swim up/down), K<ms> forward+jump, W<ms> wait, Q kick, F fire
const { chromium } = require('playwright-core');
(async () => {
  const [url, out, cmds, ...steps] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
    args: ['--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  page.on('console', m => { const t = m.text(); if (/voxworld|emine_|abort|RuntimeError/i.test(t)) console.log(t.slice(0, 200)); });
  page.on('pageerror', e => console.log('[pageerror]', e.message));
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const hold = async (k, ms = 120) => { await page.keyboard.down(k); await page.waitForTimeout(ms); await page.keyboard.up(k); await page.waitForTimeout(300); };
  // type into the console; SDL wants shifted characters as key presses
  const type = async s => { for (const c of s) { if (c === '_') await page.keyboard.press('Shift+Minus'); else await page.keyboard.type(c); await page.waitForTimeout(40); } };
  for (const k of ['Space', 'Enter', 'Enter', 'Enter', 'Enter']) { await hold(k); await page.waitForTimeout(500); }
  await page.waitForTimeout(8000);
  if (!process.env.NOGOD) for (const c of 'dnkroz') await hold(c, 80);
  for (const cmd of (cmds || '').split(';').filter(Boolean)) {
    await hold('Backquote'); await type(cmd); await hold('Enter'); await hold('Backquote');
  }
  await page.waitForTimeout(800);
  await page.screenshot({ path: out + '_0.png' });
  let i = 1;
  for (const w of steps) {
    const ms = +w.slice(1);
    // EDuke32's default (WASD) bindings
    const keys = { S: 'a', D: 'd', L: 'ArrowLeft', R: 'ArrowRight', U: 'Home', N: 'End', J: 'Space', C: 'ControlLeft', B: 's' };
    if (keys[w[0]]) await hold(keys[w[0]], ms);
    else if (w[0] === 'W') await page.waitForTimeout(ms);
    else if (w[0] === 'K') { await page.keyboard.down('w'); await page.keyboard.down('Space'); await page.waitForTimeout(ms);
                             await page.keyboard.up('Space'); await page.keyboard.up('w'); }
    else if (w === 'Q') await hold('q', 100);
    else if (w === 'F') { await page.mouse.down(); await page.waitForTimeout(60); await page.mouse.up(); }
    else await hold('w', +w);
    await page.waitForTimeout(400);
    await page.screenshot({ path: out + '_' + (i++) + '.png' });
  }
  process.exit(0);
})();

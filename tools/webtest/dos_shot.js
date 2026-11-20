// The page's DOS start screen at 960x720, ready to run (art/page/itch-launch.png: the picture behind itch's Run button).
// usage: node dos_shot.js http://localhost:8032/ <out.png>
const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const b = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true });
  const p = await b.newPage({ viewport: { width: 1600, height: 1000 } });
  await p.goto(url); await p.waitForTimeout(4000);
  await p.evaluate(() => { document.getElementById('build').style.display = 'none'; document.getElementById('cursor').style.animation = 'none'; });
  await p.locator('#game').screenshot({ path: out });
  process.exit(0);
})();

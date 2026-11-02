const { chromium } = require('playwright-core');
(async () => {
  const [url, delay = '12'] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true });
  const page = await browser.newPage();
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Debugger.enable');
  await page.goto(url);
  await new Promise(r => setTimeout(r, +delay * 1000));
  for (let i = 0; i < 3; i++) {
    const paused = new Promise(r => cdp.once('Debugger.paused', r));
    await cdp.send('Debugger.pause');
    const ev = await paused;
    console.log('--- sample', i);
    console.log(ev.callFrames.slice(0, 25).map(f => '  ' + f.functionName).join('\n'));
    await cdp.send('Debugger.resume');
    await new Promise(r => setTimeout(r, 700));
  }
  process.exit(0);
})();

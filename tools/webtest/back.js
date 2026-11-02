const { chromium } = require('playwright-core');
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true });
  const page = await browser.newPage({ viewport: { width: 1100, height: 700 } });
  await page.goto(url); await page.waitForTimeout(3000);
  await page.click('#overlay'); await page.waitForTimeout(3000); await page.click('canvas'); await page.waitForTimeout(400);
  const key = async (k) => { await page.keyboard.down(k); await page.waitForTimeout(120); await page.keyboard.up(k); await page.waitForTimeout(900); };
  await key('Space'); await key('Enter'); await key('Enter');
  await page.screenshot({ path: out + '_before.png' });
  const thumb = async (type) => page.evaluate((t) => window.dispatchEvent(new MouseEvent(t, { button: 3, bubbles: true })), type);
  await thumb('mousedown'); await page.waitForTimeout(150); await thumb('mouseup'); await page.waitForTimeout(900);
  await page.screenshot({ path: out + '_after.png' });
  console.log('url still', page.url());
  process.exit(0);
})();

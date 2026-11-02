# webtest

Drives the web build in your installed Chrome (no browser download) via playwright-core.

- `ingame.js <url> <out.png> [menuKeys]`: clicks through the menus into a level, walks and turns, prints
  `EMINE fps/busy%` lines (page needs `?stats`) and a CPU profile, and saves a screenshot.
- `stack.js <url> [delaySec]`: pauses the page after a delay and prints wasm call stacks. Use it when the
  page hangs (a loop that never yields to the browser).

`npm install` once, then `npm run ingame` with `serve-web.bat` running.

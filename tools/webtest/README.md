# webtest

Drives the web build in your installed Chrome (no browser download) via playwright-core.

- `ingame.js <url> <out.png> [menuKeys]`: clicks through the menus into a level, walks and turns, prints
  `EMINE fps/busy%` lines (page needs `?stats`) and a CPU profile, and saves a screenshot.
- `intro.js <url> <outdir> [name:x:y:ang ...]`: starts a new game (the intro), screenshots the start, then stands at
  each point (`emine_tp`, plan hundredths and Build angle) and screenshots it. `:<cmd>` runs a console command;
  `+k<key>`, `+w<ms>`, `+W<ms>`, `+S<name>` press a key, walk, wait, screenshot. The look-dev views:
  `wall:650:480:51 exit:1980:575:0 glass:3120:850:97 yard:1650:950:868 corridor:3220:520:1004
  truckside:1050:1550:819 mopeds:760:720:865 staff:3000:830:442`; Mine Shift: `mine:3000:2120:512 +ke +W6000 +Safter`.
- `stack.js <url> [delaySec]`: pauses the page after a delay and prints wasm call stacks. Use it when the
  page hangs (a loop that never yields to the browser).

`npm install` once, then `npm run ingame` with `serve-web.bat` running.

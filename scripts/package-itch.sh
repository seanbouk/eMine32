#!/bin/sh
# The itch.io upload: web/out (from scripts/build-web.sh) as a zip with index.html at its root.
# On itch: kind of project HTML, "This file will be played in the browser", embed size 960 x 720,
# tick "Fullscreen button".
set -e
cd "$(dirname "$0")/.."
rm -rf web/itch && mkdir -p web/itch/game
cp web/out/eduke32.js web/out/eduke32.wasm web/out/eduke32.data web/itch/game/
cp web/out/eduke32.html web/itch/game/index.html
(cd web/itch/game && python -c "import zipfile,os;z=zipfile.ZipFile('../shift-happens.zip','w',zipfile.ZIP_DEFLATED);[z.write(f) for f in sorted(os.listdir('.'))]")
# a stand-in for the itch page, to try it as players will: python tools/serve.py 8034 web/itch -> http://localhost:8034/
cat > web/itch/index.html <<'HTML'
<!doctype html><html><head><meta charset="utf-8"><title>itch preview</title>
<style>body{margin:0;background:#2b2b2b;color:#ddd;font:14px sans-serif;display:flex;flex-direction:column;align-items:center;padding:32px}
iframe{border:0;width:960px;height:720px;background:#000} .bar{width:960px;display:flex;justify-content:flex-end;padding:6px 0;background:#1a1a1a}
button{background:#fa5c5c;color:#fff;border:0;padding:6px 12px;font-weight:bold;cursor:pointer}</style></head>
<body><h2>Shift Happens (itch embed preview, 960x720)</h2>
<iframe id="g" src="game/index.html" allow="autoplay; fullscreen *; pointer-lock" allowfullscreen></iframe>
<div class="bar"><button onclick="document.getElementById('g').requestFullscreen()">Fullscreen</button></div>
<p>The text of the page goes here.</p></body></html>
HTML
ls -la web/itch/shift-happens.zip

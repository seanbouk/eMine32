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
ls -la web/itch/shift-happens.zip

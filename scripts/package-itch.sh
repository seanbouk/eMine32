#!/bin/sh
# The itch.io upload: exactly what we test (web/out from scripts/build-web.sh, served by tools/serve.py at
# http://localhost:8032/), zipped with index.html at its root. No separate release copy.
# On itch: kind of project HTML, "This file will be played in the browser", embed size 960 x 760, itch's fullscreen button off (the page has its own).
set -e
cd "$(dirname "$0")/.."
mkdir -p web/itch && rm -f web/itch/shift-happens.zip
(cd web/out && python -c "import zipfile;z=zipfile.ZipFile('../itch/shift-happens.zip','w',zipfile.ZIP_DEFLATED);[z.write(f) for f in ['index.html','eduke32.js','eduke32.wasm','eduke32.data']]")
ls -la web/itch/shift-happens.zip

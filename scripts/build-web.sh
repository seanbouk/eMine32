#!/usr/bin/env bash
# Build the eMine32 engine for the browser (WebAssembly, software renderer).
# Output: web/out/index.html + eduke32.{js,wasm,data}: the page we test is the page we ship (scripts/package-itch.sh)
#   DATA_DIR  - "none" (default): nothing of the original game is packaged, only our own game/ folder. Or a folder
#               of the original game's data to package alongside it, for comparisons (DATA_DIR=devdata/duke3d-sw)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/emenv.sh"
DATA_DIR="${DATA_DIR:-none}"
GAME="$ROOT/game"; command -v cygpath >/dev/null && GAME="$(cygpath -m "$GAME")"
WEB="$ROOT/web"; command -v cygpath >/dev/null && WEB="$(cygpath -m "$WEB")"
PREJS="$WEB/pre.js"
if [ "$DATA_DIR" = none ]; then
    PRELOAD=""
    # no GRP to name on the command line
    mkdir -p "$ROOT/engine/obj-web"
    sed "s/, '-gamegrp', 'DUKE3D.GRP'//; s/'-j', '\/game', //" "$ROOT/web/pre.js" > "$ROOT/engine/obj-web/pre-nogrp.js"
    PREJS="$ROOT/engine/obj-web/pre-nogrp.js"; command -v cygpath >/dev/null && PREJS="$(cygpath -m "$PREJS")"
else
    command -v cygpath >/dev/null && DATA_DIR="$(cygpath -m "$DATA_DIR")"  # emcc (Windows python) needs C:/ paths
    PRELOAD="--preload-file $DATA_DIR@/game"
fi
cd "$ROOT/engine"
rm -f eduke32.html  # always relink so new data is packaged
make -j"$(nproc)" duke3d obj=obj-web PLATFORM=EMSCRIPTEN RELEASE=1 HAVE_VORBIS=1 \
    WEBFLAGS="$PRELOAD --preload-file $GAME@/emine --pre-js $PREJS --shell-file $WEB/shell.html ${EXTRA_WEBFLAGS:-}" "$@"
mkdir -p "$ROOT/web/out"
rm -f "$ROOT/web/out/eduke32.html"
mv -f eduke32.html "$ROOT/web/out/index.html" 2>/dev/null || true
mv -f eduke32.js eduke32.wasm eduke32.data "$ROOT/web/out/" 2>/dev/null || true
rm -f mapster32.html mapster32.js mapster32.wasm mapster32.data
STAMP="$(date +%Y-%m-%d\ %H:%M:%S) $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo +)"
sed -i "s|__EMINE_BUILD__|$STAMP|g" "$ROOT/web/out/index.html"
# the page background's blocks (tools/mkpagetiles.py), inlined so the page stays one file
python "$ROOT/tools/inline_tiles.py" "$ROOT/web/out/index.html"
echo "Built: $ROOT/web/out ($STAMP)"

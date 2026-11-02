#!/usr/bin/env bash
# Build the eMine32 engine for the browser (WebAssembly, software renderer).
# Output: web/out/eduke32.{html,js,wasm,data}
#   DATA_DIR  - folder whose files get packaged as the game data (default: devdata/duke3d-sw)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/emenv.sh"
DATA_DIR="${DATA_DIR:-$ROOT/devdata/duke3d-sw}"
command -v cygpath >/dev/null && DATA_DIR="$(cygpath -m "$DATA_DIR")"  # emcc (Windows python) needs C:/ paths
GAME="$ROOT/game"; command -v cygpath >/dev/null && GAME="$(cygpath -m "$GAME")"
WEB="$ROOT/web"; command -v cygpath >/dev/null && WEB="$(cygpath -m "$WEB")"
cd "$ROOT/engine"
rm -f eduke32.html  # always relink so new data is packaged
make -j"$(nproc)" duke3d obj=obj-web PLATFORM=EMSCRIPTEN RELEASE=1 HAVE_VORBIS=0 \
    WEBFLAGS="--preload-file $DATA_DIR@/game --preload-file $GAME@/emine --pre-js $WEB/pre.js --shell-file $WEB/shell.html" "$@"
mkdir -p "$ROOT/web/out"
mv -f eduke32.html eduke32.js eduke32.wasm eduke32.data "$ROOT/web/out/" 2>/dev/null || true
rm -f mapster32.html mapster32.js mapster32.wasm mapster32.data
echo "Built: $ROOT/web/out"

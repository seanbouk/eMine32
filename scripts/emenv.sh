# Source this to put the Emscripten SDK (installed at ~/emsdk) on PATH in Git Bash.
EMSDK="${EMSDK:-$HOME/emsdk}"
export EMSDK
export EM_CONFIG="$EMSDK/.emscripten"
export PATH="$EMSDK/upstream/emscripten:$EMSDK/upstream/bin:$EMSDK/node/24.19.0_64bit/bin:$EMSDK/python/3.13.3_64bit:$PATH"

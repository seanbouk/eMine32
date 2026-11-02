# Engine changes vs upstream EDuke32

Base: EDuke32 `ec5824db8` (2026-08-07). Every change is tagged `eMine32` in a comment.

## Web build (Emscripten / WebAssembly)

Build: `scripts/build-web.sh` → `web/out/`. Serve: `serve-web.bat`. Add `?stats` to the URL for per-second
timing (`EMINE fps=… busy%=… msPerFrame=…`) in the JS console; `?args=...` appends engine command-line args.

| File | Change |
|---|---|
| `Common.mak` | New `PLATFORM=EMSCRIPTEN`: emcc/em++ drivers (emcc reports itself as clang), software renderer only (no OpenGL/Polymer), no netcode/libvpx/FLAC/mimalloc/pthreads, `.html` output, ASYNCIFY link flags, `WEBFLAGS` hook for data/shell. |
| `source/build/src/timer.cpp` | **Bug fix:** Emscripten defines `CLOCK_MONOTONIC_RAW` but `clock_gettime()` rejects it, leaving garbage time; timer then ran trillions of callbacks per call. This (plus the next fix) is why earlier web ports ran at ~2 fps. Use `CLOCK_MONOTONIC`. |
| `source/build/src/sdlayer.cpp` | **Bug fix:** refresh rate of 0 Hz (Emscripten) → default 60. Yield to the browser after each presented frame and from the event pump (ASYNCIFY); optional `?stats` timing. |
| `source/build/src/baselayer.cpp` | **Bug fix:** `engineFPSLimit` looped forever on a zero frame delay. In the browser, sleep until the next frame instead of busy-waiting. |
| `source/build/src/compat.cpp`, `source/build/include/compat.h` | Emscripten: little-endian detection, `pwd.h` for `getpwuid`. |
| `source/build/src/sdlayer.cpp` | No `execinfo.h` segfault backtraces on Emscripten. |

Measured (headless Chrome, 960×600, 8-bit classic renderer, Duke3D shareware E1L1): locked 60 fps, ~7 ms of work per
frame including game logic, OPL music synthesis and canvas upload. Rendering itself ~1.5 ms/frame.

## Windows (llvm-mingw) build

`scripts/build-native.sh` passes `USE_MIMALLOC=0 HAVE_FLAC=0 STARTUP_WINDOW=0` (mimalloc vs libc++ clash; bundled
libFLAC.a needs the old msvcrt; skip the launcher dialog).

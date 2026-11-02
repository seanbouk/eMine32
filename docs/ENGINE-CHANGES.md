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
| `source/build/src/sdlayer.cpp` | **Bug fix:** the SDL window is not resizable on the web. A resizable window let SDL resize the canvas mid-frame on browser resize / F11, corrupting the image or crashing the tab. The page CSS-scales the canvas instead. |
| `source/build/src/sdlayer.cpp`, `baselayer.cpp` | Frames are paced with `requestAnimationFrame`, so the browser build runs at the monitor's refresh rate (75 Hz, 144 Hz, ...). An explicit `r_maxfps` cap still applies. |
| `source/build/src/sdlayer.cpp`, `web/shell.html` | Render resolution follows the browser window (F11, resizing): the page calls `emine_request_resize()`, applied by `handleevents()` at a safe point via EDuke32's own resize path. |
| `source/build/src/sdlayer.cpp`, `web/shell.html` | **Bug fix:** no mouse at level start until Esc. After any browser blur (clicking the address bar, switching windows) EDuke32's `appactive` went false, and `mouseGrabInput` then recorded the mouse as grabbed without ever enabling SDL relative mode. SDL's Emscripten backend also refuses relative mode without mouse focus. On each key/click the page now calls `emine_mouse_wanted()`, which repairs the grab (focus + relative mode) and the page takes pointer lock. Repro: `tools/webtest/mouse.js` with `BLUR=1`. |
| `source/audiolib/src/driver_sdl.cpp` | Web: 2048-sample audio buffer (was 512, ~11 ms) so long frames don't starve the main-thread mixer (crackling). |
| `source/duke3d/src/screens.cpp` | No "unregistered shareware" overlay. |
| `source/mact/src/control.cpp`, `sdlayer.cpp` | Mouse back (thumb) button backs out of menus, like right-click. SDL's Emscripten backend drops buttons 3/4, so `web/shell.html` forwards them to `emine_mouse_xbutton()` (and stops browser navigation). |
| `source/build/src/compat.cpp`, `source/build/include/compat.h` | Emscripten: little-endian detection, `pwd.h` for `getpwuid`. |
| `source/build/src/sdlayer.cpp` | No `execinfo.h` segfault backtraces on Emscripten. |

Measured after rAF pacing (headless Chrome, 1720×720, in-level): 75 fps (monitor rate), ~2-3 ms of engine work per
frame. Earlier measurement (headless Chrome, 960×600, 8-bit classic renderer, Duke3D shareware E1L1): locked 60 fps, ~7 ms of work per
frame including game logic, OPL music synthesis and canvas upload. Rendering itself ~1.5 ms/frame.

## Block world ("voxworld")

New engine module `source/build/src/voxworld.cpp` + `include/voxworld.h` (eMine32 code, GPL). A map opts in with a
single sector whose `extra` is `0x7E32`; that sector becomes a box of blocks (512 xy units = 8192 z units = one cube,
16x16 texels per face, layer 0 on the sector floor). Seed = sector `hitag`.

| Hook | What |
|---|---|
| `engine.cpp` `engineLoadBoard` | calls `voxworld_onmapload` (generates terrain, puts start sprites on the ground) |
| `engine.cpp` `renderDrawRoomsQ16` | in the block-world sector, `voxworld_drawworld()` replaces Build's wall/floor drawing |
| `engine.cpp` `renderDrawMasks` | face sprites drawn by `voxworld_drawsprites()`, depth-tested against blocks (wall/floor sprites: TODO) |
| `engine.cpp` `cansee` | blocks also block line of sight |
| `clip.cpp` `clipmove` / `getzrange` / `hitscan` | wrapped: blocks collide (same height rules as Build's sector steps), give floor/ceiling heights, and stop hitscans |
| `duke3d/src/premap.cpp` | (dev only) loose map files override the GRP even with shareware data |

Renderer: per screen column, a 2D grid walk (DDA) front to back; block columns are stored as vertical solid runs.
Vertical faces are constant-depth columns (like Build walls), horizontal faces use a per-row reciprocal (like Build
floors), occlusion is a per-column list of open row intervals, depth goes to a float buffer for sprites. Everything is
drawn through Build's palette and shade tables (`palookup`), with Build's own distance-shading formula.
Console: `r_voxdist <blocks>` view distance (default 96), `r_voxstats 1` timing in the log (web: `?stats`).

Measured (headless Chrome, generated hills + trees, 96-block view): 1720x720 ~6 ms block rendering, ~8.5 ms per frame
total, holds 75 Hz; 3440x1440 ~17 ms block rendering, ~36 fps.

## Windows (llvm-mingw) build

`scripts/build-native.sh` passes `USE_MIMALLOC=0 HAVE_FLAC=0 STARTUP_WINDOW=0` (mimalloc vs libc++ clash; bundled
libFLAC.a needs the old msvcrt; skip the launcher dialog).

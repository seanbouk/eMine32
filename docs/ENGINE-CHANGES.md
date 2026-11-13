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

### Rule for page -> engine calls
Exports called from browser event handlers (`emine_mouse_wanted`, `emine_mouse_xbutton`, `emine_request_resize`)
only record requests. The engine may be paused inside an ASYNCIFY sleep when a browser event fires, so doing real
work there (SDL calls, game state) is unsafe; `em_pagerequests()` in `handleevents()` acts on them instead.
Also on the web: the window-derived size overrides the cfg size in `videoSetGameMode` (level start used to switch
back), and `videoShowFrame` still yields while a resize is pending so the page can't freeze.

## Block world ("voxworld")

New engine module `source/build/src/voxworld.cpp` + `include/voxworld.h` (eMine32 code, GPL). A map opts in with a
single sector whose `extra` is `0x7E32`; that sector becomes a box of blocks (512 xy units = 8192 z units = one cube,
16x16 texels per face, layer 0 on the sector floor). Seed = sector `hitag`.

| Hook | What |
|---|---|
| `engine.cpp` `engineLoadBoard` | calls `voxworld_onmapload` (generates terrain, puts start sprites on the ground) |
| `engine.cpp` `renderDrawRoomsQ16` | in the block-world sector, `voxworld_drawworld()` replaces Build's wall/floor drawing |
| `engine.cpp` `renderDrawMasks` | face sprites drawn by `voxworld_drawsprites()`, depth-tested against blocks (wall/floor sprites: TODO) |
| `engine.cpp` `cansee` | blocks also block line of sight; with both ends in the block-world sector only the blocks are tested (Build's `cansee_build` overflows on walls that long and said "can't see" at random) |
| `clip.cpp` `clipmove` / `getzrange` / `hitscan` | wrapped: blocks collide (same height rules as Build's sector steps), give floor/ceiling heights, and stop hitscans |
| `duke3d/src/premap.cpp` | (dev only) loose map files override the GRP even with shareware data |
| `duke3d/src/emine.cpp` (new) | Duke-side destruction: shots chip blocks, explosions blast craters, destroyed blocks throw scrap drawn in the block's texture + smoke + sound |
| `duke3d/src/player.cpp` | player hitscan weapons (pistol/shotgun/chaingun), the kick (`Proj_HandleKnee`) and `A_PostFireHitscan` (enemy fire, custom projectiles) call `Emine_HitscanBlock`; the weapon sprite darkens a little in caves |
| `duke3d/src/osdcmds.cpp` | registers eMine32's console commands (`Emine_RegisterOSD`) |
| `duke3d/src/actors.cpp` | `A_RadiusDamage` calls `Emine_ExplosionBlocks`; scrap debris lands on blocks (`Emine_FloorZ`) |
| `duke3d/src/gameexec.cpp` `A_Execute` | asks `Emine_ActorThink` first: mobs handled in C++ (`emine_mobs.cpp`: zombie, skeleton, spider, creeper on PIGCOP, LIZTROOP, LIZMAN, DRONE) skip their CON code and `VM_Move` |
| `duke3d/src/premap.cpp` `P_ResetWeapons` | also calls `Emine_MobsLevelStart` (clears Duke's map-placed enemies; the mob spawner takes over) |
| `voxworld` `voxworld_lightlevel` | light level 0-15 of a cell, for mob spawning and spider hostility |

Renderer: per screen column, a 2D grid walk (DDA) front to back; block columns are stored as vertical solid runs.
Vertical faces are constant-depth columns (like Build walls), horizontal faces use a per-row reciprocal (like Build
floors), occlusion is a per-column list of open row intervals, depth goes to a float buffer for sprites. Everything is
drawn through Build's palette and shade tables (`palookup`), with Build's own distance-shading formula.
Destruction: per-block damage (hardness per type: leaves 6, sand 10, dirt/grass 12, gravel 14, planks 24, wood 30,
cobble 40, stone 48, coal 52, gold 56, iron 64, bedrock indestructible). Damaged blocks show one of 3 crack stages
(Minecraft-style): the crack masks are tiles `VW_CRACKBASE..+2` (right after the face tiles), shared by every block;
an opaque mask texel darkens the face texel under it by up to 16 shades (black = darkest), and every face is
pre-cracked per stage at load, so drawing costs nothing extra. Without the mask tiles damage just darkens. Sprites with
lotag `0x7E00|type` are drawn as chunks of that block (debris).
World height comes from the sector (floor to ceiling, up to 255 layers); the generated map is 256x256x160. Heights
(default world): lowlands/beaches ~60-64, hills to ~105, ~60 layers of rock below the lowlands. Bedrock is solid at
layer 0 and thins out over layers 1-4 (a lumpy, indestructible floor). Ore depth bands: coal anywhere in the rock,
iron in the lower two thirds, gold in the deepest third.
Generation (spike, in the engine for now): fbm hills, then wandering tunnels (level with the odd one-block step, since
Duke can't step up a whole block) with domed caverns, a few tunnels that stair down from the surface (one always just
ahead of the spawn), ore veins (coal, iron, gold deeper) and gravel pockets, trees. ~40 ms for 256x256x160 in the browser.
Lighting: Minecraft-style skylight 0..15 per air cell (full light falls straight down, otherwise -1 per block),
flood-filled at load, spread incrementally when a cell opens and removed-then-reflooded when one fills (Minecraft's
two-pass removal). A second channel, block light, comes from glowing blocks (`emit`/`tint` in `vw_blockdefs`; so far only the placeholder
atomic ore, green) and carries the emitter's colour. Coloured light is additive: the surface is shaded by its ordinary
(sky/lantern) light, then a tinted shade table built from Build's own (`palookup[0]`) adds the texel's unlit colour times
the light's colour (five strengths, stronger near the source) and matches the result back to the palette. Sprites without their own palette are
tinted too. Each face is shaded by the air cell in front of it, sprites by the cell they stand in. Missing light becomes
extra Build shade (`r_voxdark`, default 20) that fades in with distance (`r_voxlantern`, default 6 blocks), so nearby
walls stay readable in the dark, like a dark Doom/Duke room.
Falling blocks: sand and gravel fall when the cell under them (or beside them, for cave ceilings) opens up. They move
down the grid a cell at a time under gravity (`voxworld_tick`, once per game tic from `G_MoveWorld`), so a falling block
is always an ordinary block. The game gets events through `voxworld_seteventfn`: before a block moves into a cell it
asks whether an actor is there (eMine32 crushes players/enemies, explosion-style damage that grows with fall speed,
and the block bursts into debris), plus a landing event (thud).
Lava: a fluid block (flags `VW_BF_*`: non-solid, fluid, fullbright, burns). Columns keep two run lists, visible runs
(rendering, light, hitscan, sight) and solid runs (collision), so you wade into lava and stand on the floor under it.
Lava draws fullbright with a slowly scrolling texture, gives off orange block light, can't be destroyed, and flows:
every 12 tics fluid next to an opened cell pours down into open space, otherwise spreads sideways one level weaker
(source/pour -> 3 cells). Sources never go away (blocks are only ever removed), so flow never retreats. Generation
floods open cave below the lava line (~layer 10). The game burns players (Duke hurt-floor style: damage, pain sound,
orange flash) and enemies standing in it.
Water: a see-through fluid (`VW_BF_WATER | VW_BF_CLEAR`). Columns keep a third run list, water runs; the opaque run
list no longer includes them. Water doesn't occlude: per screen column the renderer draws the faces between water and
non-water into an overlay (colour + depth) without closing any rows, carries on to whatever is behind, then blends the
overlay over it with Duke's translucency table, by how far you look through the water (clear, half, mostly water,
then just water past 6 blocks). Rows that deep in water are closed early so rays don't trace on to the lake bed.
Sprites behind water get the same blend. With the camera under water the view fades into deep blue in 12 palette steps
(clear to 1.5 blocks, all blue at 24) and is cut off past the fog. Light passes through water (sky light loses 1 per block of water instead of falling
straight down), shots and sight pass through it. Fluids have two kinds with their own queue and pace (lava every 12
tics, 3 cells; water every 5 tics, 4 cells); where they meet, water onto a lava source makes obsidian, onto flowing
lava cobblestone, lava into water stone. Sand and gravel sink through fluids. Generation: a water table at layer
sea-1 fills every hollow below it, and lake noise eases the ground down into basins (sand/gravel beds); caves aren't
carved into anything next to water. The game: Duke's own swimming (`ST_2_UNDERWATER` movement) while his body is in
water, slow sinking with no keys held, jump climbs a bank you're facing for up to 1.5 s after leaving the water, drowning with his head under, no fall
damage into water.
Plants (tall grass, red and yellow flowers): block types drawn as camera-facing billboards a block wide through the
cell's centre (so they never leave their cell, and visiting cells in ray order still draws them front to back). Their
opaque texels go into a per-column overlay that wins over whatever is behind it by depth, so sprites and water sort
against them too. They count as air for collision, light and sight; fluids wash them away, falling blocks crush them,
explosions break them, and one goes when the block under it does. Shots go on through them but smash every plant on
the way to whatever they hit (the hitscan notes the plant cells it crosses; `voxworld_smashplants` is called after gun
and custom hitscan shots, and after a kick within its reach, not after aiming/sight hitscans). Generation puts them on ~30% of open grass:
mostly tall grass, thicker in meadow patches; flowers are rare (~3% of plants), in sparse clumps of one colour.
Glass: a solid block that light (full skylight straight down, like air) and sight pass through; hitscans stop at it
and one shot breaks it (Duke's shards and smash sound). Its faces against anything that isn't glass, near and far
side plus top and bottom, are drawn as cut-outs (frame texels opaque, middle clear) into the same per-column overlay as
the plants, which now depth-tests, so the nearest of plants and glass wins and everything behind still draws. The
test arch has a glass window in its far wall and a half-glass floating slab. `emine_go` takes an optional angle.
Ores (Minecraft's classic set and depth bands, which match our layers): coal anywhere in the rock, iron below ~42,
gold below 32, lapis around 14-30, redstone and diamond below 16; diamond and lapis rare. Redstone glows red (it
replaced the placeholder "atomic" ore and kept its block id 14; lapis is 33, diamond 34). The game: each ore drops
one kind of Duke ammo where it breaks (shots, kicks, and up to 6 per explosion): coal 3 pistol clips, iron 2 boxes of
shells, gold chaingun ammo, redstone pipe bombs, lapis freeze-thrower ammo, diamond rockets (`Emine_OreAmmo` holds the
amounts). A drop pops up out of the middle of the broken block, falls and bounces twice (straight up and down only,
moved by `Emine_MoveDrops`). Duke holds every weapon from the start with one pistol clip and can only pick weapons he has ammo for
(sector.cpp). A weapon's first ammo counts as picking it up: Duke equips it if it ranks above the current weapon in
the weapon preference order (gameexec.cpp), so a pistol clip never downgrades the chaingun. The shareware data has no freeze-thrower art, so its pickup and weapon are invisible until the art
pass. `emine_ore <type>` (dev) drops an ore's ammo in front of you.
Snow caps (on trial): ground at or above the snowline (sea + 55% of the hill range, ~layer 88, ragged by +-3 layers of
noise) is topped with snow instead of grass, so no plants or trees grow there; about 10% of columns near the spawn.
Six layers higher still the top 3 blocks are solid snow blocks (white all round): the highest peaks, ~2% of columns.
The load log reports the snow share and ground-height percentiles. `emine_find <type>` (dev) stands you on the nearest
column topped with a block type (31 = snow, 32 = snow block).
Streaming: the world is as big as the map's box (the default map is 2032x2032 blocks, about Build's coordinate limit
of +-524288); only a 256x256-block window of it (whole 16x16 chunks) is in memory, with the same layout and code as
the old fixed world (window-local block coordinates; `vw_x0/vw_y0` move with the window). `voxworld_stream(x, y)`,
called each tic with the player's position, slides the window by whole chunks once they're more than 1.5 chunks
from its centre (the arrays shift in memory: ~5-7 ms in the browser), saves chunks changed in play as they leave
(run-length coded, in memory for the session) and generates or restores the incoming ones a few per tic (~1 ms
each), nearest first; until then a chunk is solid bedrock, out of sight. Chunks right by the player load at once
(teleports). Generation is per chunk and order-independent: each chunk is built with a 2-block apron in a scratch
buffer from global noise, plus caves and ore/gravel veins seeded per 32-block cell of the world (cave worms are walked
once per cell and kept as carving steps, replayed wherever they reach); trees come from the apron. A new chunk is lit
from its own sky and emitters plus the light of its loaded neighbours; `r_voxlightcheck` (dev) compares that with a
full relight (it matches). Startup builds the whole window: ~240 ms.
Console: `r_voxdist <blocks>` view distance (default 96), `r_voxstats 1` timing in the log (web: `?stats`),
`r_voxdark <0-31>`, `r_voxlantern <blocks>`, `r_voxdump [file]` writes a top-down debug map (PPM: height in grey, caves
red (shallow) to blue (deep), gold in yellow). Game: `emine_cave [maxlight]` (dev) teleports to the nearest dark cave, `emine_glow [minlight]` (dev) teleports next to the nearest glowing block, `emine_drop [height] [type]` (dev) drops a
block on you (type 20 is water), `emine_water [shore]` (dev) drops you into the nearest open water (or stands you
beside it, facing it), `emine_where` (dev) prints the blocks at your eye/body/feet, `emine_go <dx> <dy>` (dev) moves you that many blocks across the world. The test arch's end wall (facing the spawn) is gravel topped with sand.

Measured (headless Chrome, generated hills + trees, 96-block view): 1720x720 ~6 ms block rendering, ~8.5 ms per frame
total, holds 75 Hz; 3440x1440 ~17 ms block rendering, ~36 fps.

## Windows (llvm-mingw) build

`scripts/build-native.sh` passes `USE_MIMALLOC=0 HAVE_FLAC=0 STARTUP_WINDOW=0` (mimalloc vs libc++ clash; bundled
libFLAC.a needs the old msvcrt; skip the launcher dialog).

Projectiles vs blocks: Build moves a projectile across first and then up/down, and the block world's clipmove only
checks the horizontal step at the old height, so a fast rocket going up past an overhang could cut through the corner
of a block. `A_MoveSpriteClipdist` now traces a projectile's real diagonal step through the blocks first
(`Emine_ProjectileBlocks`); a block in the way stops it just short of the face and reports a wall hit.

## Block faces at 32x32, see-through leaves, panorama sky (2026-11-11)

- `voxworld.h`: `VW_TEXSIZE` 32 with `VW_TEXSHIFT` (Build xy units to texels); every texel lookup and the side-face
  texel-to-layer step use them (a hard-coded `>> 4` there made walls show the block above).
- Leaves are drawn like glass: out of the opaque runs, into the cut-out runs (`vw_iscutout`), with each block's own
  texture; faces between leaves are all drawn so a canopy isn't hollow through its holes. Leaf faces are lit by the
  brightest open cell around them (leaves hold no light). They still shade what's under them (`vw_leaftop` in both
  sky-floor checks) and still block sight and shots.
- Sky: a tile 1024+ wide is a panorama, wrapped once around the horizon with square texels (16*vw_ky rows per
  radian); Duke's narrow sky tiles keep the old 4x wrap.
- `tools/mkworld.py` snaps faces to Duke's palette itself, skipping fullbright colours (they never fade with
  distance) and any colour that duplicates one, and paints the night sky (tile 7200).

## Food: melons, mushrooms, block-light colours (2026-11-12)
- New block types appended after `VW_DIAMOND`: `VW_MELON` (solid, hardness 10), `VW_MELON_VINE` and `VW_MUSHROOM`
  (plants). Face tiles 7027-7030 (crack masks moved to 7031-7033). `vw_isplant` is now the `VW_BF_PLANT` flag, not
  an id range, so plants can be added anywhere in the enum.
- Mushrooms give off light 5 in a new blue tint (`VW_TINT_MUSHROOM`). Shots pass mushrooms without smashing them
  (they're eaten by walking into them).
- World generation: melon patches (a vine with melons and more vine on the grass within 2 blocks, about 1 in 700 grass
  columns) and mushroom patches on cave floors between the lava line and 4 below the surface.
- Game side is `duke3d/src/emine_food.cpp`: food pickups (tiles 7150-7153), apples from leaves, melon slices and
  regrowth, mushroom spawning in the dark, and zombie meat (raw, or cooked when an explosion kills it; such deaths
  now burn instead of gibbing). Dev command `emine_food`.

## Adaptive render resolution on the web (2026-11-13)
- Jumpy turning was the frame rate halving: the page presents on each display refresh, so a frame that takes a little
  longer than one refresh waits for the next one (60 to 30 fps). Turning toward open landscape and trees costs more
  per frame than looking at a wall, which tipped heavy views over the line. The renderer's cost goes with the width
  (one ray per column), so fewer pixels helps in proportion.
- `sdlayer.cpp` exports `emine_frame_work_ms()` (the engine's work per presented frame since the last call).
  `web/shell.html` measures the display's refresh with its own requestAnimationFrame loop and checks once a second:
  work above 85% of the refresh for 2 s renders 1.2x fewer pixels per side (down to a third), below 50% for 6 s
  steps back up, but not for a minute after a step down. Load spikes are ignored. `?res=` or `?scale=` turn it off;
  the steps are logged to the JS console ("eMine32 render scale").
- Dev: `emine_viewtrace [frames]` prints, per drawn frame, the game clock, the tic fraction and the view angle.

## Rendering regression from see-through leaves, fixed (2026-11-13)
Bisected with a fixed view at 1720x720 (headless Chrome, `voxworld: columns` timing; spawn view / after a fixed turn):
11-10 7.6 / 10.7 ms, 54e8d36 (32x32 faces) 7.5 / 10.7, 917e566 (see-through leaves) 12.8 / 19.9, everything since the
same. See-through leaves let every ray carry on through each canopy, drawing the front and back of every leaf block
(three light lookups per face) and then the forest behind: in wooded views the renderer doubled, which is what made
turning jumpy (frames over the display's refresh). Fix, in `voxworld.cpp`:
- columns also keep `lruns` (opaque runs counting leaves as opaque) and `ggruns` (glass only); past `r_voxleafdist`
  blocks (default 16), and once a ray has been through one column with leaves, it uses those: leaves are solid
  (drawn with `vw_leafsolid`, the leaf texture with its holes filled by its darkest common colour) and the ray stops
  at them. Near leaves still show sky and branches through their holes.
- Now 9.2 / 13.0 ms (all-solid leaves would be 8.0 / 11.0).
- Play-testing: `emine_perf` (emine_perf.cpp) summarises the last ~1000 frames (gaps, late frames, time per part).

## Mouse input backing up in Chrome, fixed (2026-11-13)
Play-test with `emine_perf mouse` (1000 Hz mouse, Chrome): the page was handed one mousemove per frame, each ~12 ms
later than the last, so movement queued up by hundreds of ms to seconds (turning lagged, then jumped), while clicks
and frames stayed regular. The engine's whole frame ran inside the requestAnimationFrame callback and took most of
a refresh, leaving the browser no room to deliver queued input. `em_wait_animation_frame` (sdlayer.cpp) now only
posts a MessageChannel message from the rAF callback; the frame runs as its own task after it. Cost: a few more
frames miss their refresh when the work is close to the budget (headless 1300x700: 3%).

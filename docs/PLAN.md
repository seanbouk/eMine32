# Plan & decisions

## Priorities
Engine first: blocks + performance + still feeling like a sprite-based Build shooter. Game design comes later.

## Decisions so far
- **Platform**: fork of EDuke32, classic 8-bit software renderer (it's what runs on the web). Native Windows build too.
- **Display**: any resolution and aspect ratio (21:9 is a first-class target). Hi-res screens are fine; the retro look
  comes from low *texel* resolution and the palette, not low screen resolution. Resolution, render scale and FPS cap
  stay player options (period-correct). The browser build paces to the monitor's refresh rate.
- **Block textures**: start at **16×16 texels per face**, with the texel size configurable in the engine so we can try
  32/64 later.
- **Art pipeline**: author art at higher resolution (box-art style renders, not hand-placed pixels) and let a build
  script scale it down and quantise it to the palette. Applies to sprolygon monsters and to block faces. Duke's art is
  "low-res", not pixel art, so scaled-down art should look right.

## Milestones
1. ~~Stock EDuke32 native + web, measured~~ (done: 60+ fps in browser, ~2-3 ms/frame at 1720x720)
2. Voxel spike: chunk of blocks rendered in the software renderer with Duke palette/shading/fog, sprites in the same
   world, walkable. Native + web. (**first pass done**: 256x256x64 generated world, sprites depth-tested, block
   collision/hitscan/line-of-sight; needs play-testing + more renderer speed for full-res 21:9)
3. Terrain generation (noise heightmap, caves), chunk streaming. (**caves first pass done**: tunnels + caverns,
   ores, skylight lighting with a "lantern" falloff; falling sand/gravel that crushes; 160-layer world with a lumpy bedrock floor; block light with coloured tints; lava (glows, flows, burns,
   floods the deepest caves); next: water with lakes, decoration blocks (tall grass, flowers), then streaming)
4. Mining: **done** - weapons (incl. kick) chip blocks, explosions blast craters, Duke-style debris; play-tested. No collecting
   or placing (by design: Duke just destroys things).
5. Art pass: our own palette, block textures, sprolygon monsters, HUD; remove all Duke data (see PRELAUNCH-CHECKLIST).

# Backlog

Things we've agreed to do "at some point", not yet scheduled. Newest at the bottom of each section.

## Feel
- **Jump**: Duke's jump feels wrong. Make it feel more like Doom's (low priority). Note: original Doom has no jump at
  all, so decide what "Doom-like" means when we get here (snappier, shorter, less floaty arc, likely tied to block
  height so you can hop exactly one block).

## AI
- Monsters get stuck in pits in the block world (they react to the player fine). Revisit when we do our own AI/monsters.

## Block world
- Sky: replace the wrapped Duke city tile with our own (taller) sky art; maybe a day/night palette.
- Wall- and floor-aligned sprites aren't drawn in the block world yet (decals, some effects).
- Renderer speed for full-res 21:9: spread screen columns across threads (native; web with SharedArrayBuffer).

- Art pass / palette: Duke's palette has few muted greens, so green-tinted light reads olive. Our own palette should
  include ramps for the light colours we use (lava orange, atomic green, water blue). Same for seeing through water:
  Duke's translucency table turns grey + blue into mauve; our palette needs slate blue-greys.
- Fluids are full blocks: flowing water/lava has no lower "spreading" surface yet. Water source rules are simple
  (sources never multiply or drain).
- Water views cost ~2-3x a dry view to render (overlay + what's behind it): ~6-10 ms vs ~3 ms at 1100x620 web.
- Glass (another see-through block) can reuse the water overlay path.

## Web / QoL
- Present via WebGL (8-bit frame as a texture + palette lookup in a shader) instead of SDL's per-pixel JS copy; it's
  ~13% of frame time at 3440x1440.
- Render-scale / resolution / FPS-cap options in the game's own menu (today: `?scale=`, `?res=` URL options and the
  `r_maxfps` cvar). Period-correct: let players choose to run low-res or slow; no automatic dynamic resolution needed.


- **Rare RPG pass-through (unresolved):** a rocket fired high and far into (probably) stone occasionally goes through. Not reproduced headlessly: about 120 test rockets, logging any tic whose movement crossed a solid block, were all clean. Fixed so far: diagonal steps are traced (9fab1f8), and a projectile starting inside a block hits it (fdfc69e). Next lead: record the rocket path when it happens (temp logging patch: check `voxworld_cansee(prev, cur)` after `A_MoveSprite` in the projectile mover in actors.cpp).
- **Death camera:** when the player dies, Duke drops the view to the floor, which in the block world ends up inside a block (screen of streaks). Clamp the dead view above the block floor.
- **Mob behaviour lives in C++** (`emine_mobs.cpp`; A_Execute skips the CON for actors it handles). It still borrows Duke's CON *actions* (APIGWALK etc.) for which tiles to show; when the CON goes, give mobs their own action tables. Zombie still uses the pig cop's sounds.
- **Mob tuning (needs play):** spawn caps (surface 28, cave 16; `emine_spawning caps`), surface waves of 2-3 packs 12-20 s apart (3x as often on the move, mostly ahead, 18-36 blocks out; Duke pace rather than Minecraft), pack sizes and type weights (`pickType`/`packSize` in `emine_mobs.cpp`), damage per skill, creeper blast (radius 3 blocks, 90 at the centre).
- **Mobs not yet in C++:** slime (GREENSLIME, should hop and split) and enderman (OCTABRAIN, neutral until looked at, teleports, moves blocks) still run Duke's CON AI and aren't spawned by the spawner.
- **Food tuning (needs play):** heal amounts and quotes (`foodDefs` in `emine_food.cpp`: slice 5, apple 10, mushroom 8, raw meat 10, cooked 30), apple chance (1 in 10 leaves), melon regrowth (2-3 min, not within 10 blocks), melon patch and mushroom density (`vw_genscratch`), mushroom spawning (every 4 s, 10-28 blocks out, dark floors, 4 blocks apart). Eating sound is Duke's EATFOOD (328) until the audio pass.
- **Arrows only hit the player** (Minecraft's also hit other mobs, which then fight the skeleton).

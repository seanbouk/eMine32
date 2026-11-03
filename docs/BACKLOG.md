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
  include ramps for the light colours we use (lava orange, atomic green, water blue).

## Web / QoL
- Present via WebGL (8-bit frame as a texture + palette lookup in a shader) instead of SDL's per-pixel JS copy; it's
  ~13% of frame time at 3440x1440.
- Render-scale / resolution / FPS-cap options in the game's own menu (today: `?scale=`, `?res=` URL options and the
  `r_maxfps` cvar). Period-correct: let players choose to run low-res or slow; no automatic dynamic resolution needed.


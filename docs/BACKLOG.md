# Backlog

Things we've agreed to do "at some point", not yet scheduled. Newest at the bottom of each section.

## Feel
- **Jump**: Duke's jump feels wrong. Make it feel more like Doom's (low priority). Note: original Doom has no jump at
  all, so decide what "Doom-like" means when we get here (snappier, shorter, less floaty arc, likely tied to block
  height so you can hop exactly one block).

## Web / QoL
- Present via WebGL (8-bit frame as a texture + palette lookup in a shader) instead of SDL's per-pixel JS copy; it's
  ~13% of frame time at 3440x1440.
- Render-scale / resolution / FPS-cap options in the game's own menu (today: `?scale=`, `?res=` URL options and the
  `r_maxfps` cvar). Period-correct: let players choose to run low-res or slow; no automatic dynamic resolution needed.
- Follow browser window size changes by switching resolution at a safe point (today it's picked once at startup and
  the canvas is CSS-scaled; live canvas resizing caused corruption/crashes, see ENGINE-CHANGES).

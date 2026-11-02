# Backlog

Things we've agreed to do "at some point", not yet scheduled. Newest at the bottom of each section.

## Feel
- **Jump**: Duke's jump feels wrong. Make it feel more like Doom's (low priority). Note: original Doom has no jump at
  all, so decide what "Doom-like" means when we get here (snappier, shorter, less floaty arc, likely tied to block
  height so you can hop exactly one block).

## Web / QoL
- Pace frames with `requestAnimationFrame` so the browser build matches the monitor's refresh rate (75 Hz on Sean's
  display) instead of a fixed 60.
- Present via WebGL (8-bit frame as a texture + palette lookup in a shader) instead of SDL's per-pixel JS copy; it's
  ~13% of frame time at 3440x1440.
- Render-scale option in the game's own menu (today it's the `?scale=` / `?res=` URL options), maybe dynamic resolution.
- Resize the render resolution when the browser window changes size (today it's picked once at startup).

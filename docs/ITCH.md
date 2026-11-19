# itch.io page

## Upload

`bash scripts/build-web.sh && bash scripts/package-itch.sh` gives `web/itch/shift-happens.zip` (index.html at its root).
The zip is the build we test at http://localhost:8032/ (tools/serve.py): there is no separate release version.

On itch:
- Kind of project: HTML. Tick "This file will be played in the browser".
- Embed: 960 x 760 (the game, 960 x 720, and the bar under it with our own fullscreen button). Leave itch's "Fullscreen button" off: the page has its own.
- Tick the AI-generated content disclosure (art, voice).
- Submit to Game Off 2026.

## Page text (draft)

**Shift Happens**

*A 90s shooter in a world of blocks.*

It's the graveyard shift at a dead arcade, and one machine is still awake. Hit its big red button and you're
shifted into the mines: an endless world of blocks you can dig, build and blow up, built on the Duke Nukem 3D
engine.

Grab the guns, shoot your way out and get on with it:
- Mine ore for ammo: coal for the pistol, iron for shells, gold for the minigun, and so on.
- Fight zombies, skeletons, spiders and things that hiss.
- Eat what you find. Mind the lava.
- Rockets and pipe bombs take the world apart, block by block.

Plays in the browser with a keyboard and mouse.

**Credits**
- Built on EDuke32 (Ken Silverman's Build engine, 3D Realms' Duke Nukem 3D source, and the EDuke32 team).
- Music: "Absurd Outro", made with Suno.
- Art and voice made with AI tools (fal.ai), then reworked by hand into 8-bit tiles.
- Source code (GPLv2): https://github.com/seanbouk/eMine32

Made for GitHub Game Off 2026. A fan project, not affiliated with 3D Realms, Gearbox or Mojang.

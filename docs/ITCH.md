# itch.io page

## Upload

`bash scripts/build-web.sh && bash scripts/package-itch.sh` gives `web/itch/shift-happens.zip` (index.html at its root).
The zip is the build we test at http://localhost:8032/ (tools/serve.py): there is no separate release version.

On itch:
- Cover image: art/page/cover.png (630 x 500, tools/mkboxart.py).
- Kind of project: HTML. Tick "This file will be played in the browser".
- Embed options: "Click to launch in fullscreen" (itch opens the page filling the browser window: the game near
  the top, the wall of blocks round it). Not a fixed-size embed, and itch's own fullscreen button isn't needed: the
  page has its own (Alt+Enter too).
- Classification: Games (not "Game mods": the button would say "Run mod").
- Embed frame background (behind the Run button): art/page/itch-launch.png (the DOS screen, tools/webtest/dos_shot.js).
- Edit theme (itch's page round it, to match ours): background colour #0C0B0A with art/page/itch-background.png as
  the background image, tiled (tools/mkitchart.py); inner column #161412; text #C8B27A; links and buttons #E8C040.
- Tick the AI-generated content disclosure (art, voice).
- Submit to Game Off 2026.

## Page text

**Shift Happens**

*A 90s shooter in a world of blocks.*

It's the graveyard shift at a dead arcade, and one machine is still awake. Hit its big red button and you're
shifted into the mines: a huge world of blocks you can dig, build and blow up, built on the Duke Nukem 3D
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
- Made by SeanOfEarth.
- Art and voice made with AI tools (fal.ai, Gemini), then reworked into 8-bit tiles.
- Source code (GPLv2): https://github.com/seanbouk/eMine32

Made for GitHub Game Off 2026. A fan project, not affiliated with 3D Realms, Gearbox or Mojang.

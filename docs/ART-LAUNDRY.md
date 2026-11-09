# Asset laundry list

The goal is that eMine32 runs with **no `DUKE3D.GRP` at all**. Everything the game currently pulls from the
shareware GRP (13 ART files, 3 CON, 3 DAT, 6 maps, 7 MIDI, 181 VOC, 1 timbre bank, 1 demo) is either
**replaced** with ours or **cut** so nothing references it.

**Working approach:**
- Swap a handful of items at a time and see what that tells us. Every swap shows what else can simply be removed.
- The licensing chores (section 9) wait until we're through the list.
- Nothing goes public (itch.io / GitHub Pages) until the list is done.

Disposition key:
- **R**: replace with our art.
- **C**: cut the feature, so no art is needed.
- **O**: already ours.
- **?**: undecided.

## 1. World

| ✓ | Asset | Tile(s) | Disp. | Notes |
|---|---|---|---|---|
| [~] | Block faces (27) | 7000–7026 | O | Ours, generated at 16×16. Swap for the user's art at **32×32** (build-time downscale). Faces: grass top/side, dirt, stone, bedrock, sand, log side/top, leaves, planks, cobble, gravel, coal/iron/gold/redstone/lapis/diamond ore, lava, water, obsidian, tall grass, red/yellow flower, glass, snow top/side. |
| [~] | Block damage stages (3 crack masks) | 7027–7029 | O→R | Placeholder in (`crack_1..3.png`, generated). **Art spec:** one mask per stage, same size as block faces, shared by every block, each stage adding to the last. Transparent = untouched; opaque pixels darken the block under them by their darkness (black = darkest, mid-grey = a lighter chip). |
| [ ] | Sky | 89 (Duke city skyline) | R | Parallax panorama. Probably plain sky + clouds, and maybe a sun. |
| [ ] | Bullet holes / blood splats (wall decals) | BULLETHOLE 952, BLOODSPLAT1–4 | C? | Duke only puts these on Build walls, and the block world has none. |

## 2a. Duke enemies to remove or replace

Each one is either cut or becomes one of the mobs in 2b. The mapping doesn't have to be one to one.

| ✓ | Duke enemy | Behaviour | Fate (suggestion) |
|---|---|---|---|
| [~] | LIZTROOP (1680) | Shoots, ducks, jetpack variant | Becomes the skeleton: single-image art in (Duke's pal-22 recolour removed) |
| [~] | PIGCOP (2000) | Shotgun, tanky | Becomes the zombie: walk, stand and fall animated; still shoots and drops Duke's shotgun |
| [~] | DRONE (1880) | Flies at you and explodes | Becomes the creeper: single-image art in; still flies |
| [~] | LIZMAN (2120) | Big jumps, spits, charges | Becomes the spider: single-image art in; dies on its back |
| [~] | OCTABRAIN (1820) | Floats, psychic blast | Becomes the enderman: single-image art in |
| [ ] | COMMANDER (1920) | Floats, fires rockets | Cut, or a ghast-like (it's a Nether mob) |
| [ ] | ROTATEGUN (2360) | Fixed turret | Cut |
| [ ] | RECON (1960) | Flying car with spotlight | Cut |
| [~] | EGG / GREENSLIME (675/2370) | Slime that latches on | Becomes the slime: single-image art in (still latches onto your face) |
| [ ] | BOSS1 Battlelord (2630) | Boss | Cut for now |
| [ ] | Shared gore: blood, jibs, per-enemy gib chunks, burn/freeze/shrink states | | Simplify: a generic gib set + palette swaps |

## 2b. Minecraft mobs to add (in Duke style)

These are common Overworld hostiles; all were in Minecraft from early on unless marked. Our own designs and names will be needed before launch (see 9).

| ✓ | Mob | Spawns | Behaviour notes | Priority |
|---|---|---|---|---|
| [ ] | Zombie | Dark/night, surface + caves | Walks at you, melee, can break doors | 1 |
| [ ] | Skeleton | Dark/night | Ranged (arrows), strafes | 1 |
| [ ] | Creeper | Dark/night | Silent approach, hisses, explodes and **breaks blocks** | 1 (the obvious showpiece in a block world) |
| [ ] | Spider | Dark/night; neutral in daylight | Climbs walls, leaps | 2 |
| [ ] | Cave spider | Caves (mineshaft spawners) | Small, fast, poison | 3 |
| [ ] | Slime | Deep layers in "slime chunks", swamps | Bounces, splits on death | 2 |
| [ ] | Enderman | Dark, rare | Neutral until looked at, teleports, picks up blocks | 3 |
| [ ] | Witch | Dark, rare | Throws potions | 3 |
| [ ] | Silverfish | Hidden inside stone blocks | Swarm when you mine their block | 3 (block-world specific, cheap art) |
| [ ] | Drowned *(added later, 1.13)* | Water | Underwater zombie | 3 |
| [ ] | Phantom *(added later, 1.13)* | Sky at night, if you don't sleep | Swoops down from the sky | 3 |

**Sprite pipeline (`tools/mksprite.py`):**
- Source art lives in `art/` (hi-res, committed).
- `art/sprites.json` maps each source to Build tile ranges. Per group you can set `height` (pixels), `squash` (crouch) and `rotate` (falling over / lying dead).
- The tool cuts the figure out (RGBA, or a plain flat background that it keys out), scales it, and matches colours to the palette in CIELAB so greens stay olive instead of going grey.
- It writes `game/sprites/*.png` and `game/sprites.def`.
- A source can be a turnaround sheet. `views: 5` splits it into front, ¾ front, side, ¾ back and back. `mirror` flips views drawn facing right, because Duke's turned views face left. Groups with `per_view` fill Duke's frame×5+view tile layout.
- An entry can have several named `sheets` (a turnaround, one 4-frame walk sheet per view, a fall sheet). Groups then pick panels by reference: `frames` is one row of 5 views per frame, `images` is one per tile, and `side:2` means panel 2 of sheet "side", with a trailing `~` to mirror that panel. Figures are found as separate islands, so they can overlap each other's columns.
- Gemini walk sheets tend to lift the same leg in every front and back frame; fix it in `frames` by mirroring or reusing panels (see the zombie).
- Dev commands: `emine_spawn <tile> [blocks]` puts an actor in front of you; `emine_list [blocks]` lists nearby enemies.
- Status: zombie, skeleton, spider, creeper, enderman and slime are all in. The zombie has a 4-frame walk in 5 views, a standing turnaround for shoot/stand, and a 5-frame fall plus dead body (all Gemini); the rest are single front images. Behaviour is still Duke's.

**Art spec per mob (Duke convention):**
- 5 view angles: front, front-¾, side, back-¾, back. The engine mirrors these for the other 3.
- Per angle: idle 1, walk 4, attack 1–2, pain 1. That's about 40 images.
- A death sequence of about 5 frames, front only.

Each mob also needs spawn rules and Minecraft-style navigation.

## 3. Wildlife (roadmap step 3)

| ✓ | Asset | Disp. | Notes |
|---|---|---|---|
| [ ] | 2–3 passive animals | R | Same sprite spec as mobs, minus attack frames. |
| [ ] | Raw and cooked meat pickups | R | |

## 4. Player weapons (first-person HUD)

| ✓ | Weapon | Tile | Ore ammo | Disp. | Frames needed (roughly) |
|---|---|---|---|---|---|
| [ ] | Kick / foot | KNEE 2521 | — | R | 2–3 |
| [ ] | Pistol | FIRSTGUN 2524 | coal | R | idle, fire 2, reload hand ~4 |
| [ ] | Shotgun | SHOTGUN 2613 | iron | R | idle, fire 2, pump ~4 |
| [ ] | Chaingun | CHAINGUN 2536 | gold | R | idle, barrel spin 3, muzzle flash |
| [ ] | Pipe bomb + detonator | HANDTHROW 2573 / HANDREMOTE 2570 | redstone | R | throw ~4, remote 2 |
| [ ] | Freezer | FREEZE 2548 | lapis | R | idle, fire 2 (missing from shareware anyway) |
| [ ] | RPG | RPGGUN 2544 | diamond | R | idle, fire 2 |
| [ ] | Shrinker, Devastator, Trip mine | | — | C | No ore gives their ammo |
| [ ] | Crosshair | CROSSHAIR 2523 | | R | 1 |
| [ ] | Projectiles in the world: rocket, pipe bomb, freeze bolt, shotspark, shell casings, muzzle flashes | | | R | |
| [ ] | Explosion, smoke, glass shards, water splash, fire | EXPLOSION2 1890, SMALLSMOKE, GLASSPIECES 1031… | | R | Explosion is the big one. |

## 5. Pickups (world sprites)

| ✓ | Item | Disp. | Notes |
|---|---|---|---|
| [ ] | Ammo ×6: clip (AMMO), shells, chaingun ammo, pipe bomb, freezer, rockets | R | These are the ore drops, so they're seen all the time. |
| [ ] | Weapon pickups (floor sprites) | C? | Duke spawns with every weapon, so these only matter if we place them. |
| [ ] | Health: first aid, cola, sixpak, atomic health | R/C | Probably becomes meat (step 3). Atomic health can stay if we keep the pun. |
| [ ] | Inventory: armour, steroids, jetpack, night vision, holoduke, boots, scuba, keycards | C | Cut by default. Keep any that suit gameplay later. |

## 6. HUD, menus, screens

**Art handoff:** the user designs these (e.g. in Claude Design) and hands over HTML with everything embedded, as reference and as source.
- Lay screens out on a **320×200 grid**: Duke's virtual screen, and our 960×600 web view is exactly 3× it.
- Any resolution or vector is fine. The build rasterises, downscales and matches it to the palette.
- Keep strokes and text legible at 320×200: glyphs at least ~7 px tall, no hairlines, no soft gradients.

| ✓ | Asset | Tile(s) | Disp. | Notes |
|---|---|---|---|---|
| [ ] | Status bar | BOTTOMSTATUSBAR 2462 | R | Or use the minimal HUD only. |
| [ ] | HUD digits | DIGITALNUM 2472 | R | 10 glyphs. |
| [ ] | Menu font | STARTALPHANUM 2822 | R | ~64 glyphs. |
| [ ] | Big font (menu titles) | BIGALPHANUM 2940 | R | |
| [ ] | Mini font | MINIFONT 3072 | R | |
| [ ] | Inventory/ammo HUD icons | | R/C | Only for items we keep. |
| [ ] | Title / menu background | MENUSCREEN 2456, BETASCREEN 2493 | R | eMine32 title. |
| [ ] | Logos | DREALMS 2492, THREEDEE 2498, INGAMEDUKETHREEDEE 2499 | R/C | |
| [ ] | Loading screen | LOADSCREEN 3281 | R | |
| [ ] | Help / ordering / story screens | F1HELP 2445, ORDERING 3270–3273, TEXTSTORY 3280 | C | Or one help screen of our own. |
| [ ] | Player third-person sprite | APLAYER 1405 | C? | Only seen in mirrors, multiplayer and death cam. |

## 7. Audio (roadmap step 5, later)

| ✓ | Asset | Count | Notes |
|---|---|---|---|
| [ ] | Sound effects + Duke voice lines | 181 VOC | Replace the ones we use; cut the rest. The new character needs a new voice. Ours can be WAV/OGG. |
| [ ] | Music | 7 MIDI + `D3DTIMBR.TMB` | **Music:** currently MIDI played through EDuke32's built-in OPL3 (AdLib FM) emulator, voiced by Duke's timbre bank. **Replacement:** OGG Vorbis (MP3 isn't supported; OGG is the same idea, about the same size). FLAC, or tracker modules (.mod/.xm/.it), also work. |

## 9. Before going public (deferred)

| ✓ | Item | Notes |
|---|---|---|
| [ ] | Palette | **Keep Duke's 256 colours (part of the look).** Write a tool that rebuilds the shade/translucency tables (`PALETTE.DAT`) and palette swaps (`LOOKUP.DAT`) from them, so we ship our own files rather than Duke's. |
| [ ] | `TABLES.DAT` | Console/small fonts + sine tables. Generate our own. |
| [ ] | Game scripts (`GAME.CON`, `DEFS.CON`, `USER.CON`) | Duke's script content. Our own CON grows as each mob/item is swapped; whatever's left gets rewritten. |
| [ ] | Maps | Our `E1L1.MAP` is ours. E1L2–E1L6 and `DUKESW.BIN` go. |
| [ ] | "Duke Nukem" name, likeness and voice | Trademarked (Gearbox). The in-game character needs his own name and look. |
| [ ] | "Duke Nukem 3D" / 3D Realms strings | Window title, menus, quit messages, episode/skill names, EDuke32 defaults. |
| [ ] | Minecraft names and designs | Mob names like Creeper, and Mojang's exact designs, are Mojang IP. Our own designs and names in the same roles are safe. |
| [ ] | Final check | Launch with the GRP removed and fix every blank tile/missing sound. See also `PRELAUNCH-CHECKLIST.md`. |

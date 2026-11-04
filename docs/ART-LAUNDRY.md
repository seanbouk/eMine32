# Asset laundry list

The goal is that eMine32 runs with **no `DUKE3D.GRP` at all**. Everything the game currently pulls from the
shareware GRP (13 ART files, 3 CON, 3 DAT, 6 maps, 7 MIDI, 181 VOC, 1 timbre bank, 1 demo) is either
**replaced** with ours or **cut** so nothing references it.

Disposition key:
- **R**: replace with our art.
- **C**: cut the feature, so no art is needed.
- **O**: already ours.
- **?**: undecided.

## 0. Do first: the foundation

| ✓ | Asset | From | Disp. | Notes |
|---|---|---|---|---|
| [ ] | Palette (256 colours) + shade tables + translucency table | `PALETTE.DAT` | R | **Do this first.** Every piece of art gets quantised to the palette, so changing it later means requantising everything. Build it from our block art + the user's hi-res art. |
| [ ] | Palette swaps (enemy colour variants, underwater/slime tints, night vision) | `LOOKUP.DAT` | R | Generated from our palette. |
| [ ] | Console / small text fonts, sine tables | `TABLES.DAT` | R | Sine tables can be generated. The fonts are tiny bitmaps. |
| [ ] | Game logic scripts | `GAME.CON`, `DEFS.CON`, `USER.CON` | R | Not art, but it's Duke's script content and can't ship. We need our own CON (actors, weapons, pickups). It grows naturally as each monster/item is swapped. |
| [x] | Level | `E1L1.MAP` | O | Ours (`tools/mkworld.py`). Cut E1L2–E1L6 and `DUKESW.BIN`. |

## 1. World

| ✓ | Asset | Tile(s) | Disp. | Notes |
|---|---|---|---|---|
| [~] | Block faces (27) | 7000–7026 | O | Ours, generated at 16×16. Swap for the user's art at **32×32** (build-time downscale). Faces: grass top/side, dirt, stone, bedrock, sand, log side/top, leaves, planks, cobble, gravel, coal/iron/gold/redstone/lapis/diamond ore, lava, water, obsidian, tall grass, red/yellow flower, glass, snow top/side. |
| [ ] | Sky | 89 (Duke city skyline) | R | Parallax panorama. Probably plain sky + clouds, and maybe a sun. |
| [ ] | Bullet holes / scorch decals | BULLETHOLE etc. | R/C | Not visible on blocks today. Can probably cut. |

## 2. Monsters (one at a time)

Shareware enemies and a suggested role-match. Each one gets our own design in Duke's sprite style.

| ✓ | Duke enemy | Behaviour | Suggested mob role |
|---|---|---|---|
| [ ] | LIZTROOP (1680) | Shoots, ducks, jetpack variant | Ranged archer (skeleton role) |
| [ ] | PIGCOP (2000) | Shotgun, tanky | Melee brute (zombie role). The "pig" pun suits a piglin-ish design. |
| [ ] | DRONE (1880) | Flies at you and explodes | Walking/creeping exploder (creeper role) |
| [ ] | LIZMAN (2120) | Big jumps, spits, charges | Jumper/climber (spider role) |
| [ ] | OCTABRAIN (1820) | Floats, psychic blast | Teleporter or floater (enderman role) |
| [ ] | COMMANDER (1920) | Floats, fires rockets | Floating fireballer (ghast role) |
| [ ] | ROTATEGUN (2360) | Fixed turret | C, or a spawner-ish block |
| [ ] | RECON (1960) | Flying car with spotlight | C, or a bat-like ambient flyer |
| [ ] | EGG / GREENSLIME (675/2370) | Slime that latches on | Slime role |
| [ ] | BOSS1 Battlelord (2630) | Boss | Boss (later) |
| [ ] | Shared gore: blood, jibs, per-enemy gib chunks, burn/freeze/shrink states | | R. Simplify where possible. |

**Art spec per mob (Duke convention):**
- 5 view angles: front, front-¾, side, back-¾, back. The engine mirrors these for the other 3.
- Per angle: idle 1, walk 4, attack 1–2, pain 1. That's about 40 images.
- A death sequence of about 5 frames, front only.
- Optional frozen/burnt variants. Palette swaps cover these cheaply.

## 3. Wildlife (new, roadmap step 3)

| ✓ | Asset | Disp. | Notes |
|---|---|---|---|
| [ ] | 2–3 passive animals | R | Same sprite spec as monsters, minus attack frames. |
| [ ] | Raw and cooked meat pickups | R | |

## 4. Player weapons (first-person HUD)

These are seen constantly, so they're high priority.

| ✓ | Weapon | Tile | Ore ammo | Disp. | Frames needed (roughly) |
|---|---|---|---|---|---|
| [ ] | Kick / foot | KNEE 2521 | — | R | 2–3 |
| [ ] | Pistol | FIRSTGUN 2524 | coal | R | idle, fire 2, reload hand ~4 |
| [ ] | Shotgun | 2613 | iron | R | idle, fire 2, pump ~4 |
| [ ] | Chaingun | 2536 | gold | R | idle, barrel spin 3, muzzle flash |
| [ ] | Pipe bomb + detonator | HANDTHROW 2573 / HANDREMOTE 2570 | redstone | R | throw ~4, remote 2 |
| [ ] | Freezer | 2548 | lapis | R | idle, fire 2 (missing from shareware anyway) |
| [ ] | RPG | 2544 | diamond | R | idle, fire 2 |
| [ ] | Shrinker, Devastator, Trip mine | | — | C | No ore gives their ammo |
| [ ] | Crosshair | CROSSHAIR 2523 | | R | 1 |
| [ ] | Projectiles in the world: rocket, pipe bomb, freeze bolt, shotspark, shell casings, muzzle flashes | | | R | |
| [ ] | Explosion, smoke, glass shards, water splash, fire | EXPLOSION2, SMALLSMOKE, GLASSPIECES… | | R | Explosion is the big one. |

## 5. Pickups (world sprites)

| ✓ | Item | Disp. | Notes |
|---|---|---|---|
| [ ] | Ammo ×6: clip (AMMO), shells, chaingun ammo, pipe bomb, freezer, rockets | R | These are the ore drops, so they're seen all the time. |
| [ ] | Weapon pickups (floor sprites) | C? | Duke spawns with every weapon, so these only matter if we place them. |
| [ ] | Health: first aid, cola, sixpak, atomic health | R/C | Probably becomes meat (step 3). Atomic health can stay if we keep the pun. |
| [ ] | Inventory: armour, steroids, jetpack, night vision, holoduke, boots, scuba, keycards | C | Cut by default. Keep any that suit gameplay later. |

## 6. HUD, menus, screens

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

## 7. Names and text (trademarks, not art)

| ✓ | Item | Notes |
|---|---|---|
| [ ] | "Duke Nukem" name, likeness and voice | Trademarked (Gearbox). The player character needs our own name and look before launch. "Duke" in our docs is fine; in-game text isn't. |
| [ ] | "Duke Nukem 3D" / 3D Realms strings | Window title, menus, quit messages, episode/skill names in the CON, EDuke32 defaults. |
| [ ] | Minecraft names and designs | Mob names like Creeper, and Mojang's exact designs, are Mojang IP. Our own designs and names in the same roles are safe. |

## 8. Audio (roadmap step 5, later)

| ✓ | Asset | Count | Notes |
|---|---|---|---|
| [ ] | Sound effects + Duke voice lines | 181 VOC | Replace the ones we use; cut the rest. Voice lines need a new character voice. |
| [ ] | Music | 7 MIDI + `D3DTIMBR.TMB` | Replace, or ship without music. |

## Suggested order

1. **Palette.** Every later asset depends on it.
2. **Block textures at 32×32**, via the build-time downscale pipeline. This also proves out the pipeline.
3. **Ammo pickups + first-person weapons.** You see them every second.
4. **Sky, crosshair, fonts, HUD digits.**
5. **Monsters, one at a time** (with our own CON actor for each), then wildlife.
6. **Menus and screens, cutting Duke content and text**, then audio.
7. Final check: launch with the GRP removed and fix every blank tile.

# Pre-submission checklist

Run through this before publishing to itch.io / Game Off.

## Assets & licensing
- [ ] No Duke Nukem 3D data ships: no DUKE3D.GRP contents, tiles, sounds, music, maps, CON from the original game.
      (During development we use the 1.3D shareware GRP from `devdata/`, which is git-ignored. Its licence forbids
      redistribution as part of another product.)
- [ ] Every tile/texture/sprite in `game/` is ours (made locally or generated) - check palette.dat, tables.dat, lookup.dat too.
- [ ] Every sound / voice line is ours (remade V/O, not sampled).
- [ ] HUD, fonts, menus replaced.
- [ ] No Ion Fury assets anywhere (EDuke32 auto-detects the Steam install on the dev machine).
- [ ] EDuke32 GPLv2 obligations: source public (this repo), licence + credits included in the build.
- [ ] Repo is public and matches the submitted build (Game Off rule).

## Font licences (SIL Open Font Licence 1.1)
- [ ] Ship each font's `OFL.txt` with the build (`art/fonts/anton`, `pressstart2p`, `silkscreen`) and credit them:
      Anton (The Anton Project Authors), Press Start 2P (CodeMan38), Silkscreen (The Silkscreen Project Authors).

## Build engine licence (Ken Silverman, BUILDLIC.TXT) - applies on top of GPL
- [ ] Distributed only over the internet, completely free of charge (itch.io free / no paid tiers or donations-gated builds).
- [ ] Ship `BUILDLIC.TXT` and the "Build Engine & Tools Copyright (c) 1993-1997 Ken Silverman" notice with every build (incl. web).
- [ ] Any Build engine source file we modify carries: `// This file has been modified from Ken Silverman's original release`

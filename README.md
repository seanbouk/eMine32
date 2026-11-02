# eMine32

A block-world (mine/build) engine built **inside EDuke32**, the Duke Nukem 3D source port, keeping the
Build-engine look: 8-bit palette, shade tables and distance fog, billboarded "sprolygon" enemies, a weapon on the HUD.
The target is a browser build (WebAssembly) for itch.io.

Status: **early engine work**. Nothing playable yet.

## Layout

| Path | What |
|---|---|
| `engine/` | Fork of [EDuke32](https://voidpoint.io/terminx/eduke32) (upstream `ec5824db8`, 2026-08-07) plus the block-world additions. This is the reusable engine. Build it and point it at any game data. |
| `game/` | eMine32's own game data (art, CON scripts, sounds). Swap in your own to make a different game on the engine. |
| `scripts/` | Build scripts (native Windows, web). |
| `docs/` | Notes, including the pre-release licence checklist. |
| `devdata/` | *(git-ignored)* Local test data, e.g. the Duke3D 1.3D shareware `DUKE3D.GRP`. Never committed. |
| `run/` | *(git-ignored)* Working directory for configs, logs and saves. |

## Building (Windows)

Needs an llvm-mingw / MinGW toolchain and GNU make on PATH. Run from Git Bash:

```sh
scripts/build-native.sh
```

Then `run.bat` (title screen) or `run.bat E1L1.MAP` (straight into a map). Until `game/` has its own data, this
needs the Duke3D shareware `DUKE3D.GRP` in `devdata/duke3d-sw/`.

## Licences

The engine is EDuke32 (GPLv2, `engine/package/common/gpl-2.0.txt`), which includes Ken Silverman's Build engine under
its own terms (`engine/package/common/buildlic.txt`): **non-commercial, internet distribution only, free of charge**.

"Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman. Ken Silverman's official web site: http://www.advsys.net/ken

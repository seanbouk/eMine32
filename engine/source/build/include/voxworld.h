// eMine32 block world ("voxworld") for the Build engine classic renderer.
//
// A map opts in by having exactly one sector whose `extra` is VW_MAP_MAGIC. That sector is a big
// box the game logic lives in; the blocks inside it are rendered, collided with and hit-scanned by
// this module instead of Build's sector/wall code. Sprites in that sector are drawn by this module
// too, depth-tested against the blocks.
//
// Units: a block is VW_BLOCKXY Build units wide and VW_BLOCKZ z-units tall (Build z is 16x finer
// than x/y, so blocks are cubes). Block layer 0 sits on the sector floor; layers go up (Build z
// decreases upward).
//
// Copyright (C) 2026 the eMine32 project. GPLv2 (see gpl-2.0.txt).

#pragma once

#ifndef voxworld_h_
#define voxworld_h_

#include "compat.h"

#define VW_MAP_MAGIC 0x7E32

#define VW_BLOCKXY 512
#define VW_BLOCKZ  8192

#define VW_TEXSIZE 16  // texels per block face edge (block faces are VW_TEXSIZE x VW_TEXSIZE tiles)

enum vwblock_t : uint8_t
{
    VW_AIR = 0,
    VW_GRASS,
    VW_DIRT,
    VW_STONE,
    VW_BEDROCK,
    VW_SAND,
    VW_WOOD,
    VW_LEAVES,
    VW_PLANKS,
    VW_COBBLE,
    VW_NUMTYPES
};

// First tile number used for block faces; see game/emine32.def. Tile layout per block type is in
// voxworld.cpp (vw_blockdefs).
#define VW_TILEBASE 7000

#ifdef __cplusplus
extern "C++" {

bool    voxworld_active(void);
int     voxworld_sector(void);

// Called by engineLoadBoard after a map has loaded; activates the block world if the map opts in.
void    voxworld_onmapload(vec3_t *dapos);

// Block access (block coordinates). Out-of-range reads return VW_AIR (or VW_BEDROCK below the world).
uint8_t voxworld_get(int bx, int by, int bz);
void    voxworld_set(int bx, int by, int bz, uint8_t type);
void    voxworld_size(int *sx, int *sy, int *sz);
// World <-> block coordinate helpers. Return false if outside the world.
bool    voxworld_toblock(int32_t x, int32_t y, int32_t z, int *bx, int *by, int *bz);

// Rendering (classic renderer). Called from renderDrawRoomsQ16 / renderDrawMasks.
void    voxworld_drawworld(void);
void    voxworld_drawsprites(void);

// Collision. All take Build world coordinates.
// Moves *pos by (xvect>>14, yvect>>14) as far as blocks allow, sliding along faces.
// Returns 0 if unobstructed, else 1.
int     voxworld_clipmove(vec3_t *pos, int32_t *xvect, int32_t *yvect, int32_t walldist, int32_t ceildist, int32_t flordist);
// Tightens *ceilz / *florz with the blocks around (pos, walldist). Returns bit 1 if the floor came
// from a block, bit 2 if the ceiling did.
int     voxworld_getzrange(const vec3_t *pos, int32_t walldist, int32_t *ceilz, int32_t *florz);

struct vwhit_t
{
    vec3_t  pos;      // hit point (Build coords)
    int     bx, by, bz;  // block hit
    int     face;     // 0:-x 1:+x 2:-y 3:+y 4:top 5:bottom (face of the block that was hit)
    float   dist;     // parametric distance along the ray, in units of the ray vector
};
// Ray from start along (vx,vy,vz) (z in Build z units), up to maxt ray-vector lengths.
bool    voxworld_raycast(const vec3_t *start, int32_t vx, int32_t vy, int32_t vz, float maxt, vwhit_t *hit);
// Line of sight between two points (true if no block in between).
bool    voxworld_cansee(int32_t x1, int32_t y1, int32_t z1, int32_t x2, int32_t y2, int32_t z2);

}
#endif

#endif

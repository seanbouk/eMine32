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
    VW_GRAVEL,
    VW_COAL,
    VW_IRON,
    VW_GOLD,
    VW_ATOMIC,   // placeholder rare glowing ore (block light test)
    VW_LAVA,     // lava source (fluid)
    VW_LAVA_FALL,  // lava pouring down
    VW_LAVA_F3,  // lava spreading sideways, 3 = nearest the source
    VW_LAVA_F2,
    VW_LAVA_F1,
    VW_WATER,      // water source (fluid, see-through)
    VW_WATER_FALL, // water pouring down
    VW_WATER_F4,   // water spreading sideways, 4 = nearest the source
    VW_WATER_F3,
    VW_WATER_F2,
    VW_WATER_F1,
    VW_OBSIDIAN,   // what a lava source turns into when water reaches it
    VW_TALLGRASS,  // plants: drawn as billboards, no collision, light and shots pass through
    VW_FLOWER_RED,
    VW_FLOWER_YELLOW,
    VW_NUMTYPES
};

// Block type flags (voxworld_blockflags).
enum
{
    VW_BF_NONSOLID   = 1,  // drawn, hit by shots, blocks sight, but you can walk into it (fluids)
    VW_BF_FLUID      = 2,
    VW_BF_FULLBRIGHT = 4,  // drawn without lighting (glows)
    VW_BF_LAVA       = 8,  // burns
    VW_BF_WATER      = 16, // you swim in it
    VW_BF_CLEAR      = 32, // see-through (water, plants): light, sight and shots pass through
    VW_BF_PLANT      = 64, // drawn as a camera-facing billboard standing in its cell; breaks if the block under it goes
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

int     voxworld_blockflags(int type);
// Block type at a world position (VW_AIR outside the world).
int     voxworld_blockat(int32_t x, int32_t y, int32_t z);

// Destruction.
// hitscan() remembers which block it hit; voxworld_lasthit returns it if pos is that hit's position.
void    voxworld_notehit(const vec3_t *pos, int bx, int by, int bz);
bool    voxworld_lasthit(const vec3_t *pos, int *bx, int *by, int *bz);
// Adds damage; returns the destroyed block's type (or VW_AIR if it survives / can't be damaged).
int     voxworld_damage(int bx, int by, int bz, int amount);
int     voxworld_hardness(int type);
void    voxworld_blockcenter(int bx, int by, int bz, vec3_t *out);
typedef void (*vwdestroyfn_t)(int bx, int by, int bz, int type, void *ud);
// Damage falls off linearly to 0 at radius (xy units). Calls fn for each destroyed block. Returns count.
int     voxworld_explode(const vec3_t *pos, int32_t radius, int damage, vwdestroyfn_t fn, void *ud);
// Top of the nearest block at/below z at (x,y), or def if none.
int32_t voxworld_floorz(int32_t x, int32_t y, int32_t z, int32_t def);

// Lighting: skylight 0..VW_LIGHTMAX per air cell (flood-filled from the sky, Minecraft style), turned into extra
// Build shade. Returns the extra shade for a point in the world (0 in full daylight).
#define VW_LIGHTMAX 15
// Colours of block light (each gets a tinted shade table).
enum { VW_TINT_NONE, VW_TINT_LAVA, VW_TINT_ATOMIC, VW_NUMTINTS };
int     voxworld_lightshade(int32_t x, int32_t y, int32_t z);
// Dev helper: the nearest spot (feet position) with room to stand whose light is <= maxlight.
bool    voxworld_findcave(int32_t x, int32_t y, int maxlight, vec3_t *out);
// Same, but a dark spot whose block light is at least minblocklight (somewhere lit by a glowing block).
bool    voxworld_findspot(int32_t x, int32_t y, int maxlight, int minblocklight, vec3_t *out);
// Dev helper: the surface (top centre) of the nearest open water; or with shore, the top of the nearest dry block
// beside open water (*ang: Build angle facing the water).
bool    voxworld_findwater(int32_t x, int32_t y, bool shore, vec3_t *out, int *ang);

// Falling blocks and other world events. The game supplies the callback.
enum
{
    VW_EV_FALLHIT,     // a falling block is about to move into cell (bx,by,bz): return true if an actor is in the way
                       // (damage it; the block then bursts)
    VW_EV_FALLBROKE,   // the falling block at (bx,by,bz) burst (debris, sound)
    VW_EV_FALLLANDED,  // a falling block came to rest at (bx,by,bz)
};
typedef bool (*vweventfn_t)(int event, int bx, int by, int bz, int type, float speed);  // speed: cells per tic
void    voxworld_seteventfn(vweventfn_t fn);
// Advance falling blocks by one game tic.
void    voxworld_tick(void);

// Sprites whose lotag is (VW_DEBRIS_TAG | blocktype) are drawn as chunks of that block's texture.
#define VW_DEBRIS_TAG 0x7E00

}
#endif

#endif

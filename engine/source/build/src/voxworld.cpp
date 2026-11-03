// eMine32 block world for the Build engine classic renderer. See voxworld.h.
//
// Rendering: one screen column at a time, a 2D DDA walks the block grid front to back along the
// column's ray. Each block column holds a short list of solid "runs" (vertical spans of blocks).
// Because the camera never rolls and Build looks up/down by y-shearing, every block face is either
// vertical (constant depth down a screen column, like Build walls) or horizontal (like Build
// floors), so each column is filled with exact perspective using the same tricks Build uses.
// Occlusion is a per-column list of still-open pixel intervals ("gaps"); the walk stops when a
// column is full. Depth is written to a float buffer so sprites can be depth-tested.
//
// Copyright (C) 2026 the eMine32 project. GPLv2 (see gpl-2.0.txt).

#include "build.h"
#include "baselayer.h"
#include "engine_priv.h"
#include "palette.h"
#include "voxworld.h"

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
#endif

#include <math.h>
#include <float.h>
#include <algorithm>


static void vw_registerosd(void);

//////////////////////////////////////////////////////////////////////////////
// World storage
//////////////////////////////////////////////////////////////////////////////

#define VW_MAXRUNS 16  // max separate solid runs per block column

struct vwrun_t { uint8_t z0, z1; };  // solid layers [z0, z1)

struct vwcolumn_t
{
    uint8_t  nruns;
    vwrun_t  runs[VW_MAXRUNS];
};

static bool        vw_active;
static int         vw_sect = -1;
static int         vw_sx, vw_sy, vw_sz;      // size in blocks
static int32_t     vw_x0, vw_y0;             // world coords of block (0,0) corner
static int32_t     vw_zbot;                  // Build z of the bottom of layer 0
static uint8_t    *vw_blocks;                // [(x*sy + y)*sz + z]
static vwcolumn_t *vw_cols;                  // [x*sy + y]

static FORCE_INLINE uint8_t *vw_colblocks(int bx, int by) { return &vw_blocks[((size_t)bx*vw_sy + by)*vw_sz]; }
static FORCE_INLINE vwcolumn_t *vw_col(int bx, int by) { return &vw_cols[(size_t)bx*vw_sy + by]; }
static FORCE_INLINE bool vw_inxy(int bx, int by) { return (unsigned)bx < (unsigned)vw_sx && (unsigned)by < (unsigned)vw_sy; }

static void vw_rebuildcolumn(int bx, int by)
{
    uint8_t const *b = vw_colblocks(bx, by);
    vwcolumn_t *c = vw_col(bx, by);
    c->nruns = 0;
    int z = 0;
    while (z < vw_sz)
    {
        while (z < vw_sz && b[z] == VW_AIR) z++;
        if (z >= vw_sz) break;
        int const z0 = z;
        while (z < vw_sz && b[z] != VW_AIR) z++;
        if (c->nruns < VW_MAXRUNS)
            c->runs[c->nruns++] = { (uint8_t)z0, (uint8_t)z };
        else
            c->runs[VW_MAXRUNS-1].z1 = (uint8_t)z;  // merge overflow into the top run (rare)
    }
}

bool voxworld_active(void) { return vw_active; }
int  voxworld_sector(void) { return vw_sect; }
void voxworld_size(int *sx, int *sy, int *sz) { *sx = vw_sx; *sy = vw_sy; *sz = vw_sz; }

uint8_t voxworld_get(int bx, int by, int bz)
{
    if (!vw_active || !vw_inxy(bx, by)) return VW_AIR;
    if (bz < 0) return VW_BEDROCK;
    if (bz >= vw_sz) return VW_AIR;
    return vw_colblocks(bx, by)[bz];
}

static void vw_lightopened(int bx, int by, int bz);

void voxworld_set(int bx, int by, int bz, uint8_t type)
{
    if (!vw_active || !vw_inxy(bx, by) || (unsigned)bz >= (unsigned)vw_sz) return;
    vw_colblocks(bx, by)[bz] = type;
    vw_rebuildcolumn(bx, by);
    if (type == VW_AIR) vw_lightopened(bx, by, bz);
}

//////////////////////////////////////////////////////////////////////////////
// Lighting: skylight per air cell, 0..VW_LIGHTMAX. Full skylight falls straight down without fading;
// everywhere else light drops by one per block it spreads (so it reaches ~15 blocks into a cave mouth).
// Blocks only ever get removed in play, so light only ever needs to spread, never be taken away.
//////////////////////////////////////////////////////////////////////////////

static uint8_t  *vw_light;            // same layout as vw_blocks
static uint32_t *vw_lightq;           // flood queue (cell indices)
static size_t    vw_lightqcap;
static int       vw_darkness = 20;    // extra shade at light 0; OSD: r_voxdark
static uint8_t   vw_lightshadetab[VW_LIGHTMAX + 1];

static void vw_updatelightshades(void)
{
    for (int l = 0; l <= VW_LIGHTMAX; l++)
        vw_lightshadetab[l] = (uint8_t)((vw_darkness * (VW_LIGHTMAX - l) + VW_LIGHTMAX/2) / VW_LIGHTMAX);
}

static FORCE_INLINE int vw_lightat(int bx, int by, int bz)
{
    if (!vw_inxy(bx, by) || bz >= vw_sz) return VW_LIGHTMAX;  // outside the world is open sky
    if (bz < 0 || !vw_light) return bz < 0 ? 0 : VW_LIGHTMAX;
    return vw_light[((size_t)bx*vw_sy + by)*vw_sz + bz];
}

static FORCE_INLINE int vw_lightshade(int bx, int by, int bz) { return vw_lightshadetab[vw_lightat(bx, by, bz)]; }

static void vw_lightpush(size_t &n, uint32_t idx)
{
    if (n == vw_lightqcap)
    {
        vw_lightqcap = max<size_t>(vw_lightqcap * 2, 65536);
        vw_lightq = (uint32_t *)Xrealloc(vw_lightq, vw_lightqcap * sizeof(uint32_t));
    }
    vw_lightq[n++] = idx;
}

// Spread light out from the first n queued cells.
static void vw_lightflood(size_t n)
{
    int const sz = vw_sz, sy = vw_sy;
    for (size_t qi = 0; qi < n; qi++)
    {
        uint32_t const idx = vw_lightq[qi];
        int const l = vw_light[idx];
        if (l <= 1) continue;
        int const bz = idx % sz, col = idx / sz, bx = col / sy, by = col % sy;

        static const int8_t dirs[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };
        for (auto const &d : dirs)
        {
            int const nx = bx + d[0], ny = by + d[1], nz = bz + d[2];
            if (!vw_inxy(nx, ny) || (unsigned)nz >= (unsigned)sz) continue;
            uint32_t const nidx = (uint32_t)(((size_t)nx*sy + ny)*sz + nz);
            if (vw_blocks[nidx] != VW_AIR) continue;
            int const nl = (d[2] < 0 && l == VW_LIGHTMAX) ? VW_LIGHTMAX : l - 1;
            if (nl > vw_light[nidx])
            {
                vw_light[nidx] = (uint8_t)nl;
                vw_lightpush(n, nidx);
            }
        }
    }
}

static void vw_computelight(void)
{
    size_t const total = (size_t)vw_sx*vw_sy*vw_sz;
    Xfree(vw_light);
    vw_light = (uint8_t *)Xcalloc(total, 1);

    // Open sky: everything above each column's top block. skyfloor = lowest such layer.
    auto skyfloor = [](int bx, int by) -> int {
        if (!vw_inxy(bx, by)) return 0;
        vwcolumn_t const *c = vw_col(bx, by);
        return c->nruns ? c->runs[c->nruns-1].z1 : 0;
    };
    for (int bx = 0; bx < vw_sx; bx++)
        for (int by = 0; by < vw_sy; by++)
        {
            uint8_t *l = &vw_light[((size_t)bx*vw_sy + by)*vw_sz];
            for (int z = skyfloor(bx, by); z < vw_sz; z++) l[z] = VW_LIGHTMAX;
        }

    // Seed the flood with sky cells that sit beside a non-sky cell (cave mouths, overhangs).
    size_t n = 0;
    for (int bx = 0; bx < vw_sx; bx++)
        for (int by = 0; by < vw_sy; by++)
        {
            int const f = skyfloor(bx, by);
            int const top = max(max(skyfloor(bx-1, by), skyfloor(bx+1, by)), max(skyfloor(bx, by-1), skyfloor(bx, by+1)));
            for (int z = f; z < top; z++)
                vw_lightpush(n, (uint32_t)(((size_t)bx*vw_sy + by)*vw_sz + z));
        }
    vw_lightflood(n);
}

// A block was removed: light the new air cell from its neighbours and spread from there.
static void vw_lightopened(int bx, int by, int bz)
{
    if (!vw_light) return;
    int best = vw_lightat(bx, by, bz + 1);
    if (best != VW_LIGHTMAX) best--;  // full skylight from straight above doesn't fade
    best = max(best, vw_lightat(bx-1, by, bz) - 1);
    best = max(best, vw_lightat(bx+1, by, bz) - 1);
    best = max(best, vw_lightat(bx, by-1, bz) - 1);
    best = max(best, vw_lightat(bx, by+1, bz) - 1);
    best = max(best, vw_lightat(bx, by, bz - 1) - 1);
    uint32_t const idx = (uint32_t)(((size_t)bx*vw_sy + by)*vw_sz + bz);
    vw_light[idx] = (uint8_t)max(best, 0);
    size_t n = 0;
    vw_lightpush(n, idx);
    vw_lightflood(n);
}

bool voxworld_findcave(int32_t x, int32_t y, int maxlight, vec3_t *out)
{
    if (!vw_active || !vw_light) return false;
    int cx, cy, cz;
    voxworld_toblock(x, y, 0, &cx, &cy, &cz);
    // nearest column first (square rings), lowest suitable cell in it
    for (int r = 0; r < max(vw_sx, vw_sy); r++)
        for (int bx = cx - r; bx <= cx + r; bx++)
            for (int by = cy - r; by <= cy + r; by++)
            {
                if (max(abs(bx - cx), abs(by - cy)) != r || !vw_inxy(bx, by)) continue;
                uint8_t const *b = vw_colblocks(bx, by);
                uint8_t const *l = &vw_light[((size_t)bx*vw_sy + by)*vw_sz];
                for (int z = 1; z + 2 < vw_sz; z++)
                    if (b[z-1] != VW_AIR && b[z] == VW_AIR && b[z+1] == VW_AIR && b[z+2] == VW_AIR && l[z] <= maxlight)
                    {
                        out->x = vw_x0 + bx*VW_BLOCKXY + VW_BLOCKXY/2;
                        out->y = vw_y0 + by*VW_BLOCKXY + VW_BLOCKXY/2;
                        out->z = vw_zbot - z*VW_BLOCKZ;
                        return true;
                    }
            }
    return false;
}

int voxworld_lightshade(int32_t x, int32_t y, int32_t z)
{
    if (!vw_active) return 0;
    int bx, by, bz;
    voxworld_toblock(x, y, z, &bx, &by, &bz);
    // standing inside a block (feet on a step edge etc.): use the cell above
    if (vw_inxy(bx, by) && (unsigned)bz < (unsigned)vw_sz && vw_colblocks(bx, by)[bz] != VW_AIR) bz++;
    return vw_lightshade(bx, by, bz);
}

bool voxworld_toblock(int32_t x, int32_t y, int32_t z, int *bx, int *by, int *bz)
{
    *bx = (int)floorf((float)(x - vw_x0) / VW_BLOCKXY);
    *by = (int)floorf((float)(y - vw_y0) / VW_BLOCKXY);
    *bz = (int)floorf((float)(vw_zbot - z) / VW_BLOCKZ);
    return vw_inxy(*bx, *by) && (unsigned)*bz < (unsigned)vw_sz;
}

static FORCE_INLINE int32_t vw_layertop(int layer) { return vw_zbot - layer*VW_BLOCKZ; }  // Build z of top of layer-1... i.e. bottom of `layer`

//////////////////////////////////////////////////////////////////////////////
// Block types
//////////////////////////////////////////////////////////////////////////////

// Tile offsets from VW_TILEBASE for each face; must match game/emine32.def.
enum
{
    T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, T_STONE, T_BEDROCK, T_SAND, T_WOOD_SIDE, T_WOOD_TOP, T_LEAVES, T_PLANKS, T_COBBLE,
    T_GRAVEL, T_COAL, T_IRON, T_GOLD,
    T_COUNT
};

// hardness: damage points to destroy (0 = indestructible). Duke's pistol does ~6 per shot.
struct vwblockdef_t { uint8_t top, side, bottom, hardness; };

static const vwblockdef_t vw_blockdefs[VW_NUMTYPES] =
{
    { 0, 0, 0, 0 },                                 // air (unused)
    { T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, 12 },      // grass
    { T_DIRT, T_DIRT, T_DIRT, 12 },                 // dirt
    { T_STONE, T_STONE, T_STONE, 48 },              // stone
    { T_BEDROCK, T_BEDROCK, T_BEDROCK, 0 },         // bedrock
    { T_SAND, T_SAND, T_SAND, 10 },                 // sand
    { T_WOOD_TOP, T_WOOD_SIDE, T_WOOD_TOP, 30 },    // wood
    { T_LEAVES, T_LEAVES, T_LEAVES, 6 },            // leaves
    { T_PLANKS, T_PLANKS, T_PLANKS, 24 },           // planks
    { T_COBBLE, T_COBBLE, T_COBBLE, 40 },           // cobble
    { T_GRAVEL, T_GRAVEL, T_GRAVEL, 14 },           // gravel
    { T_COAL, T_COAL, T_COAL, 52 },                 // coal ore
    { T_IRON, T_IRON, T_IRON, 64 },                 // iron ore
    { T_GOLD, T_GOLD, T_GOLD, 56 },                 // gold ore
};

// Accumulated damage per block, same layout as vw_blocks (allocated on first damage).
static uint8_t *vw_dmg;

static FORCE_INLINE size_t vw_index(int bx, int by, int bz) { return ((size_t)bx*vw_sy + by)*vw_sz + bz; }

// Extra shade (darker) for a damaged block: up to +10 just before it breaks.
static FORCE_INLINE int vw_dmgshade(size_t idx, uint8_t type)
{
    if (!vw_dmg || !vw_dmg[idx]) return 0;
    int const h = vw_blockdefs[type].hardness;
    return h ? (vw_dmg[idx] * 10) / h : 0;
}

// Texel data for each face tile, column-major (Build tile order): tex[u*VW_TEXSIZE + v].
// Falls back to a generated checker if the tile isn't present or isn't VW_TEXSIZE square.
static uint8_t vw_tex[T_COUNT][VW_TEXSIZE*VW_TEXSIZE];

static void vw_loadtextures(void)
{
    for (int t = 0; t < T_COUNT; t++)
    {
        int const tile = VW_TILEBASE + t;
        uint8_t *dst = vw_tex[t];

        if (tilesiz[tile].x == VW_TEXSIZE && tilesiz[tile].y == VW_TEXSIZE && tileLoad(tile) && waloff[tile])
        {
            Bmemcpy(dst, (void *)waloff[tile], VW_TEXSIZE*VW_TEXSIZE);
            continue;
        }

        static bool warned[T_COUNT];
        if (!warned[t])
        {
            LOG_F(WARNING, "voxworld: block tile %d missing or not %dx%d; using placeholder", tile, VW_TEXSIZE, VW_TEXSIZE);
            warned[t] = true;
        }
        uint8_t const c1 = 16 + t*8, c2 = c1 + 4;
        for (int u = 0; u < VW_TEXSIZE; u++)
            for (int v = 0; v < VW_TEXSIZE; v++)
                dst[u*VW_TEXSIZE + v] = ((u ^ v) & 4) ? c1 : c2;
    }
}

//////////////////////////////////////////////////////////////////////////////
// Terrain generation (spike: lives here for now; moves to game code later)
//////////////////////////////////////////////////////////////////////////////

static uint32_t vw_seed;

static FORCE_INLINE uint32_t vw_hash(int32_t x, int32_t y, uint32_t s)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static FORCE_INLINE float vw_hashf(int32_t x, int32_t y, uint32_t s) { return (vw_hash(x, y, s) & 0xffffff) * (1.f / 16777216.f); }

static float vw_valuenoise(float x, float y, uint32_t s)
{
    int32_t const xi = (int32_t)floorf(x), yi = (int32_t)floorf(y);
    float const xf = x - xi, yf = y - yi;
    float const u = xf*xf*(3.f - 2.f*xf), v = yf*yf*(3.f - 2.f*yf);
    float const a = vw_hashf(xi, yi, s), b = vw_hashf(xi+1, yi, s), c = vw_hashf(xi, yi+1, s), d = vw_hashf(xi+1, yi+1, s);
    return a + (b - a)*u + (c - a)*v + (a - b - c + d)*u*v;
}

static float vw_fbm(float x, float y, uint32_t s, int oct)
{
    float sum = 0.f, amp = 1.f, norm = 0.f;
    for (int i = 0; i < oct; i++)
    {
        sum += vw_valuenoise(x, y, s + i*101) * amp;
        norm += amp;
        amp *= 0.5f; x *= 2.f; y *= 2.f;
    }
    return sum / norm;
}

static void vw_put(int x, int y, int z, uint8_t t)
{
    if (vw_inxy(x, y) && (unsigned)z < (unsigned)vw_sz) vw_colblocks(x, y)[z] = t;
}

static int vw_surface(int x, int y)
{
    uint8_t const *b = vw_colblocks(x, y);
    for (int z = vw_sz-1; z >= 0; z--)
        if (b[z] != VW_AIR && b[z] != VW_LEAVES) return z;
    return -1;
}

// Small deterministic RNG for generation.
struct vwrng_t
{
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float    f() { return (next() & 0xffffff) * (1.f / 16777216.f); }
    int      range(int a, int b) { return b <= a ? a : a + (int)(next() % (uint32_t)(b - a + 1)); }
};

static int *vw_hmap;  // generation only: top solid layer of the original terrain per column

static FORCE_INLINE int vw_height(int x, int y)
{
    x = clamp(x, 0, vw_sx-1); y = clamp(y, 0, vw_sy-1);
    return vw_hmap[x*vw_sy + y];
}

static void vw_carve(int x, int y, int z)
{
    if (!vw_inxy(x, y) || z < 1 || z >= vw_sz) return;
    // keep the spawn point and the test arch intact
    int const dx = x - (vw_sx/2 + 3), dy = y - vw_sy/2;
    if (dx*dx + dy*dy < 10*10 && z > vw_height(x, y) - 9) return;
    uint8_t &b = vw_colblocks(x, y)[z];
    if (b != VW_BEDROCK) b = VW_AIR;
}

// Tunnel cross-section: a disc of radius r with a flat floor, `height` layers tall, a layer taller in the middle.
static void vw_carveslice(float cx, float cy, int floor, float r, int height)
{
    for (int x = (int)floorf(cx - r); x <= (int)floorf(cx + r); x++)
        for (int y = (int)floorf(cy - r); y <= (int)floorf(cy + r); y++)
        {
            float const ex = x + 0.5f - cx, ey = y + 0.5f - cy, d2 = ex*ex + ey*ey;
            if (d2 > r*r) continue;
            int const h = height + (d2 < r*r*0.3f);
            for (int z = floor; z < floor + h; z++) vw_carve(x, y, z);
        }
}

// A cavern: flat floor, domed roof.
static void vw_room(vwrng_t &rng, float cx, float cy, int floor)
{
    float const rx = 4.f + rng.f()*5.f, ry = 4.f + rng.f()*5.f;
    int const hgt = rng.range(3, 6);
    for (int x = (int)floorf(cx - rx); x <= (int)floorf(cx + rx); x++)
        for (int y = (int)floorf(cy - ry); y <= (int)floorf(cy + ry); y++)
        {
            float const ex = (x + 0.5f - cx) / rx, ey = (y + 0.5f - cy) / ry, e = ex*ex + ey*ey;
            if (e > 1.f) continue;
            int const h = 2 + (int)(hgt * sqrtf(1.f - e) + 0.5f);
            for (int z = floor; z < floor + h; z++) vw_carve(x, y, z);
        }
}

// A wandering tunnel. Mostly level (Duke can't step up a whole block), with the odd one-block step.
// Entrance tunnels start at the surface and stair down into the ground first.
static void vw_worm(vwrng_t &rng, float x, float y, int floor, int len, bool entrance, float yaw = -1.f)
{
    if (yaw < 0.f) yaw = rng.f() * 2.f * fPI;
    float dyaw = 0.f, r = 1.3f + rng.f();
    for (int i = 0; i < len; i++)
    {
        dyaw = (dyaw + (rng.f() - 0.5f) * 0.3f) * 0.85f;
        yaw += dyaw;
        x += cosf(yaw) * 0.8f;
        y += sinf(yaw) * 0.8f;
        if (x < 1 || y < 1 || x >= vw_sx - 1 || y >= vw_sy - 1) break;
        r = clamp(r + (rng.f() - 0.5f) * 0.2f, 1.2f, 2.6f);

        int const h = vw_height((int)x, (int)y);
        if (entrance)
        {
            if (floor > h - 10) { if (i & 1) floor--; }
            else entrance = false;
            floor = min(floor, h);
        }
        else
        {
            if (rng.f() < 0.04f) floor += rng.f() < 0.5f ? -1 : 1;
            if (floor > h - 4) floor--;  // stay underground (mostly: valleys still get the odd skylight)
        }
        floor = clamp(floor, 2, vw_sz - 5);

        vw_carveslice(x, y, floor, r, 3);
        if (!entrance && rng.f() < 0.006f) vw_room(rng, x, y, floor);
    }
}

// Replace stone (and optionally dirt) along a short random walk: ore veins, gravel pockets.
static void vw_vein(vwrng_t &rng, uint8_t type, int x, int y, int z, int size, bool anysoil)
{
    for (int k = 0; k < size; k++)
    {
        if (vw_inxy(x, y) && z >= 1 && z < vw_sz)
        {
            uint8_t &b = vw_colblocks(x, y)[z];
            if (b == VW_STONE || (anysoil && b == VW_DIRT)) b = type;
        }
        switch (rng.next() % 6)
        {
            case 0: x--; break; case 1: x++; break; case 2: y--; break;
            case 3: y++; break; case 4: z--; break; default: z++; break;
        }
    }
}

static void vw_generate(void)
{
    int const sea = vw_sz/4;
    vw_hmap = (int *)Xmalloc(sizeof(int) * vw_sx * vw_sy);

    for (int x = 0; x < vw_sx; x++)
        for (int y = 0; y < vw_sy; y++)
        {
            float const hills = vw_fbm(x / 64.f, y / 64.f, vw_seed, 4);
            float const rough = vw_fbm(x / 16.f, y / 16.f, vw_seed + 7, 3);
            int h = sea + (int)(hills*hills * vw_sz*0.55f + rough * 4.f) - 4;
            h = clamp(h, 2, vw_sz - 12);

            uint8_t *b = vw_colblocks(x, y);
            for (int z = 0; z <= h; z++)
            {
                uint8_t t = VW_STONE;
                if (z == 0) t = VW_BEDROCK;
                else if (z > h - 4) t = (h <= sea + 1) ? VW_SAND : (z == h ? VW_GRASS : VW_DIRT);
                b[z] = t;
            }
            vw_hmap[x*vw_sy + y] = h;
        }

    // Caves: wandering tunnels with the odd cavern, a few of them opening onto the surface.
    vwrng_t rng = { vw_seed * 2654435761u + 12345u };
    rng.next();
    int const area = vw_sx * vw_sy;
    for (int i = 0, n = max(1, area / 1100); i < n; i++)
    {
        float const x = rng.range(4, vw_sx - 5) + 0.5f, y = rng.range(4, vw_sy - 5) + 0.5f;
        int const h = vw_height((int)x, (int)y);
        vw_worm(rng, x, y, rng.range(3, max(3, h - 8)), rng.range(60, 180), false);
    }
    for (int i = 0, n = max(1, area / 4000); i < n; i++)
    {
        // the first cave mouth is just ahead of the spawn point (a little to the left), so there's always one to find
        float const x = i ? rng.range(8, vw_sx - 9) + 0.5f : vw_sx/2 + 16.5f;
        float const y = i ? rng.range(8, vw_sy - 9) + 0.5f : vw_sy/2 - 5.5f;
        vw_worm(rng, x, y, vw_height((int)x, (int)y) - 1, rng.range(80, 200), true, i ? -1.f : 0.f);
    }

    // Ores and gravel (only replace stone, so they line cave walls rather than floating in them).
    for (int i = 0, n = area / 35; i < n; i++)
        vw_vein(rng, VW_COAL, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(2, vw_sz*6/10), rng.range(5, 10), false);
    for (int i = 0, n = area / 70; i < n; i++)
        vw_vein(rng, VW_IRON, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(2, sea), rng.range(4, 8), false);
    for (int i = 0, n = area / 350; i < n; i++)
        vw_vein(rng, VW_GOLD, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(1, max(2, sea/2)), rng.range(3, 6), false);
    for (int i = 0, n = area / 250; i < n; i++)
        vw_vein(rng, VW_GRAVEL, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(2, sea + 8), rng.range(15, 30), true);

    // Trees.
    for (int x = 3; x < vw_sx - 3; x++)
        for (int y = 3; y < vw_sy - 3; y++)
        {
            if (vw_hash(x, y, vw_seed + 99) % 97) continue;
            int const s = vw_surface(x, y);
            if (s < 0 || vw_colblocks(x, y)[s] != VW_GRASS) continue;
            int const trunk = 4 + vw_hash(x, y, vw_seed + 5) % 3;
            for (int z = 1; z <= trunk; z++) vw_put(x, y, s + z, VW_WOOD);
            for (int dz = trunk - 2; dz <= trunk + 1; dz++)
            {
                int const r = (dz > trunk - 1) ? 1 : 2;
                for (int dx = -r; dx <= r; dx++)
                    for (int dy = -r; dy <= r; dy++)
                        if ((dx || dy || dz > trunk) && abs(dx) + abs(dy) <= r + 1 && voxworld_get(x+dx, y+dy, s+dz) == VW_AIR)
                            vw_put(x + dx, y + dy, s + dz, VW_LEAVES);
            }
        }

    // A test structure near the centre: a planks arch with a cobble floor, to show overhangs.
    {
        int const cx = vw_sx/2 + 6, cy = vw_sy/2;
        int const s = vw_surface(cx, cy) + 1;
        for (int dx = -4; dx <= 4; dx++)
            for (int dy = -1; dy <= 1; dy++)
            {
                vw_put(cx + dx, cy + dy, s - 1, VW_COBBLE);
                for (int z = s; z < s + 6; z++) vw_put(cx + dx, cy + dy, z, VW_AIR);
                vw_put(cx + dx, cy + dy, s + 4, VW_PLANKS);
                if (abs(dx) == 4) for (int z = s; z < s + 4; z++) vw_put(cx + dx, cy + dy, z, VW_PLANKS);
            }
        // a floating slab to look up at
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = 4; dy <= 7; dy++)
                vw_put(cx + dx, cy + dy, s + 3, VW_STONE);
    }

    for (int x = 0; x < vw_sx; x++)
        for (int y = 0; y < vw_sy; y++)
            vw_rebuildcolumn(x, y);

    DO_FREE_AND_NULL(vw_hmap);
    vw_computelight();
}

// Highest walkable surface z (Build coords) at a world position, or INT32_MAX if none.
static int32_t vw_groundz(int32_t x, int32_t y)
{
    int bx, by, bz;
    voxworld_toblock(x, y, 0, &bx, &by, &bz);
    if (!vw_inxy(bx, by)) return INT32_MAX;
    vwcolumn_t const *c = vw_col(bx, by);
    if (!c->nruns) return INT32_MAX;
    return vw_layertop(c->runs[c->nruns-1].z1);
}

void voxworld_onmapload(vec3_t *dapos)
{
    vw_active = false;
    vw_sect = -1;

    if (numsectors != 1 || sector[0].extra != VW_MAP_MAGIC)
        return;

    auto const &sec = sector[0];

    int32_t minx = INT32_MAX, miny = INT32_MAX, maxx = INT32_MIN, maxy = INT32_MIN;
    for (int i = sec.wallptr; i < sec.wallptr + sec.wallnum; i++)
    {
        minx = min<int32_t>(minx, wall[i].x); maxx = max<int32_t>(maxx, wall[i].x);
        miny = min<int32_t>(miny, wall[i].y); maxy = max<int32_t>(maxy, wall[i].y);
    }

    vw_sect = 0;
    vw_x0   = minx;
    vw_y0   = miny;
    vw_sx   = clamp((maxx - minx) / VW_BLOCKXY, 1, 1024);
    vw_sy   = clamp((maxy - miny) / VW_BLOCKXY, 1, 1024);
    vw_sz   = clamp((sec.floorz - sec.ceilingz) / VW_BLOCKZ - 2, 8, 255);
    vw_zbot = sec.floorz;
    vw_seed = (uint16_t)sec.hitag;

    Xfree(vw_blocks);
    Xfree(vw_cols);
    DO_FREE_AND_NULL(vw_dmg);
    vw_blocks = (uint8_t *)Xcalloc((size_t)vw_sx*vw_sy*vw_sz, 1);
    vw_cols   = (vwcolumn_t *)Xcalloc((size_t)vw_sx*vw_sy, sizeof(vwcolumn_t));

    vw_updatelightshades();
    vw_active = true;  // generation uses voxworld_get
    double const t0 = timerGetFractionalTicks();
    vw_generate();
    LOG_F(INFO, "voxworld: %dx%dx%d blocks, seed %u, generated in %.1f ms", vw_sx, vw_sy, vw_sz, vw_seed, timerGetFractionalTicks() - t0);

    // Stand everything that starts in the world on the ground.
    for (int i = 0; i < MAXSPRITES; i++)
    {
        if (sprite[i].statnum == MAXSTATUS || sprite[i].sectnum != vw_sect) continue;
        int32_t const gz = vw_groundz(sprite[i].x, sprite[i].y);
        if (gz != INT32_MAX) sprite[i].z = gz;
    }
    if (dapos)
    {
        int32_t const gz = vw_groundz(dapos->x, dapos->y);
        if (gz != INT32_MAX) dapos->z = gz - (40<<8);
    }

    vw_loadtextures();
    vw_registerosd();
}

//////////////////////////////////////////////////////////////////////////////
// Rendering
//////////////////////////////////////////////////////////////////////////////

// Column-major buffers: one screen column is contiguous, which is what the column renderer walks.
static float   *vw_zbuf;     // depth (forward distance, Build xy units), [x*ydimen + y]
static uint8_t *vw_cbuf;     // colour, [x*ydimen + y]; transposed into the frame at the end
static float   *vw_rowrec;   // per row: 1 / (y + 0.5 - horizon)
static int      vw_bufw, vw_bufh;

// Per-frame camera, in floats.
static float vw_cx, vw_cy, vw_cz;           // camera position
static float vw_ca, vw_sa;                  // cos/sin of view angle
static float vw_vrf;                        // viewingrange / 65536
static float vw_ky;                         // screen rows per (z-units / depth)
static float vw_horiz;                      // horizon row
static float vw_visk;                       // shade steps per unit depth
static float vw_maxdist;                    // view distance (xy units)

#define VW_NEAR 8.f

static int vw_viewdist = 96;  // blocks; OSD: r_voxdist
static int vw_stats;          // OSD: r_voxstats

// Face shading offsets (Build shade units, 0..numshades-1): fake directional light.
static const int vw_faceshade[6] = { 3, 3, 1, 1, 0, 8 };  // -x, +x, -y, +y, top, bottom

struct vwgap_t { int16_t a, b; };  // open rows [a, b)
#define VW_MAXGAPS 64

// Remove [a,b) from the gap list; write the visible pieces to vis[] and return how many.
static FORCE_INLINE int vw_clip(vwgap_t *gaps, int &ng, int a, int b, vwgap_t *vis)
{
    int nv = 0;
    for (int i = 0; i < ng; i++)
    {
        vwgap_t &g = gaps[i];
        if (b <= g.a) break;          // gaps are sorted; nothing further overlaps
        if (a >= g.b) continue;

        int const va = max<int>(a, g.a), vb = min<int>(b, g.b);
        vis[nv++] = { (int16_t)va, (int16_t)vb };

        bool const keepTop = g.a < va, keepBot = vb < g.b;
        if (keepTop && keepBot)
        {
            if (ng < VW_MAXGAPS)
            {
                Bmemmove(&gaps[i+2], &gaps[i+1], (ng - i - 1)*sizeof(vwgap_t));
                gaps[i+1] = { (int16_t)vb, g.b };
                g.b = va;
                ng++;
                i++;
            }
            else
                g.b = va;  // out of gap slots: drop the lower piece (never seen in practice)
        }
        else if (keepTop) g.b = va;
        else if (keepBot) g.a = vb;
        else
        {
            Bmemmove(&gaps[i], &gaps[i+1], (ng - i - 1)*sizeof(vwgap_t));
            ng--;
            i--;
        }
    }
    return nv;
}

// "Lantern": darkness from missing light fades in with distance, so what's right in front of you is always visible
// (the look of a dark Doom/Duke room) while deep caves still fall away to black.
static int   vw_lantern = 6;       // blocks until darkness is at full strength (0 = off); OSD: r_voxlantern
static float vw_lanterninv;        // 1 / lantern distance in xy units

static FORCE_INLINE int vw_lanterned(int light, float depth)
{
    float const f = depth * vw_lanterninv;
    return f >= 1.f ? light : (int)(light * f);
}

static FORCE_INLINE uint8_t const *vw_shadetab(int shade, int light, float depth)
{
    int s = shade + vw_lanterned(light, depth) + (int)(depth * vw_visk);
    s = clamp(s, 0, numshades - 1);
    return (uint8_t const *)palookup[0] + (s << 8);
}

static FORCE_INLINE int vw_rowof(float y) { return (int)ceilf(y - 0.5f); }  // first row whose centre is >= y

// Horizontal face (top or bottom) at height hz/vw_ky above/below the eye, between depths [d0, d1].
static __attribute__((noinline)) void vw_drawflat(uint8_t *col, float *zc, vwgap_t const *vis, int nv, float hz, float d0, float d1,
                                     float dx, float dy, uint8_t const *tex, int faceshade, int light)
{
    float const ox = vw_cx - vw_x0, oy = vw_cy - vw_y0;
    uint8_t const *const pal0 = (uint8_t const *)palookup[0];
    int const maxshade = numshades - 1;

    for (int vi = 0; vi < nv; vi++)
        for (int y = vis[vi].a; y < vis[vi].b; y++)
        {
            float d = hz * vw_rowrec[y];
            d = d < d0 ? d0 : (d > d1 ? d1 : d);
            int const u = ((int)(ox + dx*d) >> 5) & (VW_TEXSIZE-1);
            int const v = ((int)(oy + dy*d) >> 5) & (VW_TEXSIZE-1);
            int s = faceshade + vw_lanterned(light, d) + (int)(d * vw_visk);
            s = s > maxshade ? maxshade : s;
            col[y] = pal0[(s << 8) + tex[u*VW_TEXSIZE + v]];
            zc[y] = d;
        }
}

// Most common colour along the top edge of a sky tile (cached per tile).
static uint8_t vw_skytopcolor(int tile)
{
    static int cachedtile = -1;
    static uint8_t cached;
    if (tile == cachedtile) return cached;
    int const tw = tilesiz[tile].x, th = tilesiz[tile].y;
    uint8_t const *const p = (uint8_t const *)waloff[tile];
    int count[256] = {};
    for (int u = 0; u < tw; u++)
        for (int v = 0; v < min(th, 4); v++)
            count[p[u*th + v]]++;
    int best = 0;
    for (int c = 1; c < 256; c++)
        if (count[c] > count[best]) best = c;
    cachedtile = tile;
    return cached = (uint8_t)best;
}

static __attribute__((noinline)) void vw_drawcolumn(int sx, uint8_t *col, float *zc)
{
    int const ydim = ydimen;

    vwgap_t gaps[VW_MAXGAPS];
    int ng = 1;
    gaps[0] = { 0, (int16_t)ydim };

    float const k  = vw_vrf * ((sx + 0.5f) / (float)halfxdimen - 1.f);
    float const dx = vw_ca - vw_sa*k;   // world xy direction per unit forward depth
    float const dy = vw_sa + vw_ca*k;

    // DDA in block units.
    float const px = (vw_cx - vw_x0) / VW_BLOCKXY, py = (vw_cy - vw_y0) / VW_BLOCKXY;
    int gx = (int)floorf(px), gy = (int)floorf(py);
    int const stepx = dx >= 0 ? 1 : -1, stepy = dy >= 0 ? 1 : -1;
    float const tdx = dx != 0.f ? fabsf(VW_BLOCKXY / dx) : FLT_MAX;
    float const tdy = dy != 0.f ? fabsf(VW_BLOCKXY / dy) : FLT_MAX;
    float tmx = dx != 0.f ? ((stepx > 0 ? (gx + 1 - px) : (px - gx)) * tdx) : FLT_MAX;
    float tmy = dy != 0.f ? ((stepy > 0 ? (gy + 1 - py) : (py - gy)) * tdy) : FLT_MAX;

    float din = 0.f;
    int side = -1;  // face entered through: 0..3 as voxworld.h (face of the block we're entering)

    float const eyelayer = (vw_zbot - vw_cz) / VW_BLOCKZ;
    vwgap_t vis[VW_MAXGAPS];

    for (;;)
    {
        float const dout = min(tmx, tmy);

        if (vw_inxy(gx, gy))
        {
            vwcolumn_t const *c = vw_col(gx, gy);
            int const nr = c->nruns;
            // Cheap reject: if even the top of this block column projects below every open row, skip it.
            // (The top face's far edge is its highest point when it's below the eye, else the near edge.)
            float const colztop = nr ? (float)vw_layertop(c->runs[nr-1].z1) : 0.f;
            if (nr && din > VW_NEAR)
            {
                float const dz = colztop - vw_cz;
                float const ytop = vw_horiz + dz * vw_ky / (dz > 0.f ? dout : din);
                if (ytop >= gaps[ng-1].b)
                    goto next_cell;
            }
            if (nr)
            {
                // Runs at/below eye level going down, then above eye level going up: within a cell,
                // that is front-to-back for every ray.
                int split = 0;
                while (split < nr && c->runs[split].z0 <= eyelayer) split++;  // runs[0..split) start at/below eye

                uint8_t const *cb = vw_colblocks(gx, gy);
                float const dclip = max(din, VW_NEAR);

                for (int pass = 0; pass < nr; pass++)
                {
                    int const ri = pass < split ? split - 1 - pass : pass;
                    vwrun_t const r = c->runs[ri];
                    float const ztop = (float)vw_layertop(r.z1);  // smaller Build z = higher
                    float const zbot = (float)vw_layertop(r.z0);

                    // Front (vertical) face at din: constant depth down the column, like a Build wall.
                    if (din > VW_NEAR && side >= 0)
                    {
                        float const inv = vw_ky / din;
                        int const a = max(vw_rowof(vw_horiz + (ztop - vw_cz)*inv), 0);
                        int const b = min(vw_rowof(vw_horiz + (zbot - vw_cz)*inv), ydim);
                        if (a < b)
                        {
                            int const nv = vw_clip(gaps, ng, a, b, vis);
                            if (nv)
                            {
                                // texture column from the hit point along the face
                                float const hx = vw_cx + dx*din, hy = vw_cy + dy*din;
                                float const along = (side < 2) ? (hy - vw_y0) : (hx - vw_x0);
                                int u = ((int)along >> 5) & (VW_TEXSIZE-1);
                                if (side == 0 || side == 3) u = VW_TEXSIZE-1 - u;  // keep textures unmirrored

                                uint8_t const *pal = nullptr;
                                int const ucol = u*VW_TEXSIZE;
                                size_t const colidx = vw_index(gx, gy, 0);
                                // the face is lit by the cell in front of it (the one the ray came from)
                                int const lx = gx - (side == 0) + (side == 1), ly = gy - (side == 2) + (side == 3);

                                // Height above the world floor in 16.16 texels, stepping down the screen.
                                float const zperrow = din / vw_ky;
                                float const texperz = (float)VW_TEXSIZE / VW_BLOCKZ;
                                int32_t const hstep = (int32_t)(zperrow * texperz * 65536.f);
                                int const tmin = r.z0 * VW_TEXSIZE, tmax = r.z1 * VW_TEXSIZE - 1;

                                for (int vi = 0; vi < nv; vi++)
                                {
                                    int const ya = vis[vi].a, yb = vis[vi].b;
                                    float const z = vw_cz + (ya + 0.5f - vw_horiz)*zperrow;
                                    int32_t h = (int32_t)((vw_zbot - z) * texperz * 65536.f);

                                    int curlayer = -1;
                                    uint8_t const *tex = nullptr;

                                    for (int y = ya; y < yb; y++, h -= hstep)
                                    {
                                        int t = h >> 16;
                                        t = t < tmin ? tmin : (t > tmax ? tmax : t);
                                        int const layer = t >> 4;
                                        if (layer != curlayer)
                                        {
                                            curlayer = layer;
                                            tex = vw_tex[vw_blockdefs[cb[layer]].side] + ucol;
                                            pal = vw_shadetab(vw_faceshade[side] + vw_dmgshade(colidx + layer, cb[layer]),
                                                              vw_lightshade(lx, ly, layer), din);
                                        }
                                        col[y] = pal[tex[VW_TEXSIZE-1 - (t & (VW_TEXSIZE-1))]];
                                        zc[y] = din;
                                    }
                                }
                            }
                        }
                    }

                    // Top face (seen from above).
                    if (ztop > vw_cz && ng)
                    {
                        float const hz = (ztop - vw_cz) * vw_ky;  // y-horizon = hz / depth
                        int const a = max(vw_rowof(vw_horiz + hz / dout), 0);
                        int const b = min(vw_rowof(vw_horiz + hz / dclip), ydim);
                        if (a < b)
                        {
                            int const nv = vw_clip(gaps, ng, a, b, vis);
                            if (nv)
                                vw_drawflat(col, zc, vis, nv, hz, dclip, dout, dx, dy,
                                            vw_tex[vw_blockdefs[cb[r.z1-1]].top],
                                            vw_faceshade[4] + vw_dmgshade(vw_index(gx, gy, r.z1-1), cb[r.z1-1]),
                                            vw_lightshade(gx, gy, r.z1));
                        }
                    }

                    // Bottom face (seen from below).
                    if (zbot < vw_cz && ng)
                    {
                        float const hz = (zbot - vw_cz) * vw_ky;  // negative
                        int const a = max(vw_rowof(vw_horiz + hz / dclip), 0);
                        int const b = min(vw_rowof(vw_horiz + hz / dout), ydim);
                        if (a < b)
                        {
                            int const nv = vw_clip(gaps, ng, a, b, vis);
                            if (nv)
                                vw_drawflat(col, zc, vis, nv, hz, dclip, dout, dx, dy,
                                            vw_tex[vw_blockdefs[cb[r.z0]].bottom],
                                            vw_faceshade[5] + vw_dmgshade(vw_index(gx, gy, r.z0), cb[r.z0]),
                                            vw_lightshade(gx, gy, r.z0 - 1));
                        }
                    }

                    if (!ng) break;
                }
            }
        }
        else
        {
            // Outside the world: stop once we're heading away from it.
            if ((gx < 0 && stepx < 0) || (gx >= vw_sx && stepx > 0) || (gy < 0 && stepy < 0) || (gy >= vw_sy && stepy > 0))
                break;
        }

next_cell:
        if (!ng || dout > vw_maxdist) break;

        din = dout;
        if (tmx < tmy) { gx += stepx; tmx += tdx; side = stepx > 0 ? 0 : 1; }
        else           { gy += stepy; tmy += tdy; side = stepy > 0 ? 2 : 3; }
    }

    // Whatever is left is sky.
    if (ng)
    {
        auto const &sec = sector[vw_sect];
        int const skytile = sec.ceilingpicnum;
        uint8_t const *const pal = (uint8_t const *)palookup[sec.ceilingpal ? sec.ceilingpal : 0] + (clamp<int>(sec.ceilingshade, 0, numshades-1) << 8);
        int const tw = tilesiz[skytile].x, th = tilesiz[skytile].y;
        uint8_t const *const sky = (uint8_t const *)waloff[skytile];

        if (sky && tw > 0 && th > 0)
        {
            // Wrap the sky tile 4 times around the horizon, horizon at 3/4 of its height. Above the tile's
            // top edge, fill with the tile's dominant top-row colour instead of stretching each column's top pixel.
            float const ang = atan2f(dy, dx);
            int const u = ((int)((ang * (1.f / (2.f*fPI)) + 1.f) * 4.f * tw)) % tw;
            float const vscale = th / (ydim * 0.9f);
            uint8_t const *const scol = sky + u*th;
            uint8_t const top = pal[vw_skytopcolor(skytile)];
            for (int i = 0; i < ng; i++)
                for (int y = gaps[i].a; y < gaps[i].b; y++)
                {
                    int const v = (int)(th*0.75f + (y - vw_horiz)*vscale);
                    col[y] = v < 0 ? top : pal[scol[min(v, th-1)]];
                    zc[y] = FLT_MAX;
                }
        }
        else
        {
            for (int i = 0; i < ng; i++)
                for (int y = gaps[i].a; y < gaps[i].b; y++)
                {
                    col[y] = 0;
                    zc[y] = FLT_MAX;
                }
        }
    }
}

static void vw_setupcamera(void)
{
    vw_cx = (float)globalposx;
    vw_cy = (float)globalposy;
    vw_cz = (float)globalposz;

    float const angrad = fix16_to_float(qglobalang) * (fPI / 1024.f);
    vw_ca = cosf(angrad);
    vw_sa = sinf(angrad);

    vw_vrf   = viewingrange * (1.f / 65536.f);
    vw_ky    = xdimenscale / (vw_vrf * 8192.f);
    vw_horiz = fix16_to_float(qglobalhoriz);

    // Same distance shading as Build's sprites: shade += (yp*xdimscale>>19)*globvis>>16>>8, yp = 256*depth*vrf.
    int32_t globvis = globalvisibility;
    if (sector[vw_sect].visibility != 0) globvis = mulscale4(globvis, (uint8_t)(sector[vw_sect].visibility + 16));
    vw_visk = 256.f * vw_vrf * (float)xdimscale * (float)globvis / (float)(1ll << 43);

    vw_maxdist = (float)vw_viewdist * VW_BLOCKXY;
    vw_lanterninv = vw_lantern > 0 ? 1.f / (vw_lantern * VW_BLOCKXY) : 1e9f;

    for (int y = 0; y < ydimen; y++)
    {
        float const d = y + 0.5f - vw_horiz;
        vw_rowrec[y] = (fabsf(d) < 1e-3f) ? 1e3f : 1.f / d;
    }
}

static __attribute__((noinline)) void vw_transpose(void)
{
    int const ydim = ydimen;
    // Transpose the column-major colour buffer into the frame, in cache-friendly tiles.
    uint8_t *const fb = (uint8_t *)frameoffset;
    int const bpl = bytesperline;
    int const T = 32;
    for (int x0 = 0; x0 < xdimen; x0 += T)
    {
        int const x1 = min<int>(x0 + T, xdimen);
        for (int y0 = 0; y0 < ydim; y0 += T)
        {
            int const y1 = min(y0 + T, ydim);
            for (int y = y0; y < y1; y++)
            {
                uint8_t *const row = fb + y*bpl;
                uint8_t const *src = vw_cbuf + y;
                for (int x = x0; x < x1; x++)
                    row[x] = src[x*ydim];
            }
        }
    }
}

void voxworld_drawworld(void)
{
    if (!vw_active) return;

    if (vw_bufw != xdimen || vw_bufh != ydimen)
    {
        Xfree(vw_zbuf); Xfree(vw_cbuf); Xfree(vw_rowrec);
        vw_bufw = xdimen; vw_bufh = ydimen;
        vw_zbuf   = (float *)Xmalloc(sizeof(float) * xdimen * ydimen);
        vw_cbuf   = (uint8_t *)Xmalloc(xdimen * ydimen);
        vw_rowrec = (float *)Xmalloc(sizeof(float) * ydimen);
    }

    vw_setupcamera();

    auto const &sec = sector[vw_sect];
    if (!waloff[sec.ceilingpicnum]) tileLoad(sec.ceilingpicnum);

    int const ydim = ydimen;
    double const t0 = timerGetFractionalTicks();
    for (int x = 0; x < xdimen; x++)
        vw_drawcolumn(x, vw_cbuf + x*ydim, vw_zbuf + x*ydim);
    double const t1 = timerGetFractionalTicks();
    vw_transpose();
    double const t2 = timerGetFractionalTicks();

    // timing, printed by `r_voxstats 1`
    static double acc[2]; static int frames;
    acc[0] += t1 - t0; acc[1] += t2 - t1;
    if (++frames == 60)
    {
        if (vw_stats)
            LOG_F(INFO, "voxworld: columns %.2f ms, transpose %.2f ms (avg of 60 frames, %dx%d)", acc[0]/60, acc[1]/60, xdimen, ydim);
        acc[0] = acc[1] = 0; frames = 0;
    }
}

//////////////////////////////////////////////////////////////////////////////
// Sprites (face sprites, depth-tested against the block world)
//////////////////////////////////////////////////////////////////////////////

struct vwsprite_t { int idx; float depth; };

static void vw_drawfacesprite(tspriteptr_t tspr, float depth)
{
    int tilenum = tspr->picnum;
    tileUpdatePicnum(&tilenum, tspr->owner + 32768);
    if ((unsigned)tilenum >= MAXTILES) return;

    // Debris convention: a sprite whose lotag is VW_DEBRIS_TAG|type is drawn as a chunk of that block.
    if ((unsigned)tspr->owner < MAXSPRITES && (sprite[tspr->owner].lotag & 0xFF00) == VW_DEBRIS_TAG)
    {
        int const type = sprite[tspr->owner].lotag & 0xFF;
        if (type > VW_AIR && type < VW_NUMTYPES)
            tilenum = VW_TILEBASE + vw_blockdefs[type].side;
    }

    int const spanx = tilesiz[tilenum].x, spany = tilesiz[tilenum].y;
    if (spanx <= 0 || spany <= 0 || !tspr->xrepeat || !tspr->yrepeat) return;
    if (!waloff[tilenum] && !tileLoad(tilenum)) return;
    uint8_t const *const pix = (uint8_t const *)waloff[tilenum];
    if (!pix) return;

    int const cstat = tspr->cstat;

    float const relx = tspr->x - vw_cx, rely = tspr->y - vw_cy;
    float const lat = -relx*vw_sa + rely*vw_ca;

    // Build's face sprite sizing (see classicDrawSprite), in pixels.
    float const sxc   = halfxdimen * (1.f + lat / (depth * vw_vrf));
    float const pxw   = (float)xdimenscale * (float)xyaspect * tspr->xrepeat / (depth * vw_vrf * (float)(1 << 27));  // per texel
    float const pxh   = (float)xdimenscale * tspr->yrepeat / (depth * vw_vrf * 2048.f);                            // per texel

    int offx = picanm[tilenum].xofs + tspr->xoffset;
    int offy = picanm[tilenum].yofs + tspr->yoffset;

    float x1 = sxc - spanx*pxw*0.5f;
    if (cstat & 4) x1 += offx*pxw; else x1 -= offx*pxw;
    float ybot = vw_horiz + (tspr->z - vw_cz) * vw_ky / depth - offy*pxh;
    if (cstat & 128) ybot += spany*pxh*0.5f;
    float const ytop = ybot - spany*pxh;

    int const lx = max(vw_rowof(x1), 0), rx = min(vw_rowof(x1 + spanx*pxw), (int)xdimen);
    int const ty = max(vw_rowof(ytop), 0), by = min(vw_rowof(ybot), (int)ydimen);
    if (lx >= rx || ty >= by) return;

    int pal = tspr->pal;
    if (palookup[pal] == NULL) pal = 0;
    int shade = tspr->shade + (int)(depth * vw_visk) + vw_lanterned(voxworld_lightshade(tspr->x, tspr->y, tspr->z - VW_BLOCKZ/2), depth);
    shade = clamp(shade, 0, numshades - 1);
    uint8_t const *const lut = (uint8_t const *)palookup[pal] + (shade << 8);

    uint8_t const *trans = nullptr;
    bool transrev = false;
    if (cstat & 2)
    {
        int blend = tspr->blend;
        if (!blendtable[blend]) blend = 0;
        trans = (uint8_t const *)blendtable[blend];
        transrev = (cstat & 512) != 0;
    }

    uint8_t *const fb = (uint8_t *)frameoffset;
    int const bpl = bytesperline;
    float const invw = 1.f / pxw, invh = 1.f / pxh;

    // Like Build, a sprite whose centre is in front of a surface draws over it: test against the depth of
    // the sprite's nearest point (treating it as roughly as deep as it is wide), capped at a quarter block.
    float const zdepth = depth - min(spanx * tspr->xrepeat * (1.f/8.f), (float)VW_BLOCKXY / 4);

    for (int x = lx; x < rx; x++)
    {
        int u = (int)((x + 0.5f - x1) * invw);
        u = clamp(u, 0, spanx - 1);
        if (cstat & 4) u = spanx - 1 - u;
        uint8_t const *const col = pix + u*spany;

        for (int y = ty; y < by; y++)
        {
            float *const zp = &vw_zbuf[x*ydimen + y];
            if (zdepth >= *zp) continue;

            int v = (int)((y + 0.5f - ytop) * invh);
            v = clamp(v, 0, spany - 1);
            if (cstat & 8) v = spany - 1 - v;

            uint8_t const c = col[v];
            if (c == 255) continue;

            uint8_t *const p = &fb[y*bpl + x];
            if (trans)
                *p = transrev ? trans[(lut[c] << 8) | *p] : trans[(*p << 8) | lut[c]];
            else
            {
                *p = lut[c];
                *zp = depth;
            }
        }
    }
}

void voxworld_drawsprites(void)
{
    if (!vw_active || !vw_zbuf) { spritesortcnt = 0; return; }

    static vwsprite_t list[MAXSPRITESONSCREEN];
    int n = 0;

    for (int i = 0; i < spritesortcnt; i++)
    {
        tspriteptr_t const t = &tsprite[i];
        if (t->owner < 0 || (unsigned)t->picnum >= MAXTILES) continue;
        if ((t->cstat & CSTAT_SPRITE_ALIGNMENT) != CSTAT_SPRITE_ALIGNMENT_FACING) continue;  // wall/floor sprites: TODO
        if (t->cstat & CSTAT_SPRITE_INVISIBLE) continue;
        float const depth = (t->x - vw_cx)*vw_ca + (t->y - vw_cy)*vw_sa;
        if (depth < 64.f) continue;
        list[n++] = { i, depth };
    }

    // Far to near, so translucent sprites blend over what's behind them.
    std::sort(list, list + n, [](vwsprite_t const &a, vwsprite_t const &b) { return a.depth > b.depth; });

    for (int i = 0; i < n; i++)
        vw_drawfacesprite(&tsprite[list[i].idx], list[i].depth);

    spritesortcnt = 0;
}

//////////////////////////////////////////////////////////////////////////////
// Collision
//////////////////////////////////////////////////////////////////////////////

// Does any block in column (bx,by) overlap the vertical range (ztop, zbot) (Build z, ztop < zbot)?
static bool vw_colblocks_z(int bx, int by, int32_t ztop, int32_t zbot)
{
    if (!vw_inxy(bx, by)) return false;
    vwcolumn_t const *c = vw_col(bx, by);
    for (int i = 0; i < c->nruns; i++)
    {
        int32_t const rt = vw_layertop(c->runs[i].z1), rb = vw_layertop(c->runs[i].z0);
        if (rt < zbot && rb > ztop) return true;
    }
    return false;
}

static FORCE_INLINE int vw_bx(int32_t x) { return (int)floorf((float)(x - vw_x0) / VW_BLOCKXY); }
static FORCE_INLINE int vw_by(int32_t y) { return (int)floorf((float)(y - vw_y0) / VW_BLOCKXY); }

int voxworld_clipmove(vec3_t *pos, int32_t *xvect, int32_t *yvect, int32_t walldist, int32_t ceildist, int32_t flordist)
{
    if (!vw_active) return 0;

    // Vertical extent that collides: blocks whose top is above (pos.z + flordist) and whose bottom is
    // below (pos.z - ceildist) block movement. (Same rule as Build's cliptestsector.)
    int32_t const ztop = pos->z - ceildist + 1, zbot = pos->z + flordist - 1;
    int32_t const r = walldist;
    int hit = 0;

    int32_t dx = *xvect >> 14, dy = *yvect >> 14;

    // X axis
    if (dx)
    {
        int32_t const lead = pos->x + (dx > 0 ? r : -r);
        int const g0 = vw_bx(lead), g1 = vw_bx(lead + dx);
        int const ylo = vw_by(pos->y - r), yhi = vw_by(pos->y + r - 1);
        for (int gx = g0 + (dx > 0 ? 1 : -1); dx > 0 ? gx <= g1 : gx >= g1; gx += (dx > 0 ? 1 : -1))
        {
            bool blocked = false;
            for (int gy = ylo; gy <= yhi && !blocked; gy++)
                blocked = vw_colblocks_z(gx, gy, ztop, zbot);
            if (blocked)
            {
                int32_t const face = vw_x0 + (dx > 0 ? gx : gx + 1) * VW_BLOCKXY;
                dx = (dx > 0) ? (face - r - 1) - pos->x : (face + r + 1) - pos->x;
                if ((dx > 0) != (*xvect > 0)) dx = 0;
                *xvect = dx << 14;
                hit = 1;
                break;
            }
        }
    }

    // Y axis (after X)
    if (dy)
    {
        int32_t const nx = pos->x + dx;
        int32_t const lead = pos->y + (dy > 0 ? r : -r);
        int const g0 = vw_by(lead), g1 = vw_by(lead + dy);
        int const xlo = vw_bx(nx - r), xhi = vw_bx(nx + r - 1);
        for (int gy = g0 + (dy > 0 ? 1 : -1); dy > 0 ? gy <= g1 : gy >= g1; gy += (dy > 0 ? 1 : -1))
        {
            bool blocked = false;
            for (int gx = xlo; gx <= xhi && !blocked; gx++)
                blocked = vw_colblocks_z(gx, gy, ztop, zbot);
            if (blocked)
            {
                int32_t const face = vw_y0 + (dy > 0 ? gy : gy + 1) * VW_BLOCKXY;
                dy = (dy > 0) ? (face - r - 1) - pos->y : (face + r + 1) - pos->y;
                if ((dy > 0) != (*yvect > 0)) dy = 0;
                *yvect = dy << 14;
                hit = 1;
                break;
            }
        }
    }

    return hit;
}

int voxworld_getzrange(const vec3_t *pos, int32_t walldist, int32_t *ceilz, int32_t *florz)
{
    if (!vw_active) return 0;

    int ret = 0;
    int const x0 = vw_bx(pos->x - walldist), x1 = vw_bx(pos->x + walldist);
    int const y0 = vw_by(pos->y - walldist), y1 = vw_by(pos->y + walldist);
    int32_t const tol = 256;

    for (int gx = x0; gx <= x1; gx++)
        for (int gy = y0; gy <= y1; gy++)
        {
            if (!vw_inxy(gx, gy)) continue;
            vwcolumn_t const *c = vw_col(gx, gy);
            for (int i = 0; i < c->nruns; i++)
            {
                int32_t const rt = vw_layertop(c->runs[i].z1), rb = vw_layertop(c->runs[i].z0);
                if (rt >= pos->z - tol && rt < *florz) { *florz = rt; ret |= 1; }
                if (rb <= pos->z + tol && rb > *ceilz) { *ceilz = rb; ret |= 2; }
            }
        }

    return ret;
}

//////////////////////////////////////////////////////////////////////////////
// Ray casting (3D DDA). Works in "cube space": z divided by 16 so blocks are 512^3.
//////////////////////////////////////////////////////////////////////////////

bool voxworld_raycast(const vec3_t *start, int32_t vx, int32_t vy, int32_t vz, float maxt, vwhit_t *hit)
{
    if (!vw_active) return false;

    float const ox = (start->x - vw_x0) / (float)VW_BLOCKXY;
    float const oy = (start->y - vw_y0) / (float)VW_BLOCKXY;
    float const oz = (vw_zbot - start->z) / (float)VW_BLOCKZ;  // layers, up positive
    float const rx = vx / (float)VW_BLOCKXY, ry = vy / (float)VW_BLOCKXY, rz = -vz / (float)VW_BLOCKZ;

    int bx = (int)floorf(ox), by = (int)floorf(oy), bz = (int)floorf(oz);
    int const sx = rx >= 0 ? 1 : -1, sy = ry >= 0 ? 1 : -1, sz = rz >= 0 ? 1 : -1;
    float const tdx = rx != 0 ? fabsf(1.f / rx) : FLT_MAX;
    float const tdy = ry != 0 ? fabsf(1.f / ry) : FLT_MAX;
    float const tdz = rz != 0 ? fabsf(1.f / rz) : FLT_MAX;
    float tmx = rx != 0 ? (sx > 0 ? (bx + 1 - ox) : (ox - bx)) * tdx : FLT_MAX;
    float tmy = ry != 0 ? (sy > 0 ? (by + 1 - oy) : (oy - by)) * tdy : FLT_MAX;
    float tmz = rz != 0 ? (sz > 0 ? (bz + 1 - oz) : (oz - bz)) * tdz : FLT_MAX;

    float t = 0.f;
    int face = -1;

    for (int steps = 0; steps < 4096; steps++)
    {
        if (face >= 0)  // don't count the block we start in
        {
            uint8_t const b = voxworld_get(bx, by, bz);
            if (b != VW_AIR && bz >= 0)
            {
                if (hit)
                {
                    hit->bx = bx; hit->by = by; hit->bz = bz;
                    hit->face = face;
                    hit->dist = t;
                    hit->pos = { start->x + (int32_t)(vx * t), start->y + (int32_t)(vy * t), start->z + (int32_t)(vz * t) };
                }
                return true;
            }
        }

        // Left the world for good?
        if ((bx < 0 && sx < 0) || (bx >= vw_sx && sx > 0) || (by < 0 && sy < 0) || (by >= vw_sy && sy > 0) || (bz >= vw_sz && sz > 0))
            return false;

        if (tmx < tmy && tmx < tmz) { t = tmx; bx += sx; tmx += tdx; face = sx > 0 ? 0 : 1; }
        else if (tmy < tmz)         { t = tmy; by += sy; tmy += tdy; face = sy > 0 ? 2 : 3; }
        else                        { t = tmz; bz += sz; tmz += tdz; face = sz > 0 ? 5 : 4; }

        if (t > maxt) return false;
    }
    return false;
}

bool voxworld_cansee(int32_t x1, int32_t y1, int32_t z1, int32_t x2, int32_t y2, int32_t z2)
{
    if (!vw_active) return true;
    vec3_t const s = { x1, y1, z1 };
    return !voxworld_raycast(&s, x2 - x1, y2 - y1, z2 - z1, 1.f, nullptr);
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// Destruction
//////////////////////////////////////////////////////////////////////////////

static struct { bool valid; vec3_t pos; int bx, by, bz; } vw_lasthit;

void voxworld_notehit(const vec3_t *pos, int bx, int by, int bz)
{
    vw_lasthit.valid = true;
    vw_lasthit.pos = *pos;
    vw_lasthit.bx = bx; vw_lasthit.by = by; vw_lasthit.bz = bz;
}

bool voxworld_lasthit(const vec3_t *pos, int *bx, int *by, int *bz)
{
    if (!vw_active || !vw_lasthit.valid || vw_lasthit.pos.x != pos->x || vw_lasthit.pos.y != pos->y || vw_lasthit.pos.z != pos->z)
        return false;
    *bx = vw_lasthit.bx; *by = vw_lasthit.by; *bz = vw_lasthit.bz;
    return true;
}

int voxworld_damage(int bx, int by, int bz, int amount)
{
    if (!vw_active || !vw_inxy(bx, by) || (unsigned)bz >= (unsigned)vw_sz || amount <= 0) return VW_AIR;
    size_t const idx = vw_index(bx, by, bz);
    uint8_t const type = vw_blocks[idx];
    if (type == VW_AIR) return VW_AIR;
    int const hardness = vw_blockdefs[type].hardness;
    if (!hardness) return VW_AIR;  // indestructible

    if (!vw_dmg) vw_dmg = (uint8_t *)Xcalloc((size_t)vw_sx*vw_sy*vw_sz, 1);

    int const total = vw_dmg[idx] + amount;
    if (total < hardness)
    {
        vw_dmg[idx] = (uint8_t)total;
        return VW_AIR;
    }

    vw_dmg[idx] = 0;
    voxworld_set(bx, by, bz, VW_AIR);
    return type;
}

int voxworld_hardness(int type) { return (type > VW_AIR && type < VW_NUMTYPES) ? vw_blockdefs[type].hardness : 0; }

void voxworld_blockcenter(int bx, int by, int bz, vec3_t *out)
{
    out->x = vw_x0 + bx*VW_BLOCKXY + VW_BLOCKXY/2;
    out->y = vw_y0 + by*VW_BLOCKXY + VW_BLOCKXY/2;
    out->z = vw_layertop(bz) - VW_BLOCKZ/2;
}

int voxworld_explode(const vec3_t *pos, int32_t radius, int damage, vwdestroyfn_t fn, void *ud)
{
    if (!vw_active || radius <= 0) return 0;

    // radius in xy units; the explosion is a sphere in cube space (z/16)
    float const cx = (pos->x - vw_x0) / (float)VW_BLOCKXY, cy = (pos->y - vw_y0) / (float)VW_BLOCKXY;
    float const cz = (vw_zbot - pos->z) / (float)VW_BLOCKZ;
    float const rb = (float)radius / VW_BLOCKXY;
    int destroyed = 0;

    for (int bx = (int)floorf(cx - rb); bx <= (int)floorf(cx + rb); bx++)
        for (int by = (int)floorf(cy - rb); by <= (int)floorf(cy + rb); by++)
            for (int bz = (int)floorf(cz - rb); bz <= (int)floorf(cz + rb); bz++)
            {
                if (!vw_inxy(bx, by) || (unsigned)bz >= (unsigned)vw_sz) continue;
                float const dx = bx + 0.5f - cx, dy = by + 0.5f - cy, dz = bz + 0.5f - cz;
                float const d = sqrtf(dx*dx + dy*dy + dz*dz);
                if (d >= rb) continue;
                int const amount = (int)(damage * (1.f - d / rb));
                int const type = voxworld_damage(bx, by, bz, amount);
                if (type != VW_AIR)
                {
                    destroyed++;
                    if (fn) fn(bx, by, bz, type, ud);
                }
            }
    return destroyed;
}

int32_t voxworld_floorz(int32_t x, int32_t y, int32_t z, int32_t def)
{
    if (!vw_active) return def;
    int bx, by, bz;
    voxworld_toblock(x, y, z, &bx, &by, &bz);
    if (!vw_inxy(bx, by)) return def;
    vwcolumn_t const *c = vw_col(bx, by);
    int32_t best = def;
    for (int i = 0; i < c->nruns; i++)
    {
        int32_t const top = vw_layertop(c->runs[i].z1);
        if (top >= z - 256 && top < best) best = top;
    }
    return best;
}

static int osdcmd_voxdist(osdcmdptr_t parm)
{
    if (parm->numparms == 1)
        vw_viewdist = clamp(Batol(parm->parms[0]), 8, 512);
    OSD_Printf("r_voxdist %d (blocks)\n", vw_viewdist);
    return OSDCMD_OK;
}

static int osdcmd_voxdark(osdcmdptr_t parm)
{
    if (parm->numparms == 1)
    {
        vw_darkness = clamp(Batol(parm->parms[0]), 0, 31);
        vw_updatelightshades();
    }
    OSD_Printf("r_voxdark %d (extra shade where no skylight reaches)\n", vw_darkness);
    return OSDCMD_OK;
}

// Debug: write a top-down map of the world as a PPM: terrain height in grey, caves tinted by depth
// (red = shallow, blue = deep), ores as dots. `r_voxdump [file]`.
static int osdcmd_voxdump(osdcmdptr_t parm)
{
    if (!vw_active) return OSDCMD_OK;
    char const *fn = parm->numparms >= 1 ? parm->parms[0] : "voxmap.ppm";
    FILE *f = fopen(fn, "wb");
    if (!f) { OSD_Printf("r_voxdump: can't write %s\n", fn); return OSDCMD_OK; }
    fprintf(f, "P6\n%d %d\n255\n", vw_sx, vw_sy);
    int caves = 0;
    for (int by = 0; by < vw_sy; by++)
        for (int bx = 0; bx < vw_sx; bx++)
        {
            uint8_t const *b = vw_colblocks(bx, by);
            vwcolumn_t const *c = vw_col(bx, by);
            int top = c->nruns ? c->runs[c->nruns-1].z1 : 0;
            while (top > 0 && (b[top-1] == VW_LEAVES || b[top-1] == VW_WOOD || b[top-1] == VW_AIR)) top--;  // ignore trees
            int g = top * 255 / vw_sz;
            uint8_t rgb[3] = { (uint8_t)g, (uint8_t)g, (uint8_t)g };
            int deepest = -1, ore = 0;
            for (int z = 1; z < top; z++)
            {
                if (b[z] == VW_AIR) { if (deepest < 0) deepest = z; caves++; }
                else if (b[z] == VW_GOLD) ore = 3;
                else if (b[z] == VW_IRON && ore < 2) ore = 2;
            }
            if (deepest >= 0)
            {
                int const t = deepest * 255 / vw_sz;
                rgb[0] = (uint8_t)(60 + t*3/4); rgb[1] = 40; rgb[2] = (uint8_t)(255 - t*3/4);
            }
            if (ore == 3) { rgb[0] = 255; rgb[1] = 220; rgb[2] = 0; }
            fwrite(rgb, 3, 1, f);
        }
    fclose(f);
    OSD_Printf("r_voxdump: wrote %s (%d cave cells)\n", fn, caves);
    return OSDCMD_OK;
}

static void vw_registerosd(void)
{
    static bool done;
    if (done) return;
    done = true;
    OSD_RegisterFunction("r_voxdist", "r_voxdist <blocks>: block world view distance", osdcmd_voxdist);
    OSD_RegisterFunction("r_voxdark", "r_voxdark <0-31>: how dark caves get", osdcmd_voxdark);
    static osdcvardata_t cvl = { "r_voxlantern", "blocks until cave darkness is at full strength (0 = off)", (void *)&vw_lantern, CVAR_INT, 0, 64 };
    OSD_RegisterCvar(&cvl, osdcmd_cvar_set);
    OSD_RegisterFunction("r_voxdump", "r_voxdump [file]: write a top-down debug map of the block world (PPM)", osdcmd_voxdump);
    static osdcvardata_t cv = { "r_voxstats", "block world render timing in the log every 60 frames", (void *)&vw_stats, CVAR_BOOL, 0, 1 };
    OSD_RegisterCvar(&cv, osdcmd_cvar_set);
#ifdef __EMSCRIPTEN__
    vw_stats = EM_ASM_INT({ return /[?&]stats/.test(location.search) ? 1 : 0; });  // web: ?stats turns it on
#endif
}

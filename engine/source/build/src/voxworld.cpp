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
#include "colmatch.h"
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

#define VW_MAXRUNS 32  // max separate solid runs per block column (deep cave systems stack up a lot of them)
#define VW_MAXWRUNS 8  // max separate see-through (water) runs per column

struct vwrun_t { uint8_t z0, z1; };  // non-air layers [z0, z1)

struct vwcolumn_t
{
    uint8_t  nruns;              // opaque blocks (anything not air or see-through): rendering
    uint8_t  nsruns;             // solid blocks (not fluids): collision
    uint8_t  nwruns;             // see-through blocks (water): drawn as an overlay
    vwrun_t  runs[VW_MAXRUNS];
    vwrun_t  sruns[VW_MAXRUNS];
    vwrun_t  wruns[VW_MAXWRUNS];
};

static bool        vw_active;
static int         vw_sect = -1;
static int         vw_sx, vw_sy, vw_sz;      // size in blocks
static int32_t     vw_x0, vw_y0;             // world coords of block (0,0) corner
static int32_t     vw_zbot;                  // Build z of the bottom of layer 0
static uint8_t    *vw_blocks;                // [(x*sy + y)*sz + z]
static vwcolumn_t *vw_cols;                  // [x*sy + y]
static int         vw_nfallers;              // falling blocks in flight (see voxworld_tick)

// Fluids: kind 0 is lava, kind 1 water. Each has its own queue and pace.
static const int   vw_fluidtics[2] = { 12, 5 }; // tics between flow steps (lava is slow)
static uint32_t   *vw_fluidq[2];             // fluid cells to update next flow step (see vw_fluidstep)
static size_t      vw_nfluidq[2], vw_fluidqcap[2];
static int         vw_fluidclock[2];
static void        vw_fluidstep(int kind);

static FORCE_INLINE bool vw_iswater(uint8_t t) { return t >= VW_WATER && t <= VW_WATER_F1; }
static FORCE_INLINE bool vw_islava(uint8_t t) { return t >= VW_LAVA && t <= VW_LAVA_F1; }
// light passes through air and water (water dims it a little)
static FORCE_INLINE bool vw_lightpass(uint8_t t) { return t == VW_AIR || vw_iswater(t); }

static FORCE_INLINE uint8_t *vw_colblocks(int bx, int by) { return &vw_blocks[((size_t)bx*vw_sy + by)*vw_sz]; }
static FORCE_INLINE vwcolumn_t *vw_col(int bx, int by) { return &vw_cols[(size_t)bx*vw_sy + by]; }
static FORCE_INLINE bool vw_inxy(int bx, int by) { return (unsigned)bx < (unsigned)vw_sx && (unsigned)by < (unsigned)vw_sy; }

static bool vw_issolid(uint8_t type);

static void vw_rebuildcolumn(int bx, int by)
{
    uint8_t const *b = vw_colblocks(bx, by);
    vwcolumn_t *c = vw_col(bx, by);
    for (int pass = 0; pass < 3; pass++)
    {
        vwrun_t *runs = pass == 0 ? c->runs : pass == 1 ? c->sruns : c->wruns;
        uint8_t &n = pass == 0 ? c->nruns : pass == 1 ? c->nsruns : c->nwruns;
        int const maxn = pass == 2 ? VW_MAXWRUNS : VW_MAXRUNS;
        auto in = [&](int z) { return pass == 0 ? (b[z] != VW_AIR && !vw_iswater(b[z])) : pass == 1 ? vw_issolid(b[z]) : vw_iswater(b[z]); };
        n = 0;
        int z = 0;
        while (z < vw_sz)
        {
            while (z < vw_sz && !in(z)) z++;
            if (z >= vw_sz) break;
            int const z0 = z;
            while (z < vw_sz && in(z)) z++;
            if (n < maxn)
                runs[n++] = { (uint8_t)z0, (uint8_t)z };
            else
                runs[maxn-1].z1 = (uint8_t)z;  // merge overflow into the top run (rare)
        }
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

static void vw_lightopened(int bx, int by, int bz, uint8_t oldtype);
static void vw_lightclosed(int bx, int by, int bz, uint8_t newtype);
static void vw_neighbourchanged(int bx, int by, int bz);
static void vw_startfall(int bx, int by, int bz);
static void vw_fluidqueue(int bx, int by, int bz);
static int     vw_emit(uint8_t type);
static uint8_t vw_emittint(uint8_t type);

void voxworld_set(int bx, int by, int bz, uint8_t type)
{
    if (!vw_active || !vw_inxy(bx, by) || (unsigned)bz >= (unsigned)vw_sz) return;
    uint8_t &b = vw_colblocks(bx, by)[bz];
    uint8_t const old = b;
    if (old == type) return;
    b = type;
    vw_rebuildcolumn(bx, by);
    bool const op = vw_lightpass(old), np = vw_lightpass(type);
    if (np && !op)
        vw_lightopened(bx, by, bz, old);
    else if (op && !np)
        vw_lightclosed(bx, by, bz, type);
    else if (op && np)
    {
        // air <-> water: same openness, but water dims light; redo the cell
        vw_lightclosed(bx, by, bz, type);
        vw_lightopened(bx, by, bz, old);
    }
    else if (vw_emit(old) != vw_emit(type) || vw_emittint(old) != vw_emittint(type))
        vw_lightclosed(bx, by, bz, type);  // e.g. lava turned to obsidian: its light goes
    if (type == VW_AIR)
        vw_neighbourchanged(bx, by, bz);  // sand/gravel around it may now fall, fluids flow in
    if (type != VW_AIR) vw_startfall(bx, by, bz);  // sand put over a gap falls
    if (voxworld_blockflags(type) & VW_BF_FLUID) vw_fluidqueue(bx, by, bz);  // fluid put anywhere flows
}

//////////////////////////////////////////////////////////////////////////////
// Lighting, Minecraft style, two channels per air cell, each 0..VW_LIGHTMAX:
//  - sky light: full skylight falls straight down without fading; otherwise -1 per block it spreads
//    (so it reaches ~15 blocks into a cave mouth)
//  - block light: from glowing blocks (emitters, see vw_blockdefs), -1 per block, and it carries the
//    emitter's colour (tint)
// Opening a cell spreads light into it; filling one (a falling block landing) or removing an emitter takes
// away the light that depended on it and re-spreads from whatever still lights the area (two-pass removal).
// Emitters are solid but hold their own level in their cell, which is where their light spreads from.
//////////////////////////////////////////////////////////////////////////////

static uint8_t  *vw_light;            // same layout as vw_blocks: low nibble sky, high nibble block light
static uint8_t  *vw_ltint;            // tint of the block light in each cell (VW_TINT_*)
static uint32_t *vw_lightq;           // flood queue (cell indices)
static size_t    vw_lightqcap;
static uint32_t *vw_lightrq;          // removal queue: (cell index << 4) | old level
static size_t    vw_lightrqcap;
static int       vw_darkness = 20;    // extra shade at light 0; OSD: r_voxdark
static uint8_t   vw_lightshadetab[VW_LIGHTMAX + 1];

static int     vw_emit(uint8_t type);      // light level a block type gives off (0 = none)
static uint8_t vw_emittint(uint8_t type);  // and its colour

static void vw_updatelightshades(void)
{
    for (int l = 0; l <= VW_LIGHTMAX; l++)
        vw_lightshadetab[l] = (uint8_t)((vw_darkness * (VW_LIGHTMAX - l) + VW_LIGHTMAX/2) / VW_LIGHTMAX);
}

static FORCE_INLINE int  vw_lget(uint32_t idx, int ch) { return (vw_light[idx] >> (ch*4)) & 15; }
static FORCE_INLINE void vw_lset(uint32_t idx, int ch, int l) { vw_light[idx] = (uint8_t)((vw_light[idx] & (ch ? 0x0F : 0xF0)) | (l << (ch*4))); }
static FORCE_INLINE uint32_t vw_lidx(int bx, int by, int bz) { return (uint32_t)(((size_t)bx*vw_sy + by)*vw_sz + bz); }

// Light at a cell, for shading, plus *tint: coloured block light to add on top (colour * 8 + strength 1..5).
// Coloured light doesn't set the shade: the surface is shaded by its ordinary light and the colour is added to it.
static FORCE_INLINE int vw_lightat(int bx, int by, int bz, int *tint)
{
    *tint = 0;
    if (!vw_inxy(bx, by) || bz >= vw_sz) return VW_LIGHTMAX;  // outside the world is open sky
    if (bz < 0) return 0;
    if (!vw_light) return VW_LIGHTMAX;
    uint32_t const idx = vw_lidx(bx, by, bz);
    int const v = vw_light[idx], sky = v & 15, blk = v >> 4;
    if (blk > sky)
    {
        if (int const t = vw_ltint[idx])
        {
            *tint = t*8 + (blk + 2) / 3;  // strength 1..5, stronger nearer the source
            return sky;
        }
        return blk;  // uncoloured block light just lights
    }
    return sky;
}

// Extra shade for a face lit by cell (bx,by,bz); *tint gets the light's colour.
static FORCE_INLINE int vw_lightshade(int bx, int by, int bz, int *tint) { return vw_lightshadetab[vw_lightat(bx, by, bz, tint)]; }

static void vw_lightpush(size_t &n, uint32_t idx)
{
    if (n == vw_lightqcap)
    {
        vw_lightqcap = max<size_t>(vw_lightqcap * 2, 65536);
        vw_lightq = (uint32_t *)Xrealloc(vw_lightq, vw_lightqcap * sizeof(uint32_t));
    }
    vw_lightq[n++] = idx;
}

static const int8_t vw_dirs[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };

// Spread channel ch out from the first n queued cells.
static void vw_lightflood(size_t n, int ch)
{
    int const sz = vw_sz, sy = vw_sy;
    for (size_t qi = 0; qi < n; qi++)
    {
        uint32_t const idx = vw_lightq[qi];
        int const l = vw_lget(idx, ch);
        if (l <= 1) continue;
        int const bz = idx % sz, col = idx / sz, bx = col / sy, by = col % sy;

        for (auto const &d : vw_dirs)
        {
            int const nx = bx + d[0], ny = by + d[1], nz = bz + d[2];
            if (!vw_inxy(nx, ny) || (unsigned)nz >= (unsigned)sz) continue;
            uint32_t const nidx = (uint32_t)(((size_t)nx*sy + ny)*sz + nz);
            uint8_t const nb = vw_blocks[nidx];
            if (!vw_lightpass(nb)) continue;
            // full skylight goes straight down through air without fading; water dims it
            int const nl = (ch == 0 && d[2] < 0 && l == VW_LIGHTMAX && nb == VW_AIR) ? VW_LIGHTMAX : l - 1;
            if (nl > vw_lget(nidx, ch))
            {
                vw_lset(nidx, ch, nl);
                if (ch) vw_ltint[nidx] = vw_ltint[idx];
                vw_lightpush(n, nidx);
            }
        }
    }
}

static void vw_computelight(void)
{
    size_t const total = (size_t)vw_sx*vw_sy*vw_sz;
    Xfree(vw_light);
    Xfree(vw_ltint);
    vw_light = (uint8_t *)Xcalloc(total, 1);
    vw_ltint = (uint8_t *)Xcalloc(total, 1);

    // Open sky: everything above each column's top block. skyfloor = lowest such layer.
    auto skyfloor = [](int bx, int by) -> int {
        if (!vw_inxy(bx, by)) return 0;
        vwcolumn_t const *c = vw_col(bx, by);
        return max(c->nruns ? c->runs[c->nruns-1].z1 : 0, c->nwruns ? c->wruns[c->nwruns-1].z1 : 0);
    };
    for (int bx = 0; bx < vw_sx; bx++)
        for (int by = 0; by < vw_sy; by++)
        {
            uint8_t *l = &vw_light[((size_t)bx*vw_sy + by)*vw_sz];
            for (int z = skyfloor(bx, by); z < vw_sz; z++) l[z] = VW_LIGHTMAX;
        }

    // Seed the sky flood with sky cells that sit beside a non-sky cell (cave mouths, overhangs).
    size_t n = 0;
    for (int bx = 0; bx < vw_sx; bx++)
        for (int by = 0; by < vw_sy; by++)
        {
            int const f = skyfloor(bx, by);
            int const top = max(max(skyfloor(bx-1, by), skyfloor(bx+1, by)), max(skyfloor(bx, by-1), skyfloor(bx, by+1)));
            for (int z = f; z < top; z++)
                vw_lightpush(n, vw_lidx(bx, by, z));
            if (f > 0 && f < vw_sz && vw_iswater(vw_colblocks(bx, by)[f-1]))
                vw_lightpush(n, vw_lidx(bx, by, f));  // daylight on a water surface sinks in
        }
    vw_lightflood(n, 0);

    // Block light from every emitter.
    n = 0;
    for (size_t i = 0; i < total; i++)
        if (int const e = vw_emit(vw_blocks[i]))
        {
            vw_lset((uint32_t)i, 1, e);
            vw_ltint[i] = vw_emittint(vw_blocks[i]);
            vw_lightpush(n, (uint32_t)i);
        }
    vw_lightflood(n, 1);
}

// Take away channel ch's light that came through cell idx (whose level was `old`), then re-flood from the
// edges of the removed area.
static void vw_lightremove(uint32_t idx, int ch, int old)
{
    int const sz = vw_sz, sy = vw_sy;
    vw_lset(idx, ch, 0);
    if (!old) return;

    size_t nr = 0, n = 0;
    auto rpush = [&](uint32_t i, int l) {
        if (nr == vw_lightrqcap)
        {
            vw_lightrqcap = max<size_t>(vw_lightrqcap * 2, 4096);
            vw_lightrq = (uint32_t *)Xrealloc(vw_lightrq, vw_lightrqcap * sizeof(uint32_t));
        }
        vw_lightrq[nr++] = (i << 4) | (uint32_t)l;
    };
    rpush(idx, old);

    for (size_t qi = 0; qi < nr; qi++)
    {
        uint32_t const i = vw_lightrq[qi] >> 4;
        int const l = vw_lightrq[qi] & 15;
        int const z = i % sz, col = i / sz, x = col / sy, y = col % sy;

        for (auto const &d : vw_dirs)
        {
            int const nx = x + d[0], ny = y + d[1], nz = z + d[2];
            if (!vw_inxy(nx, ny) || (unsigned)nz >= (unsigned)sz) continue;
            uint32_t const ni = (uint32_t)(((size_t)nx*sy + ny)*sz + nz);
            if (!vw_lightpass(vw_blocks[ni]))
            {
                if (ch && vw_emit(vw_blocks[ni])) vw_lightpush(n, ni);  // a neighbouring emitter shines back in
                continue;
            }
            int const nl = vw_lget(ni, ch);
            if (!nl) continue;
            bool const fromhere = nl < l || (ch == 0 && d[2] < 0 && l == VW_LIGHTMAX && nl == VW_LIGHTMAX);
            if (fromhere)
            {
                vw_lset(ni, ch, 0);
                rpush(ni, nl);
            }
            else
                vw_lightpush(n, ni);  // lit from elsewhere: spreads back in afterwards
        }
    }

    // the very top layer of an open column is lit by the sky directly
    if (ch == 0)
        for (size_t qi = 0; qi < nr; qi++)
        {
            uint32_t const i = vw_lightrq[qi] >> 4;
            if ((int)(i % sz) == sz - 1 && !vw_lget(i, 0)) { vw_lset(i, 0, VW_LIGHTMAX); vw_lightpush(n, i); }
        }
    vw_lightflood(n, ch);
}

// A block was removed (it was `oldtype`): light the new air cell from its neighbours and spread from there.
static void vw_lightopened(int bx, int by, int bz, uint8_t oldtype)
{
    if (!vw_light) return;
    uint32_t const idx = vw_lidx(bx, by, bz);
    if (vw_emit(oldtype)) vw_lightremove(idx, 1, vw_lget(idx, 1));  // an emitter went: its light goes with it

    // sky: full skylight from straight above doesn't fade (unless this is water); anything else loses one
    bool const air = vw_blocks[idx] == VW_AIR;
    int best = 0;
    if (bz + 1 >= vw_sz) best = air ? VW_LIGHTMAX : VW_LIGHTMAX - 1;
    else
    {
        uint32_t const above = vw_lidx(bx, by, bz + 1);
        if (air && vw_blocks[above] == VW_AIR && (vw_light[above] & 15) == VW_LIGHTMAX) best = VW_LIGHTMAX;
    }
    // block light (and its colour) from the brightest neighbour
    int bbest = 0, btint = 0;
    for (auto const &d : vw_dirs)
    {
        int const nx = bx + d[0], ny = by + d[1], nz = bz + d[2];
        if (nz < 0) continue;
        if (!vw_inxy(nx, ny) || nz >= vw_sz) { best = max(best, VW_LIGHTMAX - 1); continue; }  // open sky outside
        uint32_t const ni = vw_lidx(nx, ny, nz);
        if (vw_lightpass(vw_blocks[ni])) best = max(best, (vw_light[ni] & 15) - 1);
        int const bl = (vw_light[ni] >> 4) - 1;
        if (bl > bbest && (vw_lightpass(vw_blocks[ni]) || vw_emit(vw_blocks[ni]))) { bbest = bl; btint = vw_ltint[ni]; }
    }
    vw_lset(idx, 0, max(best, 0));
    vw_lset(idx, 1, bbest);
    vw_ltint[idx] = (uint8_t)btint;

    size_t n = 0;
    vw_lightpush(n, idx);
    vw_lightflood(n, 0);
    n = 0;
    vw_lightpush(n, idx);
    vw_lightflood(n, 1);
}

// A cell became solid (`newtype`): remove the light that depended on it; an emitter starts shining.
static void vw_lightclosed(int bx, int by, int bz, uint8_t newtype)
{
    if (!vw_light) return;
    uint32_t const idx = vw_lidx(bx, by, bz);
    vw_lightremove(idx, 0, vw_lget(idx, 0));
    vw_lightremove(idx, 1, vw_lget(idx, 1));
    if (int const e = vw_emit(newtype))
    {
        vw_lset(idx, 1, e);
        vw_ltint[idx] = vw_emittint(newtype);
        size_t n = 0;
        vw_lightpush(n, idx);
        vw_lightflood(n, 1);
    }
}

bool voxworld_findcave(int32_t x, int32_t y, int maxlight, vec3_t *out) { return voxworld_findspot(x, y, maxlight, 0, out); }

bool voxworld_findspot(int32_t x, int32_t y, int maxlight, int minblocklight, vec3_t *out)
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
                    if (vw_issolid(b[z-1]) && b[z] == VW_AIR && b[z+1] == VW_AIR && b[z+2] == VW_AIR &&
                        (l[z] & 15) <= maxlight && (minblocklight ? (l[z] >> 4) >= minblocklight : (l[z] >> 4) <= maxlight))
                    {
                        out->x = vw_x0 + bx*VW_BLOCKXY + VW_BLOCKXY/2;
                        out->y = vw_y0 + by*VW_BLOCKXY + VW_BLOCKXY/2;
                        out->z = vw_zbot - z*VW_BLOCKZ;
                        return true;
                    }
            }
    return false;
}

// Top of the open water in column (bx,by), or -1 if its top block isn't water.
static int vw_openwatertop(int bx, int by)
{
    if (!vw_inxy(bx, by)) return -1;
    vwcolumn_t const *c = vw_col(bx, by);
    if (!c->nwruns) return -1;
    int const wtop = c->wruns[c->nwruns-1].z1;
    return (c->nruns && c->runs[c->nruns-1].z1 > wtop) ? -1 : wtop;
}

bool voxworld_findwater(int32_t x, int32_t y, bool shore, vec3_t *out, int *ang)
{
    if (!vw_active) return false;
    int cx, cy, cz;
    voxworld_toblock(x, y, 0, &cx, &cy, &cz);
    static const int8_t sdirs[4][2] = { {1,0}, {0,1}, {-1,0}, {0,-1} };  // Build angles 0, 512, 1024, 1536
    for (int r = 0; r < max(vw_sx, vw_sy); r++)
        for (int bx = cx - r; bx <= cx + r; bx++)
            for (int by = cy - r; by <= cy + r; by++)
            {
                if (max(abs(bx - cx), abs(by - cy)) != r || !vw_inxy(bx, by)) continue;
                int top = vw_openwatertop(bx, by);
                int dir = 0;
                if (shore)
                {
                    // dry ground at the water's level (or a block above it) right next to open water
                    vwcolumn_t const *c = vw_col(bx, by);
                    if (top >= 0 || !c->nruns) continue;
                    top = c->runs[c->nruns-1].z1;
                    for (dir = 0; dir < 4; dir++)
                    {
                        int const wt = vw_openwatertop(bx + sdirs[dir][0], by + sdirs[dir][1]);
                        if (wt >= 0 && top >= wt && top <= wt + 1) break;
                    }
                    if (dir == 4) continue;
                }
                else if (top < 0) continue;
                out->x = vw_x0 + bx*VW_BLOCKXY + VW_BLOCKXY/2;
                out->y = vw_y0 + by*VW_BLOCKXY + VW_BLOCKXY/2;
                out->z = vw_zbot - top*VW_BLOCKZ;
                if (ang) *ang = dir * 512;
                return true;
            }
    return false;
}

// The cell a sprite at (x,y,z) is lit by.
static void vw_spritecell(int32_t x, int32_t y, int32_t z, int *bx, int *by, int *bz)
{
    voxworld_toblock(x, y, z, bx, by, bz);
    // standing inside a block (feet on a step edge etc.): use the cell above
    if (vw_inxy(*bx, *by) && (unsigned)*bz < (unsigned)vw_sz && !vw_lightpass(vw_colblocks(*bx, *by)[*bz])) (*bz)++;
}

int voxworld_lightshade(int32_t x, int32_t y, int32_t z)
{
    if (!vw_active) return 0;
    int bx, by, bz, tint;
    vw_spritecell(x, y, z, &bx, &by, &bz);
    return vw_lightshade(bx, by, bz, &tint);
}

//////////////////////////////////////////////////////////////////////////////
// Coloured light: shade tables per light colour and strength, built from Build's normal one (palookup[0]). A
// surface lit by coloured light is shaded by its ordinary (sky/lantern) light as usual, and the table then adds
// the light on top: the colour brightens and takes on the light's hue (see vw_buildtints).
//////////////////////////////////////////////////////////////////////////////

static const uint8_t vw_tintrgb[VW_NUMTINTS][3] =
{
    { 255, 255, 255 },  // none
    { 255, 120,  30 },  // lava: orange
    {  80, 255,  60 },  // atomic: green
};
static uint8_t *vw_tintlut[VW_NUMTINTS*8];  // [tint*8 + strength][shade*256 + colour]

// Additive coloured light: each pixel gets its own unlit colour times the light's colour added on top of its shaded
// colour (so a surface brightens and takes on the colour, more so near the source), then the nearest palette entry.
static void vw_buildtints(void)
{
    if (vw_tintlut[8 + 1] || !palookup[0]) return;
    uint8_t const *const base = (uint8_t const *)palookup[0];
    for (int t = 1; t < VW_NUMTINTS; t++)
    {
        float const tr = vw_tintrgb[t][0] / 255.f, tg = vw_tintrgb[t][1] / 255.f, tb = vw_tintrgb[t][2] / 255.f;
        for (int st = 1; st <= 5; st++)
        {
            float const a = 0.8f * st / 5.f;
            uint8_t *lut = (uint8_t *)Xmalloc(numshades * 256);
            for (int s = 0; s < numshades; s++)
            {
                float const fade = a * (1.f - 0.5f * s / numshades);  // the added light fades a little with distance too
                for (int c = 0; c < 256; c++)
                {
                    if (c == 255) { lut[s*256 + c] = 255; continue; }  // transparent stays transparent
                    palette_t const lit = paletteGetColor(base[s*256 + c]);  // shaded colour
                    palette_t const alb = paletteGetColor(c);                // unlit colour
                    int const r = min(255, (int)(lit.r + alb.r * tr * fade));
                    int const g = min(255, (int)(lit.g + alb.g * tg * fade));
                    int const b = min(255, (int)(lit.b + alb.b * tb * fade));
                    lut[s*256 + c] = (uint8_t)paletteGetClosestColorUpToIndex(r, g, b, 254);
                }
            }
            vw_tintlut[t*8 + st] = lut;
        }
    }
}

static FORCE_INLINE uint8_t const *vw_tinttable(int tint)
{
    return (tint && vw_tintlut[tint]) ? vw_tintlut[tint] : (uint8_t const *)palookup[0];
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
    T_GRAVEL, T_COAL, T_IRON, T_GOLD, T_ATOMIC, T_LAVA, T_WATER, T_OBSIDIAN,
    T_COUNT
};

// hardness: damage points to destroy (0 = indestructible). Duke's pistol does ~6 per shot.
// emit: light level given off (0 = none, up to VW_LIGHTMAX); tint: its colour (VW_TINT_*); flags: VW_BF_*.
struct vwblockdef_t { uint8_t top, side, bottom, hardness, emit = 0, tint = 0, flags = 0; };

#define VW_LAVAFLAGS (VW_BF_NONSOLID | VW_BF_FLUID | VW_BF_FULLBRIGHT | VW_BF_LAVA)
#define VW_WATERFLAGS (VW_BF_NONSOLID | VW_BF_FLUID | VW_BF_WATER | VW_BF_CLEAR)

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
    { T_ATOMIC, T_ATOMIC, T_ATOMIC, 60, 11, VW_TINT_ATOMIC },  // atomic ore: glows green
    { T_LAVA, T_LAVA, T_LAVA, 0, 15, VW_TINT_LAVA, VW_LAVAFLAGS },  // lava source
    { T_LAVA, T_LAVA, T_LAVA, 0, 15, VW_TINT_LAVA, VW_LAVAFLAGS },  // lava falling
    { T_LAVA, T_LAVA, T_LAVA, 0, 14, VW_TINT_LAVA, VW_LAVAFLAGS },  // lava flowing 3
    { T_LAVA, T_LAVA, T_LAVA, 0, 13, VW_TINT_LAVA, VW_LAVAFLAGS },  // lava flowing 2
    { T_LAVA, T_LAVA, T_LAVA, 0, 12, VW_TINT_LAVA, VW_LAVAFLAGS },  // lava flowing 1
    { T_WATER, T_WATER, T_WATER, 0, 0, 0, VW_WATERFLAGS },  // water source
    { T_WATER, T_WATER, T_WATER, 0, 0, 0, VW_WATERFLAGS },  // water falling
    { T_WATER, T_WATER, T_WATER, 0, 0, 0, VW_WATERFLAGS },  // water flowing 4
    { T_WATER, T_WATER, T_WATER, 0, 0, 0, VW_WATERFLAGS },  // water flowing 3
    { T_WATER, T_WATER, T_WATER, 0, 0, 0, VW_WATERFLAGS },  // water flowing 2
    { T_WATER, T_WATER, T_WATER, 0, 0, 0, VW_WATERFLAGS },  // water flowing 1
    { T_OBSIDIAN, T_OBSIDIAN, T_OBSIDIAN, 220 },    // obsidian: very hard
};

static int     vw_emit(uint8_t type) { return type < VW_NUMTYPES ? vw_blockdefs[type].emit : 0; }
static bool    vw_issolid(uint8_t type) { return type != VW_AIR && !(type < VW_NUMTYPES && (vw_blockdefs[type].flags & VW_BF_NONSOLID)); }
static FORCE_INLINE int vw_flags(uint8_t type) { return type < VW_NUMTYPES ? vw_blockdefs[type].flags : 0; }
int voxworld_blockflags(int type) { return (unsigned)type < VW_NUMTYPES ? vw_blockdefs[type].flags : 0; }
static uint8_t vw_emittint(uint8_t type) { return type < VW_NUMTYPES ? vw_blockdefs[type].tint : 0; }

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
static uint8_t vw_lavabase[VW_TEXSIZE*VW_TEXSIZE];   // lava as loaded; vw_tex[T_LAVA] is it scrolled (vw_animate)
static uint8_t vw_waterbase[VW_TEXSIZE*VW_TEXSIZE];  // same for water

// Scroll a texture, with each texel column drifting at its own pace.
static void vw_scrolltex(uint8_t *dst, uint8_t const *base, int t, int wobble)
{
    for (int u = 0; u < VW_TEXSIZE; u++)
    {
        int const du = (u + t/3) & (VW_TEXSIZE-1);
        int const dv = (t + ((u * 5) & 7) / wobble) & (VW_TEXSIZE-1);
        for (int v = 0; v < VW_TEXSIZE; v++)
            dst[u*VW_TEXSIZE + v] = base[du*VW_TEXSIZE + ((v + dv) & (VW_TEXSIZE-1))];
    }
}

// Slowly churn the lava texture; ripple the water.
static void vw_animate(void)
{
    // totalclock: 120 ticks a second
    vw_scrolltex(vw_tex[T_LAVA], vw_lavabase, (int)totalclock / 6, 3);
    vw_scrolltex(vw_tex[T_WATER], vw_waterbase, (int)totalclock / 10, 2);
}

static void vw_loadtextures(void)
{
    for (int t = 0; t < T_COUNT; t++)
    {
        int const tile = VW_TILEBASE + t;
        uint8_t *dst = vw_tex[t];

        if (tilesiz[tile].x == VW_TEXSIZE && tilesiz[tile].y == VW_TEXSIZE && tileLoad(tile) && waloff[tile])
        {
            Bmemcpy(dst, (void *)waloff[tile], VW_TEXSIZE*VW_TEXSIZE);
            if (t == T_LAVA) Bmemcpy(vw_lavabase, dst, sizeof(vw_lavabase));
            if (t == T_WATER) Bmemcpy(vw_waterbase, dst, sizeof(vw_waterbase));
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
    // don't open caves up into lakes (the water would hang there until something disturbed it)
    auto wet = [](int x, int y, int z) { return vw_inxy(x, y) && z < vw_sz && vw_iswater(vw_colblocks(x, y)[z]); };
    if (wet(x, y, z) || wet(x, y, z + 1) || wet(x - 1, y, z) || wet(x + 1, y, z) || wet(x, y - 1, z) || wet(x, y + 1, z))
        return;
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
            if (floor > h - 16) { if (i & 1) floor--; }
            else entrance = false;
            floor = min(floor, h);
        }
        else
        {
            if (rng.f() < 0.04f) floor += rng.f() < 0.5f ? -1 : 1;
            if (floor > h - 4) floor--;  // stay underground (mostly: valleys still get the odd skylight)
        }
        floor = clamp(floor, 5, vw_sz - 5);  // never into the bedrock layers

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
    // Heights in layers. With the default 160-layer world: lowlands/beaches around 60-64, hills up to ~105,
    // ~60 layers of underground below the lowlands, and the sky above.
    int const sea   = min(64, vw_sz*2/5);              // lowland level; beaches at or below it
    int const amp   = max(8, min(44, vw_sz - sea - 24)); // hill height range (leaves room for trees)
    int const depth = max(1, sea / 16);                // underground volume relative to the old 64-layer world
    vw_hmap = (int *)Xmalloc(sizeof(int) * vw_sx * vw_sy);

    for (int x = 0; x < vw_sx; x++)
        for (int y = 0; y < vw_sy; y++)
        {
            float const hills = vw_fbm(x / 64.f, y / 64.f, vw_seed, 4);
            float const rough = vw_fbm(x / 16.f, y / 16.f, vw_seed + 7, 3);
            int h = sea - 4 + (int)(hills*hills * amp * 1.4f + rough * 4.f);
            // Lakes: where the lake noise is high the ground eases down into a basin below the water line, deepest
            // in the middle (in hills too: a lake in a valley). None right at the spawn.
            float const lake = vw_fbm(x / 48.f, y / 48.f, vw_seed + 31, 3);
            float const sd = sqrtf((float)((x - vw_sx/2)*(x - vw_sx/2) + (y - vw_sy/2)*(y - vw_sy/2)));
            float t = clamp((lake - 0.55f) / 0.08f, 0.f, 1.f) * clamp((sd - 24.f) / 10.f, 0.f, 1.f);
            if (t > 0.f)
            {
                t = t*t*(3.f - 2.f*t);
                float const bed = sea - 2 - (lake - 0.55f) * 60.f;
                h = (int)lrintf(h + (min(bed, (float)h) - h) * t);
            }
            h = clamp(h, 6, vw_sz - 12);

            uint8_t *b = vw_colblocks(x, y);
            for (int z = 0; z <= h; z++)
            {
                uint8_t t = VW_STONE;
                if (z > h - 4)
                {
                    if (h < sea - 1)  // lake bed: sand with patches of gravel
                        t = (vw_hash(x >> 2, y >> 2, vw_seed + 41) % 3) ? VW_SAND : VW_GRAVEL;
                    else
                        t = (h <= sea + 1) ? VW_SAND : (z == h ? VW_GRASS : VW_DIRT);
                }
                // Bedrock: solid at layer 0, then thinning out over layers 1-4 (4/5 .. 1/5), so the floor is lumpy.
                if (z == 0 || (z <= 4 && (int)(vw_hash(x * 5 + z, y, vw_seed + 17) % 5) < 5 - z)) t = VW_BEDROCK;
                b[z] = t;
            }
            for (int z = h + 1; z < sea; z++) b[z] = VW_WATER;  // the water table: lakes and ponds up to layer sea-1
            vw_hmap[x*vw_sy + y] = h;
        }

    // Caves: wandering tunnels with the odd cavern, a few of them opening onto the surface.
    vwrng_t rng = { vw_seed * 2654435761u + 12345u };
    rng.next();
    int const area = vw_sx * vw_sy;
    for (int i = 0, n = max(1, area * depth / 1100); i < n; i++)
    {
        float const x = rng.range(4, vw_sx - 5) + 0.5f, y = rng.range(4, vw_sy - 5) + 0.5f;
        int const h = vw_height((int)x, (int)y);
        vw_worm(rng, x, y, rng.range(5, max(5, h - 8)), rng.range(60, 180), false);
    }
    for (int i = 0, n = max(1, area / 4000); i < n; i++)
    {
        // the first cave mouth is just ahead of the spawn point (a little to the left), so there's always one to find
        float const x = i ? rng.range(8, vw_sx - 9) + 0.5f : vw_sx/2 + 16.5f;
        float const y = i ? rng.range(8, vw_sy - 9) + 0.5f : vw_sy/2 - 5.5f;
        vw_worm(rng, x, y, vw_height((int)x, (int)y) - 1, rng.range(80, 200), true, i ? -1.f : 0.f);
    }

    // Lava: open cave below the lava line fills with lava (still sources; it only flows once you dig).
    int const lavaline = max(6, sea / 6);  // ~layer 10 in the default world
    for (int x = 0; x < vw_sx; x++)
        for (int y = 0; y < vw_sy; y++)
        {
            uint8_t *b = vw_colblocks(x, y);
            for (int z = 5; z <= lavaline; z++)
                if (b[z] == VW_AIR) b[z] = VW_LAVA;
        }

    // Ores and gravel (only replace stone, so they line cave walls rather than floating in them).
    // Depth bands (with the default world): coal anywhere in the rock, iron in the lower two thirds,
    // gold only in the deepest third.
    for (int i = 0, n = area * depth / 35; i < n; i++)
        vw_vein(rng, VW_COAL, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(5, sea + amp/2), rng.range(5, 10), false);
    for (int i = 0, n = area * depth / 70; i < n; i++)
        vw_vein(rng, VW_IRON, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(5, sea*2/3), rng.range(4, 8), false);
    for (int i = 0, n = area * depth / 500; i < n; i++)
        vw_vein(rng, VW_GOLD, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(5, max(6, sea/3)), rng.range(3, 6), false);
    for (int i = 0, n = area * depth / 2500; i < n; i++)
        vw_vein(rng, VW_ATOMIC, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(5, max(6, sea/4)), rng.range(2, 4), false);
    for (int i = 0, n = area * depth / 250; i < n; i++)
        vw_vein(rng, VW_GRAVEL, rng.range(0, vw_sx-1), rng.range(0, vw_sy-1), rng.range(5, sea + 8), rng.range(15, 30), true);

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
        // The middle of the end wall facing the spawn is gravel with sand on top: shoot it to see blocks fall.
        // (the full width of the wall, so it is right under the spawn crosshair)
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int z = s; z <= s + 4; z++) vw_put(cx - 4, cy + dy, z, VW_GRAVEL);
            for (int z = s + 5; z <= s + 6; z++) vw_put(cx - 4, cy + dy, z, VW_SAND);
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
    if (!c->nsruns) return INT32_MAX;
    return vw_layertop(c->sruns[c->nsruns-1].z1);
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
    DO_FREE_AND_NULL(vw_ltint);
    vw_nfallers = 0;
    vw_nfluidq[0] = vw_nfluidq[1] = 0;
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
static float   *vw_wzbuf;    // depth of the nearest water surface (FLT_MAX: none), [x*ydimen + y]
static uint8_t *vw_wcbuf;    // its colour
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

static FORCE_INLINE uint8_t const *vw_shadetab(int shade, int light, float depth, int tint)
{
    int s = shade + vw_lanterned(light, depth) + (int)(depth * vw_visk);
    s = clamp(s, 0, numshades - 1);
    return vw_tinttable(tint) + (s << 8);
}

static FORCE_INLINE int vw_rowof(float y) { return (int)ceilf(y - 0.5f); }  // first row whose centre is >= y

// Horizontal face (top or bottom) at height hz/vw_ky above/below the eye, between depths [d0, d1].
static __attribute__((noinline)) void vw_drawflat(uint8_t *col, float *zc, vwgap_t const *vis, int nv, float hz, float d0, float d1,
                                     float dx, float dy, uint8_t const *tex, int faceshade, int light, uint8_t const *pal0)
{
    float const ox = vw_cx - vw_x0, oy = vw_cy - vw_y0;
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

// Rows that look through more than VW_WATERMURKY of water show nothing but water, so they needn't be traced on
// to the lake bed. Each batch of water rows drawn queues up to be closed once the ray is that far past it.
struct vwmurky_t { int16_t a, b; float at; };
#define VW_MAXMURKY 256
struct vwmurkyq_t { vwmurky_t q[VW_MAXMURKY]; int head = 0, tail = 0; };

// How much of what's behind water shows through, by how far you look through the water.
#define VW_WATERCLEAR  (2.f*VW_BLOCKXY)  // up to here: mostly what's behind
#define VW_WATERMURKY  (6.f*VW_BLOCKXY)  // up to here: mostly water; beyond, just water
#define VW_WATERNEAR   (1.5f*VW_BLOCKXY) // under water, this close is clear

static bool    vw_camwater;  // this frame's camera is under water: everything fogs into vw_fogcol with distance
static uint8_t vw_fogcol;

static FORCE_INLINE uint8_t vw_waterblend(uint8_t water, uint8_t behind, float through)
{
    uint8_t const *const tr = (uint8_t const *)blendtable[0];
    if (!tr) return water;
    if (through < VW_WATERCLEAR) return tr[(behind << 8) | water];
    if (through < VW_WATERMURKY) return tr[(water << 8) | behind];
    return water;
}

// Write water rows [a,b) that are still open (gaps) and have no nearer water yet. pix(y) gives colour and depth.
template <typename F>
static FORCE_INLINE void vw_waterrows(vwgap_t const *gaps, int ng, int a, int b, uint8_t *wcol, float *wz, F pix)
{
    for (int i = 0; i < ng; i++)
    {
        if (b <= gaps[i].a) break;
        int const ya = max<int>(a, gaps[i].a), yb = min<int>(b, gaps[i].b);
        for (int y = ya; y < yb; y++)
            if (wz[y] == FLT_MAX) pix(y, wcol[y], wz[y]);
    }
}

// The water surfaces of block column c (cell gx,gy) along this screen column's ray: faces between water and
// anything that isn't water. They go into the water overlay; nothing is occluded, so what's behind still draws.
static __attribute__((noinline)) void vw_drawwatercell(vwcolumn_t const *c, uint8_t const *cb, int gx, int gy, int side,
                                                       float din, float dout, float dx, float dy,
                                                       vwgap_t const *gaps, int ng, uint8_t *wcol, float *wz, vwmurkyq_t &mq)
{
    auto murky = [&](int a, int b, float d) { if (mq.tail < VW_MAXMURKY) mq.q[mq.tail++] = { (int16_t)a, (int16_t)b, d + VW_WATERMURKY }; };
    int const ydim = ydimen, maxshade = numshades - 1;
    float const dclip = max(din, VW_NEAR);
    uint8_t const *const tex = vw_tex[T_WATER];
    float const ox = vw_cx - vw_x0, oy = vw_cy - vw_y0;
    // the cell the ray came from: its side of a front face
    int const px = gx - (side == 0) + (side == 1), py = gy - (side == 2) + (side == 3);
    uint8_t const *const pb = (side >= 0 && vw_inxy(px, py)) ? vw_colblocks(px, py) : nullptr;

    for (int ri = 0; ri < c->nwruns; ri++)
    {
        vwrun_t const r = c->wruns[ri];

        // Front faces at din, for the layers not continuing water from the cell in front.
        if (din > VW_NEAR && side >= 0)
        {
            float const inv = vw_ky / din;
            float const hx = vw_cx + dx*din, hy = vw_cy + dy*din;
            float const along = (side < 2) ? (hy - vw_y0) : (hx - vw_x0);
            int u = ((int)along >> 5) & (VW_TEXSIZE-1);
            if (side == 0 || side == 3) u = VW_TEXSIZE-1 - u;
            uint8_t const *const tcol = tex + u*VW_TEXSIZE;
            float const zperrow = din / vw_ky, texperz = (float)VW_TEXSIZE / VW_BLOCKZ;

            for (int z = r.z0; z < r.z1;)
            {
                if (pb && pb[z] != VW_AIR) { z++; continue; }  // water beyond water: no face; solid: can't see it
                int z1 = z + 1;
                while (z1 < r.z1 && !(pb && pb[z1] != VW_AIR)) z1++;
                int ltint;
                int const ls = vw_lightshade(px, py, z1 - 1, &ltint);
                uint8_t const *const pal = vw_shadetab(vw_faceshade[side], ls, din, ltint);
                int const a = max(vw_rowof(vw_horiz + ((float)vw_layertop(z1) - vw_cz)*inv), 0);
                int const b = min(vw_rowof(vw_horiz + ((float)vw_layertop(z) - vw_cz)*inv), ydim);
                vw_waterrows(gaps, ng, a, b, wcol, wz, [&](int y, uint8_t &oc, float &oz) {
                    int const t = (int)((vw_zbot - (vw_cz + (y + 0.5f - vw_horiz)*zperrow)) * texperz);
                    oc = pal[tcol[VW_TEXSIZE-1 - (t & (VW_TEXSIZE-1))]];
                    oz = din;
                });
                murky(a, b, din);
                z = z1;
            }
        }

        // Top (the surface, seen from above) and bottom (seen from below, only over open air).
        for (int face = 0; face < 2; face++)
        {
            float const zf = (float)vw_layertop(face ? r.z0 : r.z1);
            if (face == 0 ? !(zf > vw_cz) : !(zf < vw_cz && r.z0 > 0 && cb[r.z0-1] == VW_AIR)) continue;
            float const hz = (zf - vw_cz) * vw_ky;
            int const a = max(vw_rowof(vw_horiz + hz / (face ? dclip : dout)), 0);
            int const b = min(vw_rowof(vw_horiz + hz / (face ? dout : dclip)), ydim);
            if (a >= b) continue;
            int ftint;
            int const fl = vw_lightshade(gx, gy, face ? r.z0 - 1 : r.z1, &ftint);
            uint8_t const *const pal0 = vw_tinttable(ftint);
            int const fshade = vw_faceshade[face ? 5 : 4];
            vw_waterrows(gaps, ng, a, b, wcol, wz, [&](int y, uint8_t &oc, float &oz) {
                float d = hz * vw_rowrec[y];
                d = d < dclip ? dclip : (d > dout ? dout : d);
                int const u = ((int)(ox + dx*d) >> 5) & (VW_TEXSIZE-1);
                int const v = ((int)(oy + dy*d) >> 5) & (VW_TEXSIZE-1);
                int sh = fshade + vw_lanterned(fl, d) + (int)(d * vw_visk);
                sh = sh > maxshade ? maxshade : sh;
                oc = pal0[(sh << 8) + tex[u*VW_TEXSIZE + v]];
                oz = d;
            });
            murky(a, b, dout);
        }
    }
}

static __attribute__((noinline)) void vw_drawcolumn(int sx, uint8_t *col, float *zc, uint8_t *wcol, float *wz)
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
    for (int y = 0; y < ydim; y++) wz[y] = FLT_MAX;
    vwmurkyq_t mq;  // water rows to close once they're deep enough in murk
    // under water, everything past the fog is fog
    float const maxdist = vw_camwater ? min(vw_maxdist, VW_WATERNEAR + VW_WATERMURKY + VW_BLOCKXY) : vw_maxdist;

    for (;;)
    {
        float const dout = min(tmx, tmy);

        if (vw_inxy(gx, gy))
        {
            vwcolumn_t const *c = vw_col(gx, gy);
            int const nr = c->nruns, nw = c->nwruns;
            // Cheap reject: if even the top of this block column projects below every open row, skip it.
            // (The top face's far edge is its highest point when it's below the eye, else the near edge.)
            float const colztop = (float)vw_layertop(max(nr ? c->runs[nr-1].z1 : 0, nw ? c->wruns[nw-1].z1 : 0));
            if ((nr || nw) && din > VW_NEAR)
            {
                float const dz = colztop - vw_cz;
                float const ytop = vw_horiz + dz * vw_ky / (dz > 0.f ? dout : din);
                if (ytop >= gaps[ng-1].b)
                    goto next_cell;
            }
            // water first: it doesn't occlude, and what's in this same cell may be behind it
            if (nw)
                vw_drawwatercell(c, vw_colblocks(gx, gy), gx, gy, side, din, dout, dx, dy, gaps, ng, wcol, wz, mq);
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
                                            if (vw_flags(cb[layer]) & VW_BF_FULLBRIGHT)
                                                pal = vw_shadetab(0, 0, din, 0);
                                            else
                                            {
                                                int ltint;
                                                int const ls = vw_lightshade(lx, ly, layer, &ltint);
                                                pal = vw_shadetab(vw_faceshade[side] + vw_dmgshade(colidx + layer, cb[layer]), ls, din, ltint);
                                            }
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
                            {
                                int ftint;
                                int const fl = vw_lightshade(gx, gy, r.z1, &ftint);
                                bool const fb = vw_flags(cb[r.z1-1]) & VW_BF_FULLBRIGHT;
                                vw_drawflat(col, zc, vis, nv, hz, dclip, dout, dx, dy,
                                            vw_tex[vw_blockdefs[cb[r.z1-1]].top],
                                            fb ? 0 : vw_faceshade[4] + vw_dmgshade(vw_index(gx, gy, r.z1-1), cb[r.z1-1]),
                                            fb ? 0 : fl, vw_tinttable(fb ? 0 : ftint));
                            }
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
                            {
                                int ftint;
                                int const fl = vw_lightshade(gx, gy, r.z0 - 1, &ftint);
                                bool const fb = vw_flags(cb[r.z0]) & VW_BF_FULLBRIGHT;
                                vw_drawflat(col, zc, vis, nv, hz, dclip, dout, dx, dy,
                                            vw_tex[vw_blockdefs[cb[r.z0]].bottom],
                                            fb ? 0 : vw_faceshade[5] + vw_dmgshade(vw_index(gx, gy, r.z0), cb[r.z0]),
                                            fb ? 0 : fl, vw_tinttable(fb ? 0 : ftint));
                            }
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
        while (mq.head < mq.tail && mq.q[mq.head].at < dout && ng)
        {
            // close them: zc FLT_MAX makes the composite give pure water colour
            vwmurky_t const m = mq.q[mq.head++];
            int const nv = vw_clip(gaps, ng, m.a, m.b, vis);
            for (int v = 0; v < nv; v++)
                for (int y = vis[v].a; y < vis[v].b; y++)
                    zc[y] = FLT_MAX;
        }
        if (!ng || dout > maxdist) break;

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

    // Water over whatever is behind it: the further you look through it, the murkier.
    if (vw_camwater)
    {
        // under water: clear close up, fogging into the water's colour further away
        for (int y = 0; y < ydim; y++)
            if (zc[y] > VW_WATERNEAR) col[y] = vw_waterblend(vw_fogcol, col[y], zc[y] - VW_WATERNEAR);
    }
    else
    {
        for (int y = 0; y < ydim; y++)
            if (wz[y] < zc[y])
                col[y] = vw_waterblend(wcol[y], col[y], zc[y] - wz[y]);
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
        Xfree(vw_zbuf); Xfree(vw_cbuf); Xfree(vw_rowrec); Xfree(vw_wzbuf); Xfree(vw_wcbuf);
        vw_bufw = xdimen; vw_bufh = ydimen;
        vw_zbuf   = (float *)Xmalloc(sizeof(float) * xdimen * ydimen);
        vw_cbuf   = (uint8_t *)Xmalloc(xdimen * ydimen);
        vw_wzbuf  = (float *)Xmalloc(sizeof(float) * xdimen * ydimen);
        vw_wcbuf  = (uint8_t *)Xmalloc(xdimen * ydimen);
        vw_rowrec = (float *)Xmalloc(sizeof(float) * ydimen);
    }

    vw_setupcamera();
    vw_buildtints();
    vw_animate();

    // Under water? Then the view fogs into deep blue, darker where the water is darker.
    {
        int bx, by, bz, tint;
        voxworld_toblock((int32_t)vw_cx, (int32_t)vw_cy, (int32_t)vw_cz, &bx, &by, &bz);
        vw_camwater = vw_iswater(voxworld_get(bx, by, bz));
        if (vw_camwater)
        {
            static int fogbase = -1;
            if (fogbase < 0) fogbase = paletteGetClosestColorUpToIndex(24, 56, 120, 254);
            int const sh = clamp(vw_lightshade(bx, by, bz, &tint), 0, numshades - 1);
            vw_fogcol = ((uint8_t const *)palookup[0])[(sh << 8) + fogbase];
        }
    }

    auto const &sec = sector[vw_sect];
    if (!waloff[sec.ceilingpicnum]) tileLoad(sec.ceilingpicnum);

    int const ydim = ydimen;
    double const t0 = timerGetFractionalTicks();
    for (int x = 0; x < xdimen; x++)
        vw_drawcolumn(x, vw_cbuf + x*ydim, vw_zbuf + x*ydim, vw_wcbuf + x*ydim, vw_wzbuf + x*ydim);
    double const t1 = timerGetFractionalTicks();
    vw_transpose();
    double const t2 = timerGetFractionalTicks();

    // timing, printed by `r_voxstats 1`
    static double acc[2]; static int frames;
    acc[0] += t1 - t0; acc[1] += t2 - t1;
    if (++frames == 60)
    {
        if (vw_stats)
        {
            int nwater = 0;
            for (int i = 0; i < xdimen*ydim; i++) nwater += vw_wzbuf[i] != FLT_MAX;
            LOG_F(INFO, "voxworld: columns %.2f ms, transpose %.2f ms (avg of 60 frames, %dx%d), %d%% water%s", acc[0]/60, acc[1]/60,
                  xdimen, ydim, nwater * 100 / (xdimen*ydim), vw_camwater ? " (under)" : "");
        }
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
    int cbx, cby, cbz, tint;
    vw_spritecell(tspr->x, tspr->y, tspr->z - VW_BLOCKZ/2, &cbx, &cby, &cbz);
    int const lshade = vw_lightshade(cbx, cby, cbz, &tint);
    int shade = tspr->shade + (int)(depth * vw_visk) + vw_lanterned(lshade, depth);
    shade = clamp(shade, 0, numshades - 1);
    // coloured light tints ordinary sprites; ones with their own palette (pal != 0) keep it
    uint8_t const *const lut = (pal == 0 ? vw_tinttable(tint) : (uint8_t const *)palookup[pal]) + (shade << 8);

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
            float const wd = vw_wzbuf[x*ydimen + y];
            uint8_t const sc = vw_camwater ? (depth > VW_WATERNEAR ? vw_waterblend(vw_fogcol, lut[c], depth - VW_WATERNEAR) : lut[c])
                             : wd < depth ? vw_waterblend(vw_wcbuf[x*ydimen + y], lut[c], depth - wd) : lut[c];  // under water
            if (trans)
                *p = transrev ? trans[(sc << 8) | *p] : trans[(*p << 8) | sc];
            else
            {
                *p = sc;
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
    for (int i = 0; i < c->nsruns; i++)
    {
        int32_t const rt = vw_layertop(c->sruns[i].z1), rb = vw_layertop(c->sruns[i].z0);
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
            for (int i = 0; i < c->nsruns; i++)
            {
                int32_t const rt = vw_layertop(c->sruns[i].z1), rb = vw_layertop(c->sruns[i].z0);
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
            if (b != VW_AIR && bz >= 0 && !(vw_flags(b) & VW_BF_CLEAR))
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

//////////////////////////////////////////////////////////////////////////////
// Falling blocks (sand, gravel). A block falls when the cell under it opens up. It moves down the grid one cell
// at a time under gravity, so it always stays a normal block (rendered, collided, lit like any other). If an actor
// is in the way the game is told (it does the damage) and the block bursts.
//////////////////////////////////////////////////////////////////////////////

struct vwfaller_t
{
    int16_t x, y;
    int16_t z;       // the cell it currently occupies
    uint8_t type;
    uint8_t delay;   // tics before it starts moving
    float   pos;     // fractional fall progress below z (cells)
    float   vel;     // cells per tic
};

#define VW_MAXFALLERS 2048
static vwfaller_t  vw_fallers[VW_MAXFALLERS];
static vweventfn_t vw_eventfn;
static uint8_t     vw_falldelay = 3;  // tics before a disturbed block drops; 0 while a falling block pulls the next one along

void voxworld_seteventfn(vweventfn_t fn) { vw_eventfn = fn; }

static FORCE_INLINE bool vw_falls(uint8_t type) { return type == VW_SAND || type == VW_GRAVEL; }

static void vw_startfall(int bx, int by, int bz)
{
    if (!vw_inxy(bx, by) || (unsigned)bz >= (unsigned)vw_sz) return;
    uint8_t const type = vw_colblocks(bx, by)[bz];
    if (!vw_falls(type) || bz == 0) return;
    uint8_t const below = voxworld_get(bx, by, bz - 1);
    if (below != VW_AIR && !(vw_flags(below) & VW_BF_FLUID)) return;
    for (int i = 0; i < vw_nfallers; i++)
        if (vw_fallers[i].x == bx && vw_fallers[i].y == by && vw_fallers[i].z == bz) return;
    if (vw_nfallers >= VW_MAXFALLERS) return;
    vw_fallers[vw_nfallers++] = { (int16_t)bx, (int16_t)by, (int16_t)bz, type, vw_falldelay, 0.f, 0.f };
}

static void vw_fluidqueue(int bx, int by, int bz);

static void vw_neighbourchanged(int bx, int by, int bz)
{
    vw_fluidqueue(bx, by, bz + 1);
    vw_fluidqueue(bx - 1, by, bz);
    vw_fluidqueue(bx + 1, by, bz);
    vw_fluidqueue(bx, by - 1, bz);
    vw_fluidqueue(bx, by + 1, bz);
    vw_startfall(bx, by, bz + 1);
    vw_startfall(bx - 1, by, bz);
    vw_startfall(bx + 1, by, bz);
    vw_startfall(bx, by - 1, bz);
    vw_startfall(bx, by + 1, bz);
}

#define VW_GRAVITY  0.035f  // cells per tic per tic (~30 tics a second)
#define VW_MAXFALL  1.5f

void voxworld_tick(void)
{
    if (!vw_active) return;
    for (int k = 0; k < 2; k++)
        if (++vw_fluidclock[k] >= vw_fluidtics[k]) { vw_fluidclock[k] = 0; vw_fluidstep(k); }
    if (!vw_nfallers) return;

    // lowest first, so a falling stack moves as one
    std::sort(vw_fallers, vw_fallers + vw_nfallers, [](vwfaller_t const &a, vwfaller_t const &b) { return a.z < b.z; });

    int n = vw_nfallers;
    vw_falldelay = 0;
    for (int i = 0; i < n; i++)
    {
        vwfaller_t &f = vw_fallers[i];
        bool done = false;

        if (vw_colblocks(f.x, f.y)[f.z] != f.type)
            done = true;  // destroyed while falling
        else if (f.delay)
            f.delay--;
        else
        {
            f.vel = min(f.vel + VW_GRAVITY, VW_MAXFALL);
            f.pos += f.vel;
            while (f.pos >= 1.f && !done)
            {
                f.pos -= 1.f;
                int const nz = f.z - 1;
                uint8_t const nb = nz >= 0 ? vw_colblocks(f.x, f.y)[nz] : VW_BEDROCK;
                if (nb != VW_AIR && !(vw_flags(nb) & VW_BF_FLUID))  // sinks through water and lava
                {
                    // landed
                    if (vw_eventfn) vw_eventfn(VW_EV_FALLLANDED, f.x, f.y, f.z, f.type, f.vel);
                    done = true;
                    break;
                }
                if (vw_eventfn && vw_eventfn(VW_EV_FALLHIT, f.x, f.y, nz, f.type, f.vel))
                {
                    // hit something: the block bursts
                    if (vw_dmg) vw_dmg[vw_index(f.x, f.y, f.z)] = 0;
                    int const oz = f.z;
                    f.z = -1;  // so voxworld_set's neighbour check doesn't find this one
                    voxworld_set(f.x, f.y, oz, VW_AIR);
                    vw_eventfn(VW_EV_FALLBROKE, f.x, f.y, oz, f.type, f.vel);
                    done = true;
                    break;
                }
                // move down one cell: fill the new one first, then open the old one (which may start the next
                // block above falling)
                if (vw_dmg) vw_dmg[vw_index(f.x, f.y, f.z)] = 0;
                int const oz = f.z;
                f.z = nz;  // first, so filling the new cell doesn't register it as another faller
                voxworld_set(f.x, f.y, nz, f.type);
                voxworld_set(f.x, f.y, oz, VW_AIR);
            }
        }

        if (done)
        {
            // fallers added this tic (beyond n) stay; swap in the last of this tic's ones
            vw_fallers[i] = vw_fallers[n - 1];
            vw_fallers[n - 1] = vw_fallers[vw_nfallers - 1];
            vw_nfallers--;
            n--;
            i--;
        }
    }
    vw_falldelay = 3;
}

//////////////////////////////////////////////////////////////////////////////
// Fluids (lava). Sources are placed by generation and never go away. Every few tics, fluid cells that might be
// able to spread do: down into an open cell below (pouring), otherwise sideways into open neighbours one level
// weaker, so lava spreads up to 3 cells from where it lands. Blocks only ever get removed in play, so flow never
// has to retreat.
//////////////////////////////////////////////////////////////////////////////


static void vw_fluidqueue(int bx, int by, int bz)
{
    if (!vw_inxy(bx, by) || (unsigned)bz >= (unsigned)vw_sz) return;
    uint8_t const t = vw_colblocks(bx, by)[bz];
    if (!(vw_flags(t) & VW_BF_FLUID)) return;
    int const k = vw_iswater(t);
    if (vw_nfluidq[k] == vw_fluidqcap[k])
    {
        vw_fluidqcap[k] = max<size_t>(vw_fluidqcap[k] * 2, 1024);
        vw_fluidq[k] = (uint32_t *)Xrealloc(vw_fluidq[k], vw_fluidqcap[k] * sizeof(uint32_t));
    }
    vw_fluidq[k][vw_nfluidq[k]++] = vw_lidx(bx, by, bz);
}

// Fluid of kind k (0 lava, 1 water) flows into cell (bx,by,bz) as type nt. Into air it just fills it; where lava
// and water meet they set: water onto a lava source makes obsidian, onto flowing lava cobblestone, and lava
// flowing into water makes stone. Returns false if it can't go there.
static bool vw_flowinto(int k, int bx, int by, int bz, uint8_t nt)
{
    if (!vw_inxy(bx, by) || (unsigned)bz >= (unsigned)vw_sz) return false;
    uint8_t const t = vw_colblocks(bx, by)[bz];
    if (t == VW_AIR)
    {
        voxworld_set(bx, by, bz, nt);
        vw_fluidqueue(bx, by, bz);
        return true;
    }
    if (k == 1 && vw_islava(t))
    {
        voxworld_set(bx, by, bz, t == VW_LAVA ? VW_OBSIDIAN : VW_COBBLE);
        return true;
    }
    if (k == 0 && vw_iswater(t))
    {
        voxworld_set(bx, by, bz, VW_STONE);
        return true;
    }
    return false;
}

static void vw_fluidstep(int k)
{
    if (!vw_nfluidq[k]) return;
    // take this step's list (new entries queue up for the next step)
    size_t const n = vw_nfluidq[k];
    uint32_t *const list = (uint32_t *)Xmalloc(n * sizeof(uint32_t));
    Bmemcpy(list, vw_fluidq[k], n * sizeof(uint32_t));
    vw_nfluidq[k] = 0;
    std::sort(list, list + n);
    uint32_t last = UINT32_MAX;

    for (size_t i = 0; i < n; i++)
    {
        uint32_t const idx = list[i];
        if (idx == last) continue;
        last = idx;
        uint8_t const t = vw_blocks[idx];
        if (!(vw_flags(t) & VW_BF_FLUID) || vw_iswater(t) != k) continue;
        int const bz = idx % vw_sz, col = idx / vw_sz, bx = col / vw_sy, by = col % vw_sy;

        // pour down if it can
        if (bz > 0 && vw_flowinto(k, bx, by, bz - 1, k ? VW_WATER_FALL : VW_LAVA_FALL))
        {
            if (!(vw_flags(vw_blocks[idx - 1]) & VW_BF_FLUID)) vw_fluidqueue(bx, by, bz);  // it set solid: spread next
            continue;
        }
        uint8_t const below = bz > 0 ? vw_blocks[idx - 1] : VW_BEDROCK;
        if ((t == VW_WATER_FALL || t == VW_LAVA_FALL) && (vw_flags(below) & VW_BF_FLUID) && vw_iswater(below) == (bool)k)
            continue;  // poured into a pool of its own kind: it doesn't spread across the top

        // otherwise sideways, one level weaker: source/pour -> F3 -> F2 -> F1 (lava: 3 cells),
        // source/pour -> F4 .. F1 (water: 4 cells)
        uint8_t nt;
        switch (t)
        {
            case VW_LAVA: case VW_LAVA_FALL: nt = VW_LAVA_F3; break;
            case VW_LAVA_F3: nt = VW_LAVA_F2; break;
            case VW_LAVA_F2: nt = VW_LAVA_F1; break;
            case VW_WATER: case VW_WATER_FALL: nt = VW_WATER_F4; break;
            case VW_WATER_F4: nt = VW_WATER_F3; break;
            case VW_WATER_F3: nt = VW_WATER_F2; break;
            case VW_WATER_F2: nt = VW_WATER_F1; break;
            default: continue;
        }
        static const int8_t sdirs[4][2] = { {-1,0}, {1,0}, {0,-1}, {0,1} };
        for (auto const &d : sdirs)
            vw_flowinto(k, bx + d[0], by + d[1], bz, nt);
    }
    Xfree(list);
}

int voxworld_blockat(int32_t x, int32_t y, int32_t z)
{
    int bx, by, bz;
    if (!vw_active || !voxworld_toblock(x, y, z, &bx, &by, &bz)) return VW_AIR;
    return vw_colblocks(bx, by)[bz];
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
    for (int i = 0; i < c->nsruns; i++)
    {
        int32_t const top = vw_layertop(c->sruns[i].z1);
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
            if (c->nwruns && c->wruns[c->nwruns-1].z1 >= top)
            {
                int const d = c->wruns[c->nwruns-1].z1 - c->wruns[c->nwruns-1].z0;
                rgb[0] = 20; rgb[1] = (uint8_t)max(40, 140 - d*8); rgb[2] = (uint8_t)max(90, 240 - d*8);
            }
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

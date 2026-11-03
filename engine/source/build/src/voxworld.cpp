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
#include <unordered_map>
#include <vector>


static void vw_registerosd(void);

//////////////////////////////////////////////////////////////////////////////
// World storage
//////////////////////////////////////////////////////////////////////////////

#define VW_MAXRUNS 32  // max separate solid runs per block column (deep cave systems stack up a lot of them)
#define VW_MAXWRUNS 8  // max separate see-through (water) runs per column
#define VW_MAXPLANTS 4 // max plants (billboard blocks) per column
#define VW_MAXGRUNS 8  // max separate glass runs per column

struct vwrun_t { uint8_t z0, z1; };  // non-air layers [z0, z1)

struct vwcolumn_t
{
    uint8_t  nruns;              // opaque blocks (anything not air or see-through): rendering
    uint8_t  nsruns;             // solid blocks (not fluids): collision
    uint8_t  nwruns;             // see-through blocks (water): drawn as an overlay
    vwrun_t  runs[VW_MAXRUNS];
    vwrun_t  sruns[VW_MAXRUNS];
    vwrun_t  wruns[VW_MAXWRUNS];
    uint8_t  nplants;            // plant layers, lowest first: drawn as billboards
    uint8_t  plants[VW_MAXPLANTS];
    uint8_t  ngruns;             // glass: solid, drawn as cut-out faces
    vwrun_t  gruns[VW_MAXGRUNS];
};

static bool        vw_active;
static int         vw_sect = -1;
static int         vw_sx, vw_sy, vw_sz;      // size in blocks
static int32_t     vw_x0, vw_y0;             // world coords of block (0,0) corner
static int32_t     vw_zbot;                  // Build z of the bottom of layer 0
static uint8_t    *vw_blocks;                // [(x*sy + y)*sz + z]
static vwcolumn_t *vw_cols;                  // [x*sy + y]
static int         vw_nfallers;              // falling blocks in flight (see voxworld_tick)

// The world and the resident window. The world is vw_wsx x vw_wsy blocks (global block coordinates); only a
// vw_sx x vw_sy window of it, in whole chunks, is in memory, at global block offset (vw_ox, vw_oy). Everything else in
// this file works in window (local) block coordinates: local = global - offset, and vw_x0/vw_y0 are the Build
// coordinates of the window's corner, so world <-> block conversion is the same as for a fixed world. See
// "Streaming" for how the window follows the player.
#define VW_CHUNK  16   // chunk edge in blocks (columns are generated, saved and lit a chunk at a time)
#define VW_WINDOW 256  // resident window edge in blocks (a multiple of VW_CHUNK)
static int         vw_wsx, vw_wsy;           // world size in blocks
static int         vw_ox, vw_oy;             // window origin, in global blocks (multiples of VW_CHUNK)
static int32_t     vw_wx0, vw_wy0;           // Build coords of global block (0,0)
static int         vw_ncx, vw_ncy;           // window size in chunks
static uint8_t    *vw_cready;                // [cx*vw_ncy + cy]: chunk generated (else a solid placeholder)
static uint8_t    *vw_cdirty;                // changed since generated/restored: saved when it leaves the window
static int         vw_sea, vw_amp, vw_depth, vw_lavaline;  // generation constants (layers)
static int         vw_snowline;              // ground about this high or higher is snow-capped (0: none)
static FORCE_INLINE int vw_chunkof(int bx, int by) { return (bx / VW_CHUNK) * vw_ncy + by / VW_CHUNK; }

// Fluids: kind 0 is lava, kind 1 water. Each has its own queue and pace.
static const int   vw_fluidtics[2] = { 12, 5 }; // tics between flow steps (lava is slow)
static uint32_t   *vw_fluidq[2];             // fluid cells to update next flow step (see vw_fluidstep)
static size_t      vw_nfluidq[2], vw_fluidqcap[2];
static int         vw_fluidclock[2];
static void        vw_fluidstep(int kind);

static FORCE_INLINE bool vw_iswater(uint8_t t) { return t >= VW_WATER && t <= VW_WATER_F1; }
static FORCE_INLINE bool vw_islava(uint8_t t) { return t >= VW_LAVA && t <= VW_LAVA_F1; }
static FORCE_INLINE bool vw_isplant(uint8_t t) { return t >= VW_TALLGRASS && t <= VW_FLOWER_YELLOW; }
// air, or as good as: plants don't block anything
static FORCE_INLINE bool vw_isopen(uint8_t t) { return t == VW_AIR || vw_isplant(t); }
static FORCE_INLINE bool vw_isglass(uint8_t t) { return t == VW_GLASS; }
// light goes straight through: full skylight falls down through these without fading
static FORCE_INLINE bool vw_skyclear(uint8_t t) { return vw_isopen(t) || vw_isglass(t); }
// light passes through air, glass and water (water dims it a little)
static FORCE_INLINE bool vw_lightpass(uint8_t t) { return vw_skyclear(t) || vw_iswater(t); }

static FORCE_INLINE uint8_t *vw_colblocks(int bx, int by) { return &vw_blocks[((size_t)bx*vw_sy + by)*vw_sz]; }
static FORCE_INLINE vwcolumn_t *vw_col(int bx, int by) { return &vw_cols[(size_t)bx*vw_sy + by]; }
static FORCE_INLINE bool vw_inxy(int bx, int by) { return (unsigned)bx < (unsigned)vw_sx && (unsigned)by < (unsigned)vw_sy; }

static bool vw_issolid(uint8_t type);

static void vw_rebuildcolumn(int bx, int by)
{
    uint8_t const *b = vw_colblocks(bx, by);
    vwcolumn_t *c = vw_col(bx, by);
    for (int pass = 0; pass < 4; pass++)
    {
        vwrun_t *runs = pass == 0 ? c->runs : pass == 1 ? c->sruns : pass == 2 ? c->wruns : c->gruns;
        uint8_t &n = pass == 0 ? c->nruns : pass == 1 ? c->nsruns : pass == 2 ? c->nwruns : c->ngruns;
        int const maxn = pass == 2 ? VW_MAXWRUNS : pass == 3 ? VW_MAXGRUNS : VW_MAXRUNS;
        auto in = [&](int z) {
            switch (pass)
            {
                case 0: return !vw_lightpass(b[z]);  // opaque: not air, plants, glass or water
                case 1: return vw_issolid(b[z]);
                case 2: return vw_iswater(b[z]);
                default: return vw_isglass(b[z]);
            }
        };
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
    c->nplants = 0;
    for (int z = 0; z < vw_sz; z++)
        if (vw_isplant(b[z]))
        {
            if (c->nplants == VW_MAXPLANTS) c->nplants--;  // keep the highest (never happens with generated plants)
            c->plants[c->nplants++] = (uint8_t)z;
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
    if (vw_cdirty) vw_cdirty[vw_chunkof(bx, by)] = 1;
    vw_rebuildcolumn(bx, by);
    bool const op = vw_lightpass(old), np = vw_lightpass(type);
    if (np && !op)
        vw_lightopened(bx, by, bz, old);
    else if (op && !np)
        vw_lightclosed(bx, by, bz, type);
    else if (op && np && vw_iswater(old) != vw_iswater(type))
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
    // a plant loses its footing
    if (!vw_issolid(type) && bz + 1 < vw_sz && vw_isplant(vw_colblocks(bx, by)[bz + 1])) voxworld_set(bx, by, bz + 1, VW_AIR);
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
            int const nl = (ch == 0 && d[2] < 0 && l == VW_LIGHTMAX && vw_skyclear(nb)) ? VW_LIGHTMAX : l - 1;
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
    bool const air = vw_skyclear(vw_blocks[idx]);
    int best = 0;
    if (bz + 1 >= vw_sz) best = air ? VW_LIGHTMAX : VW_LIGHTMAX - 1;
    else
    {
        uint32_t const above = vw_lidx(bx, by, bz + 1);
        if (air && vw_skyclear(vw_blocks[above]) && (vw_light[above] & 15) == VW_LIGHTMAX) best = VW_LIGHTMAX;
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

bool voxworld_findtop(int32_t x, int32_t y, int type, vec3_t *out)
{
    if (!vw_active) return false;
    int cx, cy, cz;
    voxworld_toblock(x, y, 0, &cx, &cy, &cz);
    for (int r = 1; r < max(vw_sx, vw_sy); r++)
        for (int bx = cx - r; bx <= cx + r; bx++)
            for (int by = cy - r; by <= cy + r; by++)
            {
                if (max(abs(bx - cx), abs(by - cy)) != r || !vw_inxy(bx, by)) continue;
                vwcolumn_t const *c = vw_col(bx, by);
                if (!c->nsruns) continue;
                int const top = c->sruns[c->nsruns-1].z1;
                if (vw_colblocks(bx, by)[top - 1] != type) continue;
                out->x = vw_x0 + bx*VW_BLOCKXY + VW_BLOCKXY/2;
                out->y = vw_y0 + by*VW_BLOCKXY + VW_BLOCKXY/2;
                out->z = vw_zbot - top*VW_BLOCKZ;
                return true;
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
    T_GRAVEL, T_COAL, T_IRON, T_GOLD, T_ATOMIC, T_LAVA, T_WATER, T_OBSIDIAN, T_TALLGRASS, T_FLOWER_RED, T_FLOWER_YELLOW,
    T_GLASS, T_SNOW, T_SNOW_SIDE,
    T_COUNT
};

// hardness: damage points to destroy (0 = indestructible). Duke's pistol does ~6 per shot.
// emit: light level given off (0 = none, up to VW_LIGHTMAX); tint: its colour (VW_TINT_*); flags: VW_BF_*.
struct vwblockdef_t { uint8_t top, side, bottom, hardness, emit = 0, tint = 0, flags = 0; };

#define VW_LAVAFLAGS (VW_BF_NONSOLID | VW_BF_FLUID | VW_BF_FULLBRIGHT | VW_BF_LAVA)
#define VW_WATERFLAGS (VW_BF_NONSOLID | VW_BF_FLUID | VW_BF_WATER | VW_BF_CLEAR)
#define VW_PLANTFLAGS (VW_BF_NONSOLID | VW_BF_CLEAR | VW_BF_PLANT)

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
    { T_TALLGRASS, T_TALLGRASS, T_TALLGRASS, 1, 0, 0, VW_PLANTFLAGS },        // tall grass
    { T_FLOWER_RED, T_FLOWER_RED, T_FLOWER_RED, 1, 0, 0, VW_PLANTFLAGS },     // red flower
    { T_FLOWER_YELLOW, T_FLOWER_YELLOW, T_FLOWER_YELLOW, 1, 0, 0, VW_PLANTFLAGS },  // yellow flower
    { T_GLASS, T_GLASS, T_GLASS, 3, 0, 0, VW_BF_GLASS },  // glass: one shot breaks it
    { T_SNOW, T_SNOW_SIDE, T_DIRT, 12 },            // snow (on dirt, like grass)
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

// Small deterministic RNG for generation.
struct vwrng_t
{
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float    f() { return (next() & 0xffffff) * (1.f / 16777216.f); }
    int      range(int a, int b) { return b <= a ? a : a + (int)(next() % (uint32_t)(b - a + 1)); }
};

//////////////////////////////////////////////////////////////////////////////
// Terrain generation, a chunk at a time. Every chunk comes out the same whichever order chunks are made in: the
// chunk plus an apron of VW_APRON blocks round it is built in a scratch buffer (things that spill a little way,
// like tree leaves, come from the apron), from global noise and from caves and veins that are seeded per cell of
// the world and replayed wherever they reach. Coordinates here are global blocks.
//////////////////////////////////////////////////////////////////////////////

#define VW_APRON 2
#define VW_GENW  (VW_CHUNK + 2*VW_APRON)

static uint8_t *vw_gbuf;                 // scratch: [((x*VW_GENW) + y)*vw_sz + z]
static int      vw_gx0, vw_gy0;          // global block coords of the scratch's (0,0)
static int      vw_gh[VW_GENW*VW_GENW];  // terrain height per scratch column

// Height (top solid layer) of the untouched terrain at a global column.
static int vw_terrainh(int x, int y)
{
    float const hills = vw_fbm(x / 64.f, y / 64.f, vw_seed, 4);
    float const rough = vw_fbm(x / 16.f, y / 16.f, vw_seed + 7, 3);
    int h = vw_sea - 4 + (int)(hills*hills * vw_amp * 1.4f + rough * 4.f);
    // Lakes: where the lake noise is high the ground eases down into a basin below the water line, deepest
    // in the middle (in hills too: a lake in a valley). None right at the spawn.
    float const lake = vw_fbm(x / 48.f, y / 48.f, vw_seed + 31, 3);
    float const sd = sqrtf((float)((x - vw_wsx/2)*(x - vw_wsx/2) + (y - vw_wsy/2)*(y - vw_wsy/2)));
    float t = clamp((lake - 0.55f) / 0.08f, 0.f, 1.f) * clamp((sd - 24.f) / 10.f, 0.f, 1.f);
    if (t > 0.f)
    {
        t = t*t*(3.f - 2.f*t);
        float const bed = vw_sea - 2 - (lake - 0.55f) * 60.f;
        h = (int)lrintf(h + (min(bed, (float)h) - h) * t);
    }
    return clamp(h, 6, vw_sz - 12);
}

static FORCE_INLINE bool vw_ging(int x, int y) { return (unsigned)(x - vw_gx0) < VW_GENW && (unsigned)(y - vw_gy0) < VW_GENW; }
static FORCE_INLINE uint8_t *vw_gcol(int x, int y) { return &vw_gbuf[((size_t)(x - vw_gx0)*VW_GENW + (y - vw_gy0))*vw_sz]; }
static FORCE_INLINE int vw_height(int x, int y) { return vw_ging(x, y) ? vw_gh[(x - vw_gx0)*VW_GENW + (y - vw_gy0)] : vw_terrainh(x, y); }

static void vw_put(int x, int y, int z, uint8_t t)
{
    if (vw_ging(x, y) && (unsigned)z < (unsigned)vw_sz) vw_gcol(x, y)[z] = t;
}

static uint8_t vw_gget(int x, int y, int z)
{
    return (vw_ging(x, y) && (unsigned)z < (unsigned)vw_sz) ? vw_gcol(x, y)[z] : (uint8_t)VW_AIR;
}

static int vw_surface(int x, int y)
{
    uint8_t const *b = vw_gcol(x, y);
    for (int z = vw_sz-1; z >= 0; z--)
        if (b[z] != VW_AIR && b[z] != VW_LEAVES) return z;
    return -1;
}

static void vw_carve(int x, int y, int z)
{
    if (!vw_ging(x, y) || z < 1 || z >= vw_sz) return;
    int const h = vw_height(x, y);
    // keep the spawn point and the test arch intact
    int const dx = x - (vw_wsx/2 + 3), dy = y - vw_wsy/2;
    if (dx*dx + dy*dy < 10*10 && z > h - 9) return;
    // don't open caves up into lakes (the water would hang there until something disturbed it). The water table
    // fills each column from its terrain height up to the sea line.
    auto wet = [](int x, int y, int z) { return z < vw_sea && z > vw_height(x, y); };
    if (wet(x, y, z) || wet(x, y, z + 1) || wet(x - 1, y, z) || wet(x + 1, y, z) || wet(x, y - 1, z) || wet(x, y + 1, z))
        return;
    uint8_t &b = vw_gcol(x, y)[z];
    if (b != VW_BEDROCK) b = VW_AIR;
}

// Tunnel cross-section: a disc of radius r with a flat floor, `height` layers tall, a layer taller in the middle.
static void vw_carveslice(float cx, float cy, int floor, float r, int height)
{
    for (int x = (int)floorf(cx - r); x <= (int)floorf(cx + r); x++)
        for (int y = (int)floorf(cy - r); y <= (int)floorf(cy + r); y++)
        {
            if (!vw_ging(x, y)) continue;
            float const ex = x + 0.5f - cx, ey = y + 0.5f - cy, d2 = ex*ex + ey*ey;
            if (d2 > r*r) continue;
            int const h = height + (d2 < r*r*0.3f);
            for (int z = floor; z < floor + h; z++) vw_carve(x, y, z);
        }
}

// A cavern: flat floor, domed roof.
static void vw_room(float cx, float cy, int floor, float rx, float ry, int hgt)
{
    for (int x = (int)floorf(cx - rx); x <= (int)floorf(cx + rx); x++)
        for (int y = (int)floorf(cy - ry); y <= (int)floorf(cy + ry); y++)
        {
            if (!vw_ging(x, y)) continue;
            float const ex = (x + 0.5f - cx) / rx, ey = (y + 0.5f - cy) / ry, e = ex*ex + ey*ey;
            if (e > 1.f) continue;
            int const h = 2 + (int)(hgt * sqrtf(1.f - e) + 0.5f);
            for (int z = floor; z < floor + h; z++) vw_carve(x, y, z);
        }
}

// Caves are seeded per VW_CAVECELL-square cell of the world. A cell's worms are walked once and kept as a list of
// carving steps (tunnel slices and caverns) with their bounds; every chunk they reach replays the steps it overlaps.
#define VW_CAVECELL  32
#define VW_CAVEREACH 172  // furthest a worm carves from where it starts: 200 steps of 0.8, plus a cavern
struct vwcaveop_t { float x, y, a, b; int16_t floor; uint8_t room, hgt; };  // tunnel: radius a; cavern: radii a, b
struct vwworm_t { float x0, y0, x1, y1; std::vector<vwcaveop_t> ops; };
static std::unordered_map<uint32_t, std::vector<vwworm_t>> vw_cavecells;

// A wandering tunnel. Mostly level (Duke can't step up a whole block), with the odd one-block step.
// Entrance tunnels start at the surface and stair down into the ground first.
static void vw_worm(std::vector<vwworm_t> &out, vwrng_t &rng, float x, float y, int floor, int len, bool entrance, float yaw = -1.f)
{
    if (yaw < 0.f) yaw = rng.f() * 2.f * fPI;
    float dyaw = 0.f, r = 1.3f + rng.f();
    vwworm_t w;
    w.x0 = w.y0 = FLT_MAX; w.x1 = w.y1 = -FLT_MAX;
    auto grow = [&](float x, float y, float rx, float ry) {
        w.x0 = min(w.x0, x - rx - 1); w.y0 = min(w.y0, y - ry - 1); w.x1 = max(w.x1, x + rx + 1); w.y1 = max(w.y1, y + ry + 1);
    };
    for (int i = 0; i < len; i++)
    {
        dyaw = (dyaw + (rng.f() - 0.5f) * 0.3f) * 0.85f;
        yaw += dyaw;
        x += cosf(yaw) * 0.8f;
        y += sinf(yaw) * 0.8f;
        if (x < 1 || y < 1 || x >= vw_wsx - 1 || y >= vw_wsy - 1) break;
        r = clamp(r + (rng.f() - 0.5f) * 0.2f, 1.2f, 2.6f);

        int const h = vw_terrainh((int)x, (int)y);
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

        w.ops.push_back({ x, y, r, r, (int16_t)floor, 0, 3 });
        grow(x, y, r, r);
        if (!entrance && rng.f() < 0.006f)
        {
            float const rx = 4.f + rng.f()*5.f, ry = 4.f + rng.f()*5.f;
            int const hgt = rng.range(3, 6);
            w.ops.push_back({ x, y, rx, ry, (int16_t)floor, 1, (uint8_t)hgt });
            grow(x, y, rx, ry);
        }
    }
    if (!w.ops.empty()) out.push_back(std::move(w));
}

static std::vector<vwworm_t> const &vw_cavecell(int cx, int cy)
{
    uint32_t const key = ((uint32_t)(uint16_t)cx << 16) | (uint16_t)cy;
    auto it = vw_cavecells.find(key);
    if (it != vw_cavecells.end()) return it->second;
    if (vw_cavecells.size() > 2048) vw_cavecells.clear();  // keep the cache bounded (the window needs ~400 cells)

    std::vector<vwworm_t> worms;
    vwrng_t rng = { vw_hash(cx, cy, vw_seed + 1234) | 1u };
    rng.next();
    int const bx = cx * VW_CAVECELL, by = cy * VW_CAVECELL;
    auto count = [&](float mean) { int n = (int)mean; return n + (rng.f() < mean - n); };

    // tunnels (the same density as the old whole-map generator: area * depth / 1100)
    for (int i = 0, n = count(VW_CAVECELL*VW_CAVECELL * vw_depth / 1100.f); i < n; i++)
    {
        float const x = bx + rng.range(0, VW_CAVECELL - 1) + 0.5f, y = by + rng.range(0, VW_CAVECELL - 1) + 0.5f;
        int const floor = rng.range(5, max(5, vw_terrainh((int)x, (int)y) - 8)), len = rng.range(60, 180);
        if (x < 4 || y < 4 || x >= vw_wsx - 4 || y >= vw_wsy - 4) continue;
        vw_worm(worms, rng, x, y, floor, len, false);
    }
    // entrances from the surface (area / 4000)
    for (int i = 0, n = count(VW_CAVECELL*VW_CAVECELL / 4000.f); i < n; i++)
    {
        float const x = bx + rng.range(0, VW_CAVECELL - 1) + 0.5f, y = by + rng.range(0, VW_CAVECELL - 1) + 0.5f;
        int const len = rng.range(80, 200);
        if (x < 8 || y < 8 || x >= vw_wsx - 8 || y >= vw_wsy - 8) continue;
        vw_worm(worms, rng, x, y, vw_terrainh((int)x, (int)y) - 1, len, true);
    }
    // there's always a cave mouth just ahead of the spawn point (a little to the left)
    int const sx = vw_wsx/2 + 16, sy = vw_wsy/2 - 6;
    if (sx >= bx && sx < bx + VW_CAVECELL && sy >= by && sy < by + VW_CAVECELL)
        vw_worm(worms, rng, sx + 0.5f, sy + 0.5f, vw_terrainh(sx, sy) - 1, rng.range(80, 200), true, 0.f);

    return vw_cavecells.emplace(key, std::move(worms)).first->second;
}

// Replace stone (and optionally dirt) along a short random walk: ore veins, gravel pockets.
static void vw_vein(vwrng_t &rng, uint8_t type, int x, int y, int z, int size, bool anysoil)
{
    for (int k = 0; k < size; k++)
    {
        if (vw_ging(x, y) && z >= 1 && z < vw_sz)
        {
            uint8_t &b = vw_gcol(x, y)[z];
            if (b == VW_STONE || (anysoil && b == VW_DIRT)) b = type;
        }
        switch (rng.next() % 6)
        {
            case 0: x--; break; case 1: x++; break; case 2: y--; break;
            case 3: y++; break; case 4: z--; break; default: z++; break;
        }
    }
}

#define VW_VEINCELL  32
#define VW_VEINREACH 30  // longest vein walk

// Build the chunk whose corner is global block (x0, y0), with its apron, in the scratch buffer.
static void vw_genscratch(int x0, int y0)
{
    if (!vw_gbuf) vw_gbuf = (uint8_t *)Xmalloc((size_t)VW_GENW*VW_GENW*vw_sz);
    vw_gx0 = x0 - VW_APRON;
    vw_gy0 = y0 - VW_APRON;
    int const sea = vw_sea, x1 = vw_gx0 + VW_GENW, y1 = vw_gy0 + VW_GENW;

    // Terrain, and the water table: lakes and ponds up to layer sea-1.
    for (int x = vw_gx0; x < x1; x++)
        for (int y = vw_gy0; y < y1; y++)
        {
            int const h = vw_terrainh(x, y);
            vw_gh[(x - vw_gx0)*VW_GENW + (y - vw_gy0)] = h;
            // snow caps: a ragged snowline, a few layers up or down from column to column
            bool const snowy = vw_snowline && h + (int)(vw_fbm(x / 6.f, y / 6.f, vw_seed + 81, 2) * 6.f) - 3 >= vw_snowline;
            uint8_t *b = vw_gcol(x, y);
            Bmemset(b, VW_AIR, vw_sz);
            for (int z = 0; z <= h; z++)
            {
                uint8_t t = VW_STONE;
                if (z > h - 4)
                {
                    if (h < sea - 1)  // lake bed: sand with patches of gravel
                        t = (vw_hash(x >> 2, y >> 2, vw_seed + 41) % 3) ? VW_SAND : VW_GRAVEL;
                    else
                        t = (h <= sea + 1) ? VW_SAND : (z == h ? (snowy ? VW_SNOW : VW_GRASS) : VW_DIRT);
                }
                // Bedrock: solid at layer 0, then thinning out over layers 1-4 (4/5 .. 1/5), so the floor is lumpy.
                if (z == 0 || (z <= 4 && (int)(vw_hash(x * 5 + z, y, vw_seed + 17) % 5) < 5 - z)) t = VW_BEDROCK;
                b[z] = t;
            }
            for (int z = h + 1; z < sea; z++) b[z] = VW_WATER;
        }

    // Caves: every worm from cells near enough to reach the scratch carves the bits that land in it.
    for (int cx = max(0, (vw_gx0 - VW_CAVEREACH) / VW_CAVECELL); cx <= (x1 + VW_CAVEREACH) / VW_CAVECELL; cx++)
        for (int cy = max(0, (vw_gy0 - VW_CAVEREACH) / VW_CAVECELL); cy <= (y1 + VW_CAVEREACH) / VW_CAVECELL; cy++)
        {
            if (cx * VW_CAVECELL >= vw_wsx || cy * VW_CAVECELL >= vw_wsy) continue;
            for (auto const &w : vw_cavecell(cx, cy))
            {
                if (w.x1 < vw_gx0 || w.y1 < vw_gy0 || w.x0 > x1 || w.y0 > y1) continue;
                for (auto const &o : w.ops)
                {
                    if (o.x + o.a + 1 < vw_gx0 || o.y + o.b + 1 < vw_gy0 || o.x - o.a - 1 > x1 || o.y - o.b - 1 > y1) continue;
                    if (o.room) vw_room(o.x, o.y, o.floor, o.a, o.b, o.hgt);
                    else vw_carveslice(o.x, o.y, o.floor, o.a, o.hgt);
                }
            }
        }

    // Lava: open cave below the lava line fills with lava (still sources; it only flows once you dig).
    for (int x = vw_gx0; x < x1; x++)
        for (int y = vw_gy0; y < y1; y++)
        {
            uint8_t *b = vw_gcol(x, y);
            for (int z = 5; z <= vw_lavaline; z++)
                if (b[z] == VW_AIR) b[z] = VW_LAVA;
        }

    // Ores and gravel (only replace stone, so they line cave walls rather than floating in them), seeded per cell
    // and laid in the same order everywhere. Depth bands (with the default world): coal anywhere in the rock, iron
    // in the lower two thirds, gold only in the deepest third.
    for (int cx = max(0, (vw_gx0 - VW_VEINREACH) / VW_VEINCELL); cx <= (x1 + VW_VEINREACH) / VW_VEINCELL; cx++)
        for (int cy = max(0, (vw_gy0 - VW_VEINREACH) / VW_VEINCELL); cy <= (y1 + VW_VEINREACH) / VW_VEINCELL; cy++)
        {
            vwrng_t rng = { vw_hash(cx, cy, vw_seed + 2345) | 1u };
            rng.next();
            float const area = VW_VEINCELL * VW_VEINCELL * vw_depth;
            auto count = [&](float mean) { int n = (int)mean; return n + (rng.f() < mean - n); };
            int const bx = cx * VW_VEINCELL, by = cy * VW_VEINCELL, c = VW_VEINCELL - 1;
            for (int i = 0, n = count(area / 35); i < n; i++)
                vw_vein(rng, VW_COAL, bx + rng.range(0, c), by + rng.range(0, c), rng.range(5, sea + vw_amp/2), rng.range(5, 10), false);
            for (int i = 0, n = count(area / 70); i < n; i++)
                vw_vein(rng, VW_IRON, bx + rng.range(0, c), by + rng.range(0, c), rng.range(5, sea*2/3), rng.range(4, 8), false);
            for (int i = 0, n = count(area / 500); i < n; i++)
                vw_vein(rng, VW_GOLD, bx + rng.range(0, c), by + rng.range(0, c), rng.range(5, max(6, sea/3)), rng.range(3, 6), false);
            for (int i = 0, n = count(area / 2500); i < n; i++)
                vw_vein(rng, VW_ATOMIC, bx + rng.range(0, c), by + rng.range(0, c), rng.range(5, max(6, sea/4)), rng.range(2, 4), false);
            for (int i = 0, n = count(area / 250); i < n; i++)
                vw_vein(rng, VW_GRAVEL, bx + rng.range(0, c), by + rng.range(0, c), rng.range(5, sea + 8), rng.range(15, 30), true);
        }

    // Trees (a tree's leaves reach 2 blocks, which is why the scratch has an apron of 2).
    for (int x = max(vw_gx0, 3); x < min(x1, vw_wsx - 3); x++)
        for (int y = max(vw_gy0, 3); y < min(y1, vw_wsy - 3); y++)
        {
            if (vw_hash(x, y, vw_seed + 99) % 97) continue;
            int const s = vw_surface(x, y);
            if (s < 0 || vw_gcol(x, y)[s] != VW_GRASS) continue;
            int const trunk = 4 + vw_hash(x, y, vw_seed + 5) % 3;
            for (int z = 1; z <= trunk; z++) vw_put(x, y, s + z, VW_WOOD);
            for (int dz = trunk - 2; dz <= trunk + 1; dz++)
            {
                int const r = (dz > trunk - 1) ? 1 : 2;
                for (int dx = -r; dx <= r; dx++)
                    for (int dy = -r; dy <= r; dy++)
                        if ((dx || dy || dz > trunk) && abs(dx) + abs(dy) <= r + 1 && vw_gget(x+dx, y+dy, s+dz) == VW_AIR)
                            vw_put(x + dx, y + dy, s + dz, VW_LEAVES);
            }
        }

    // Plants on open grass: mostly tall grass, thicker in meadow patches; the odd sparse clump of flowers of one colour.
    for (int x = vw_gx0; x < x1; x++)
        for (int y = vw_gy0; y < y1; y++)
        {
            uint8_t *b = vw_gcol(x, y);
            int const s = vw_height(x, y);
            if (s + 1 >= vw_sz || b[s] != VW_GRASS || b[s+1] != VW_AIR) continue;
            uint32_t const h = vw_hash(x, y, vw_seed + 61) % 1000;
            float const meadow = vw_fbm(x / 24.f, y / 24.f, vw_seed + 63, 2);
            float const flowers = vw_fbm(x / 10.f, y / 10.f, vw_seed + 67, 2);
            if (flowers > 0.68f && h < 60)
                b[s+1] = (vw_fbm(x / 40.f, y / 40.f, vw_seed + 71, 1) > 0.5f) ? VW_FLOWER_RED : VW_FLOWER_YELLOW;
            else if (h < (int)(150 + clamp((meadow - 0.4f) * 1500.f, 0.f, 450.f)))
                b[s+1] = VW_TALLGRASS;
        }

    // A test structure by the spawn: a planks arch with a cobble floor, to show overhangs.
    {
        int const cx = vw_wsx/2 + 6, cy = vw_wsy/2;
        if (cx + 4 >= vw_gx0 && cx - 4 < x1 && cy + 7 >= vw_gy0 && cy - 1 < y1)
        {
            int const s = vw_terrainh(cx, cy) + 1;
            for (int dx = -4; dx <= 4; dx++)
                for (int dy = -1; dy <= 1; dy++)
                {
                    vw_put(cx + dx, cy + dy, s - 1, VW_COBBLE);
                    for (int z = s; z < s + 6; z++) vw_put(cx + dx, cy + dy, z, VW_AIR);
                    vw_put(cx + dx, cy + dy, s + 4, VW_PLANKS);
                    if (abs(dx) == 4) for (int z = s; z < s + 4; z++) vw_put(cx + dx, cy + dy, z, VW_PLANKS);
                }
            // a glass window in the far end wall
            for (int dy = -1; dy <= 1; dy++)
                for (int z = s + 1; z <= s + 2; z++) vw_put(cx + 4, cy + dy, z, VW_GLASS);
            // The middle of the end wall facing the spawn is gravel with sand on top: shoot it to see blocks fall.
            for (int dy = -1; dy <= 1; dy++)
            {
                for (int z = s; z <= s + 4; z++) vw_put(cx - 4, cy + dy, z, VW_GRAVEL);
                for (int z = s + 5; z <= s + 6; z++) vw_put(cx - 4, cy + dy, z, VW_SAND);
            }
            // a floating slab to look up at, half of it glass
            for (int dx = -2; dx <= 2; dx++)
                for (int dy = 4; dy <= 7; dy++)
                    vw_put(cx + dx, cy + dy, s + 3, dy >= 6 ? VW_GLASS : VW_STONE);
        }
    }
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

static void vw_loadchunk(int lcx, int lcy, bool light);
static std::unordered_map<uint32_t, std::vector<uint8_t>> vw_saved;  // see Streaming

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
    vw_wsx  = clamp((maxx - minx) / VW_BLOCKXY, VW_CHUNK, 4096);
    vw_wsy  = clamp((maxy - miny) / VW_BLOCKXY, VW_CHUNK, 4096);
    vw_sx   = min(VW_WINDOW, vw_wsx / VW_CHUNK * VW_CHUNK);
    vw_sy   = min(VW_WINDOW, vw_wsy / VW_CHUNK * VW_CHUNK);
    vw_ncx  = vw_sx / VW_CHUNK;
    vw_ncy  = vw_sy / VW_CHUNK;
    vw_sz   = clamp((sec.floorz - sec.ceilingz) / VW_BLOCKZ - 2, 8, 255);
    vw_zbot = sec.floorz;
    vw_seed = (uint16_t)sec.hitag;
    vw_wx0  = minx;
    vw_wy0  = miny;

    // Heights in layers. With the default 160-layer world: lowlands/beaches around 60-64, hills up to ~105,
    // ~60 layers of underground below the lowlands, and the sky above.
    vw_sea      = min(64, vw_sz*2/5);                    // lowland level; beaches at or below it
    vw_amp      = max(8, min(44, vw_sz - vw_sea - 24));  // hill height range (leaves room for trees)
    vw_depth    = max(1, vw_sea / 16);                   // underground volume relative to the old 64-layer world
    vw_lavaline = max(6, vw_sea / 6);                    // ~layer 10 in the default world
    vw_snowline = vw_sea + vw_amp * 11 / 20;             // ~layer 88: the tops of the higher hills

    // The window starts centred on the player's start (whole chunks, inside the world).
    int const sbx = dapos ? (dapos->x - minx) / VW_BLOCKXY : vw_wsx / 2, sby = dapos ? (dapos->y - miny) / VW_BLOCKXY : vw_wsy / 2;
    vw_ox = clamp((sbx - vw_sx/2) / VW_CHUNK * VW_CHUNK, 0, (vw_wsx - vw_sx) / VW_CHUNK * VW_CHUNK);
    vw_oy = clamp((sby - vw_sy/2) / VW_CHUNK * VW_CHUNK, 0, (vw_wsy - vw_sy) / VW_CHUNK * VW_CHUNK);
    vw_x0 = vw_wx0 + vw_ox * VW_BLOCKXY;
    vw_y0 = vw_wy0 + vw_oy * VW_BLOCKXY;

    Xfree(vw_blocks);
    Xfree(vw_cols);
    Xfree(vw_cready);
    Xfree(vw_cdirty);
    DO_FREE_AND_NULL(vw_dmg);
    DO_FREE_AND_NULL(vw_ltint);
    DO_FREE_AND_NULL(vw_gbuf);
    vw_saved.clear();
    vw_cavecells.clear();
    vw_nfallers = 0;
    vw_nfluidq[0] = vw_nfluidq[1] = 0;
    vw_blocks = (uint8_t *)Xcalloc((size_t)vw_sx*vw_sy*vw_sz, 1);
    vw_cols   = (vwcolumn_t *)Xcalloc((size_t)vw_sx*vw_sy, sizeof(vwcolumn_t));
    vw_cready = (uint8_t *)Xcalloc(vw_ncx*vw_ncy, 1);
    vw_cdirty = (uint8_t *)Xcalloc(vw_ncx*vw_ncy, 1);

    vw_updatelightshades();
    vw_active = true;
    double const t0 = timerGetFractionalTicks();
    for (int cx = 0; cx < vw_ncx; cx++)
        for (int cy = 0; cy < vw_ncy; cy++)
            vw_loadchunk(cx, cy, false);
    vw_computelight();
    {
        int snow = 0, hist[256] = {};
        for (int x = 0; x < vw_sx; x++)
            for (int y = 0; y < vw_sy; y++)
            {
                vwcolumn_t const *c = vw_col(x, y);
                if (!c->nruns) continue;
                snow += vw_colblocks(x, y)[c->runs[c->nruns-1].z1 - 1] == VW_SNOW;
                hist[c->runs[c->nruns-1].z1 - 1]++;
            }
        int pct[3] = { 90, 99, 100 }, at[3] = {}, acc = 0, k = 0;
        for (int z = 0; z < 256 && k < 3; z++)
            for (acc += hist[z]; k < 3 && acc * 100 >= pct[k] * vw_sx * vw_sy; k++) at[k] = z;
        LOG_F(INFO, "voxworld: %d%% of the window's columns are snow-capped (snowline %d); ground tops: 90%% <= %d, 99%% <= %d, max %d",
              snow * 100 / (vw_sx*vw_sy), vw_snowline, at[0], at[1], at[2]);
    }
    LOG_F(INFO, "voxworld: world %dx%dx%d blocks, window %dx%d at %d,%d, seed %u, generated in %.1f ms", vw_wsx, vw_wsy, vw_sz,
          vw_sx, vw_sy, vw_ox, vw_oy, vw_seed, timerGetFractionalTicks() - t0);

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
static float   *vw_pz;       // one screen column: depth of the nearest plant texel (FLT_MAX: none)
static uint8_t *vw_pcol;     // its colour
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

// Under water the view fades smoothly into vw_fogcol between VW_WATERNEAR and VW_UWFAR, in VW_FOGSTEPS palette
// steps (each a colour -> nearest-palette-colour table, rebuilt only when the fog colour changes).
#define VW_UWFAR     (24.f*VW_BLOCKXY)
#define VW_FOGSTEPS  12
static uint8_t vw_fogtab[VW_FOGSTEPS][256];
static int     vw_fogtabcol = -1;
static float   vw_fogscale;

static void vw_buildfog(void)
{
    vw_fogscale = VW_FOGSTEPS / (VW_UWFAR - VW_WATERNEAR);
    if (vw_fogtabcol == vw_fogcol) return;
    vw_fogtabcol = vw_fogcol;
    palette_t const f = curpalette[vw_fogcol];
    for (int k = 0; k < VW_FOGSTEPS; k++)
    {
        int const w = (k + 1) * 256 / (VW_FOGSTEPS + 1);  // fog weight, 0..256
        for (int c = 0; c < 256; c++)
        {
            palette_t const s = curpalette[c];
            vw_fogtab[k][c] = paletteGetClosestColorUpToIndex(s.r + (((f.r - s.r) * w) >> 8), s.g + (((f.g - s.g) * w) >> 8),
                                                              s.b + (((f.b - s.b) * w) >> 8), 254);
        }
    }
}

static FORCE_INLINE uint8_t vw_uwfog(uint8_t c, float d)
{
    if (d <= VW_WATERNEAR) return c;
    int const k = (int)((d - VW_WATERNEAR) * vw_fogscale);
    return k < VW_FOGSTEPS ? vw_fogtab[k][c] : vw_fogcol;
}

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
                if (pb && !vw_skyclear(pb[z])) { z++; continue; }  // water beyond water: no face; solid: can't see it
                int z1 = z + 1;
                while (z1 < r.z1 && !(pb && !vw_skyclear(pb[z1]))) z1++;
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

        // Top (the surface, seen from above, or from below when the camera is under water and there's air over it)
        // and bottom (seen from below, only over open air).
        for (int face = 0; face < 2; face++)
        {
            float const zf = (float)vw_layertop(face ? r.z0 : r.z1);
            bool const under = zf < vw_cz;  // the face is above the eye: seen from below
            if (face == 0 ? !(!under || (vw_camwater && r.z1 < vw_sz && vw_skyclear(cb[r.z1])))
                          : !(under && r.z0 > 0 && vw_skyclear(cb[r.z0-1]))) continue;
            float const hz = (zf - vw_cz) * vw_ky;
            int const a = max(vw_rowof(vw_horiz + hz / (under ? dclip : dout)), 0);
            int const b = min(vw_rowof(vw_horiz + hz / (under ? dout : dclip)), ydim);
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
            if (face || !under) murky(a, b, dout);  // from below, the rows go on up into the air above
        }
    }
}

// The plants of block column c (cell gx,gy) along this screen column's ray. Each is a camera-facing billboard a
// block wide through the cell's centre; that stays inside the cell from every angle, so visiting cells in ray order
// still draws plants front to back. Opaque texels go into the plant overlay (vw_pcol/vw_pz), nearest first.
static __attribute__((noinline)) void vw_drawplantcell(vwcolumn_t const *c, uint8_t const *cb, int gx, int gy, float k,
                                                       vwgap_t const *gaps, int ng)
{
    float const ccx = vw_x0 + (gx + 0.5f) * VW_BLOCKXY - vw_cx, ccy = vw_y0 + (gy + 0.5f) * VW_BLOCKXY - vw_cy;
    float const d = ccx*vw_ca + ccy*vw_sa;  // depth of the billboard
    if (d <= VW_NEAR) return;
    float const u = (k*d - (ccy*vw_ca - ccx*vw_sa)) * (1.f / VW_BLOCKXY) + 0.5f;  // across it, 0..1 left to right
    if (!(u >= 0.f && u < 1.f)) return;
    int const tu = (int)(u * VW_TEXSIZE);
    float const inv = vw_ky / d, zperrow = d / vw_ky, texperz = (float)VW_TEXSIZE / VW_BLOCKZ;
    int const ydim = ydimen;

    for (int i = 0; i < c->nplants; i++)
    {
        int const z = c->plants[i];
        uint8_t const *const tcol = vw_tex[vw_blockdefs[cb[z]].side] + tu*VW_TEXSIZE;
        float const zb = (float)vw_layertop(z);
        int const a = max(vw_rowof(vw_horiz + ((float)vw_layertop(z + 1) - vw_cz)*inv), 0);
        int const b = min(vw_rowof(vw_horiz + (zb - vw_cz)*inv), ydim);
        if (a >= b) continue;
        int tint;
        uint8_t const *const pal = vw_shadetab(0, vw_lightshade(gx, gy, z, &tint), d, tint);
        for (int g = 0; g < ng; g++)
        {
            if (b <= gaps[g].a) break;
            int const ya = max<int>(a, gaps[g].a), yb = min<int>(b, gaps[g].b);
            for (int y = ya; y < yb; y++)
            {
                if (d >= vw_pz[y]) continue;  // something nearer (glass, another plant) already there
                int const t = (int)((zb - (vw_cz + (y + 0.5f - vw_horiz)*zperrow)) * texperz);  // texels up from the bottom
                uint8_t const texel = tcol[VW_TEXSIZE-1 - clamp(t, 0, VW_TEXSIZE-1)];
                if (texel == 255) continue;  // transparent
                vw_pcol[y] = pal[texel];
                vw_pz[y] = d;
            }
        }
    }
}

// The glass of block column c (cell gx,gy) along this screen column's ray: its faces against anything that isn't
// glass, on the near side (entered at din through `side`) and the far side (left at dout into cell nx,ny through
// `nside`), plus tops and bottoms: you see a glass block's whole frame. Opaque texels go into the cut-out overlay
// with the plants, nearest wins.
static __attribute__((noinline)) void vw_drawglasscell(vwcolumn_t const *c, uint8_t const *cb, int gx, int gy, int side, float din,
                                                       int nx, int ny, int nside, float dout, float dx, float dy,
                                                       vwgap_t const *gaps, int ng)
{
    int const ydim = ydimen, maxshade = numshades - 1;
    uint8_t const *const tex = vw_tex[T_GLASS];
    float const ox = vw_cx - vw_x0, oy = vw_cy - vw_y0;
    float const dclip = max(din, VW_NEAR);
    int const px = gx - (side == 0) + (side == 1), py = gy - (side == 2) + (side == 3);
    auto glassat = [](int x, int y, int z) { return vw_inxy(x, y) && vw_isglass(vw_colblocks(x, y)[z]); };

    // rows [a,b) that are still open
    auto rows = [&](int a, int b, auto pix) {
        for (int g = 0; g < ng; g++)
        {
            if (b <= gaps[g].a) break;
            int const ya = max<int>(a, gaps[g].a), yb = min<int>(b, gaps[g].b);
            for (int y = ya; y < yb; y++) pix(y);
        }
    };

    // layers [z0,z1) of the vertical face at depth d, crossed through side s
    auto wall = [&](float d, int s, int z0, int z1) {
        if (d <= VW_NEAR) return;
        float const inv = vw_ky / d;
        float const hx = vw_cx + dx*d, hy = vw_cy + dy*d;
        float const along = (s < 2) ? (hy - vw_y0) : (hx - vw_x0);
        int u = ((int)along >> 5) & (VW_TEXSIZE-1);
        if (s == 0 || s == 3) u = VW_TEXSIZE-1 - u;
        uint8_t const *const tcol = tex + u*VW_TEXSIZE;
        float const zperrow = d / vw_ky, texperz = (float)VW_TEXSIZE / VW_BLOCKZ;
        int tint;
        int const ls = vw_lightshade(gx, gy, z0, &tint);
        uint8_t const *const pal = vw_shadetab(vw_faceshade[s], ls, d, tint);
        int const a = max(vw_rowof(vw_horiz + ((float)vw_layertop(z1) - vw_cz)*inv), 0);
        int const b = min(vw_rowof(vw_horiz + ((float)vw_layertop(z0) - vw_cz)*inv), ydim);
        rows(a, b, [&](int y) {
            if (d >= vw_pz[y]) return;
            int const t = (int)((vw_zbot - (vw_cz + (y + 0.5f - vw_horiz)*zperrow)) * texperz);
            uint8_t const texel = tcol[VW_TEXSIZE-1 - (t & (VW_TEXSIZE-1))];
            if (texel == 255) return;
            vw_pcol[y] = pal[texel];
            vw_pz[y] = d;
        });
    };

    for (int ri = 0; ri < c->ngruns; ri++)
    {
        vwrun_t const r = c->gruns[ri];

        // near and far sides, where the neighbour there isn't glass
        for (int back = 0; back < 2; back++)
        {
            int const s = back ? nside : side, fx = back ? nx : px, fy = back ? ny : py;
            if (s < 0) continue;
            for (int z = r.z0; z < r.z1;)
            {
                if (glassat(fx, fy, z)) { z++; continue; }
                int z1 = z + 1;
                while (z1 < r.z1 && !glassat(fx, fy, z1)) z1++;
                wall(back ? dout : din, s, z, z1);
                z = z1;
            }
        }

        // top and bottom
        for (int face = 0; face < 2; face++)
        {
            if (face ? (r.z0 > 0 && vw_isglass(cb[r.z0-1])) : (r.z1 < vw_sz && vw_isglass(cb[r.z1]))) continue;
            float const zf = (float)vw_layertop(face ? r.z0 : r.z1);
            float const hz = (zf - vw_cz) * vw_ky;
            bool const above = zf < vw_cz;  // the face is above the eye
            int const a = max(vw_rowof(vw_horiz + hz / (above ? dclip : dout)), 0);
            int const b = min(vw_rowof(vw_horiz + hz / (above ? dout : dclip)), ydim);
            if (a >= b) continue;
            int tint;
            int const fl = vw_lightshade(gx, gy, face ? r.z0 : r.z1 - 1, &tint);
            uint8_t const *const pal0 = vw_tinttable(tint);
            int const fshade = vw_faceshade[face ? 5 : 4];
            rows(a, b, [&](int y) {
                float const d = hz * vw_rowrec[y];
                if (d < dclip || d > dout || d >= vw_pz[y]) return;
                int const u = ((int)(ox + dx*d) >> 5) & (VW_TEXSIZE-1);
                int const v = ((int)(oy + dy*d) >> 5) & (VW_TEXSIZE-1);
                uint8_t const texel = tex[u*VW_TEXSIZE + v];
                if (texel == 255) return;
                int sh = fshade + vw_lanterned(fl, d) + (int)(d * vw_visk);
                sh = sh > maxshade ? maxshade : sh;
                vw_pcol[y] = pal0[(sh << 8) + texel];
                vw_pz[y] = d;
            });
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
    bool cutouts = false;  // the cut-out overlay (plants, glass: vw_pz/vw_pcol) is cleared when the first one comes along
    vwmurkyq_t mq;  // water rows to close once they're deep enough in murk
    // under water, everything past the fog is fog
    float const maxdist = vw_camwater ? min(vw_maxdist, VW_UWFAR + VW_BLOCKXY) : vw_maxdist;

    for (;;)
    {
        float const dout = min(tmx, tmy);

        if (vw_inxy(gx, gy))
        {
            vwcolumn_t const *c = vw_col(gx, gy);
            int const nr = c->nruns, nw = c->nwruns, np = c->nplants, ngl = c->ngruns;
            // Cheap reject: if even the top of this block column projects below every open row, skip it.
            // (The top face's far edge is its highest point when it's below the eye, else the near edge.)
            float const colztop = (float)vw_layertop(max(max(nr ? c->runs[nr-1].z1 : 0, nw ? c->wruns[nw-1].z1 : 0),
                                                         max(np ? c->plants[np-1] + 1 : 0, ngl ? c->gruns[ngl-1].z1 : 0)));
            if ((nr || nw || np || ngl) && din > VW_NEAR)
            {
                float const dz = colztop - vw_cz;
                float const ytop = vw_horiz + dz * vw_ky / (dz > 0.f ? dout : din);
                if (ytop >= gaps[ng-1].b)
                    goto next_cell;
            }
            // water first: it doesn't occlude, and what's in this same cell may be behind it
            if (nw)
                vw_drawwatercell(c, vw_colblocks(gx, gy), gx, gy, side, din, dout, dx, dy, gaps, ng, wcol, wz, mq);
            if ((np || ngl) && !cutouts) { for (int y = 0; y < ydim; y++) vw_pz[y] = FLT_MAX; cutouts = true; }
            if (ngl)
            {
                bool const xnext = tmx < tmy;
                vw_drawglasscell(c, vw_colblocks(gx, gy), gx, gy, side, din, xnext ? gx + stepx : gx, xnext ? gy : gy + stepy,
                                 xnext ? (stepx > 0 ? 0 : 1) : (stepy > 0 ? 2 : 3), dout, dx, dy, gaps, ng);
            }
            if (np)
                vw_drawplantcell(c, vw_colblocks(gx, gy), gx, gy, k, gaps, ng);
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

    // Plants and glass in front of what's behind them (they count as the nearest thing there, for sprites and water too).
    if (cutouts)
        for (int y = 0; y < ydim; y++)
            if (vw_pz[y] < zc[y]) { col[y] = vw_pcol[y]; zc[y] = vw_pz[y]; }

    // Water over whatever is behind it: the further you look through it, the murkier.
    if (vw_camwater)
    {
        // under water: clear close up, fogging into the water's colour further away. The surface seen from
        // below (the only water face drawn from inside) shows over what's above it, then fogs by its own distance.
        for (int y = 0; y < ydim; y++)
            col[y] = wz[y] < zc[y] ? vw_uwfog(vw_waterblend(wcol[y], col[y], VW_WATERCLEAR), wz[y]) : vw_uwfog(col[y], zc[y]);
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
        Xfree(vw_zbuf); Xfree(vw_cbuf); Xfree(vw_rowrec); Xfree(vw_wzbuf); Xfree(vw_wcbuf); Xfree(vw_pz); Xfree(vw_pcol);
        vw_bufw = xdimen; vw_bufh = ydimen;
        vw_zbuf   = (float *)Xmalloc(sizeof(float) * xdimen * ydimen);
        vw_cbuf   = (uint8_t *)Xmalloc(xdimen * ydimen);
        vw_wzbuf  = (float *)Xmalloc(sizeof(float) * xdimen * ydimen);
        vw_wcbuf  = (uint8_t *)Xmalloc(xdimen * ydimen);
        vw_rowrec = (float *)Xmalloc(sizeof(float) * ydimen);
        vw_pz     = (float *)Xmalloc(sizeof(float) * ydimen);
        vw_pcol   = (uint8_t *)Xmalloc(ydimen);
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
            vw_buildfog();
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
            uint8_t const sc = vw_camwater ? vw_uwfog(lut[c], depth)
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

// Plants a hitscan (a raycast with a hit record) went through on its way, for voxworld_smashplants.
#define VW_MAXSHOTPLANTS 16
static struct { int16_t x, y, z; } vw_shotplants[VW_MAXSHOTPLANTS];
static int vw_nshotplants;

bool voxworld_raycast(const vec3_t *start, int32_t vx, int32_t vy, int32_t vz, float maxt, vwhit_t *hit)
{
    if (!vw_active) return false;
    if (hit) vw_nshotplants = 0;

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
            if (hit && vw_isplant(b) && vw_nshotplants < VW_MAXSHOTPLANTS)
                vw_shotplants[vw_nshotplants++] = { (int16_t)bx, (int16_t)by, (int16_t)bz };
            if (b != VW_AIR && bz >= 0 && !(vw_flags(b) & VW_BF_CLEAR) && !(!hit && vw_isglass(b)))  // sight sees through glass
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

int voxworld_smashplants(const vec3_t *from, int32_t reach, vwdestroyfn_t fn, void *ud)
{
    int n = 0;
    for (int i = 0; i < vw_nshotplants; i++)
    {
        auto const &p = vw_shotplants[i];
        uint8_t const type = voxworld_get(p.x, p.y, p.z);
        if (!vw_isplant(type)) continue;
        if (reach > 0)
        {
            int32_t const cx = vw_x0 + p.x*VW_BLOCKXY + VW_BLOCKXY/2, cy = vw_y0 + p.y*VW_BLOCKXY + VW_BLOCKXY/2;
            if (klabs(cx - from->x) + klabs(cy - from->y) > reach + VW_BLOCKXY/2) continue;
        }
        voxworld_set(p.x, p.y, p.z, VW_AIR);
        if (fn) fn(p.x, p.y, p.z, type, ud);
        n++;
    }
    vw_nshotplants = 0;
    return n;
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
    if (!vw_isopen(below) && !(vw_flags(below) & VW_BF_FLUID)) return;
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
                if (!vw_isopen(nb) && !(vw_flags(nb) & VW_BF_FLUID))  // sinks through water and lava, crushes plants
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
    if (vw_isopen(t))  // washes plants away
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

//////////////////////////////////////////////////////////////////////////////
// Streaming. The window follows the player a chunk at a time: when they're more than a chunk and a half from its
// centre it slides over (the arrays shift in memory), the chunks that leave are saved if they were changed, and the
// chunks that come in start as solid placeholders that get generated (or restored) a few per tic, nearest first.
// With the default view distance that all happens out of sight.
//////////////////////////////////////////////////////////////////////////////

static FORCE_INLINE uint32_t vw_chunkkey(int gcx, int gcy) { return ((uint32_t)gcx << 16) | (uint32_t)gcy; }

// Saved chunks are run-length coded: (count, type) pairs, column after column.
static void vw_savechunk(int lcx, int lcy)
{
    std::vector<uint8_t> out;
    out.reserve(VW_CHUNK*VW_CHUNK*16);
    for (int x = lcx*VW_CHUNK; x < (lcx+1)*VW_CHUNK; x++)
        for (int y = lcy*VW_CHUNK; y < (lcy+1)*VW_CHUNK; y++)
        {
            uint8_t const *b = vw_colblocks(x, y);
            for (int z = 0; z < vw_sz;)
            {
                int n = 1;
                while (z + n < vw_sz && n < 255 && b[z+n] == b[z]) n++;
                out.push_back((uint8_t)n);
                out.push_back(b[z]);
                z += n;
            }
        }
    vw_saved[vw_chunkkey((vw_ox / VW_CHUNK) + lcx, (vw_oy / VW_CHUNK) + lcy)] = std::move(out);
}

static bool vw_restorechunk(int lcx, int lcy)
{
    auto it = vw_saved.find(vw_chunkkey((vw_ox / VW_CHUNK) + lcx, (vw_oy / VW_CHUNK) + lcy));
    if (it == vw_saved.end()) return false;
    uint8_t const *p = it->second.data();
    for (int x = lcx*VW_CHUNK; x < (lcx+1)*VW_CHUNK; x++)
        for (int y = lcy*VW_CHUNK; y < (lcy+1)*VW_CHUNK; y++)
        {
            uint8_t *b = vw_colblocks(x, y);
            for (int z = 0; z < vw_sz; p += 2)
            {
                Bmemset(b + z, p[1], p[0]);
                z += p[0];
            }
        }
    return true;
}

static int vw_skyfloor(int bx, int by)
{
    if (!vw_inxy(bx, by)) return 0;
    vwcolumn_t const *c = vw_col(bx, by);
    return max(c->nruns ? c->runs[c->nruns-1].z1 : 0, c->nwruns ? c->wruns[c->nwruns-1].z1 : 0);
}

// Light a freshly loaded chunk: its own sky and emitters, plus whatever light comes in from loaded neighbours (and
// goes out to them: flooding only ever raises light).
static void vw_lightchunk(int lcx, int lcy)
{
    if (!vw_light) return;
    int const x0 = lcx*VW_CHUNK, y0 = lcy*VW_CHUNK, x1 = x0 + VW_CHUNK, y1 = y0 + VW_CHUNK;
    for (int x = x0; x < x1; x++)
        for (int y = y0; y < y1; y++)
        {
            size_t const base = ((size_t)x*vw_sy + y)*vw_sz;
            Bmemset(&vw_light[base], 0, vw_sz);
            Bmemset(&vw_ltint[base], 0, vw_sz);
            for (int z = vw_skyfloor(x, y); z < vw_sz; z++) vw_light[base + z] = VW_LIGHTMAX;
        }

    for (int ch = 0; ch < 2; ch++)
    {
        size_t n = 0;
        for (int x = x0 - 1; x <= x1; x++)
            for (int y = y0 - 1; y <= y1; y++)
            {
                if (!vw_inxy(x, y)) continue;
                bool const inside = x >= x0 && x < x1 && y >= y0 && y < y1;
                if (inside)
                {
                    if (ch == 0)
                    {
                        // as vw_computelight: sky cells beside non-sky ones, and daylight into a water surface
                        int const f = vw_skyfloor(x, y);
                        int const top = max(max(vw_skyfloor(x-1, y), vw_skyfloor(x+1, y)), max(vw_skyfloor(x, y-1), vw_skyfloor(x, y+1)));
                        for (int z = f; z < min(top, vw_sz); z++) vw_lightpush(n, vw_lidx(x, y, z));
                        if (f > 0 && f < vw_sz && vw_iswater(vw_colblocks(x, y)[f-1])) vw_lightpush(n, vw_lidx(x, y, f));
                    }
                    else
                    {
                        uint8_t const *b = vw_colblocks(x, y);
                        for (int z = 0; z < vw_sz; z++)
                            if (int const e = vw_emit(b[z]))
                            {
                                uint32_t const i = vw_lidx(x, y, z);
                                vw_lset(i, 1, e);
                                vw_ltint[i] = vw_emittint(b[z]);
                                vw_lightpush(n, i);
                            }
                    }
                }
                else if (vw_cready[vw_chunkof(x, y)])
                {
                    // a loaded neighbour's light spreads in
                    for (int z = 0; z < vw_sz; z++)
                    {
                        uint32_t const i = vw_lidx(x, y, z);
                        if (vw_lget(i, ch) > 1) vw_lightpush(n, i);
                    }
                }
            }
        vw_lightflood(n, ch);
    }
}

// Placeholder for a chunk not generated yet: solid bedrock (nothing can get in, light stays out), unlit.
static void vw_placeholder(int lcx, int lcy)
{
    for (int x = lcx*VW_CHUNK; x < (lcx+1)*VW_CHUNK; x++)
        for (int y = lcy*VW_CHUNK; y < (lcy+1)*VW_CHUNK; y++)
        {
            size_t const base = ((size_t)x*vw_sy + y)*vw_sz;
            Bmemset(&vw_blocks[base], VW_BEDROCK, vw_sz);
            if (vw_light) Bmemset(&vw_light[base], 0, vw_sz);
            if (vw_ltint) Bmemset(&vw_ltint[base], 0, vw_sz);
            if (vw_dmg) Bmemset(&vw_dmg[base], 0, vw_sz);
            vw_rebuildcolumn(x, y);
        }
    vw_cready[lcx*vw_ncy + lcy] = 0;
    vw_cdirty[lcx*vw_ncy + lcy] = 0;
}

// Generate (or restore, if it was changed in play) chunk (lcx, lcy) of the window.
static void vw_loadchunk(int lcx, int lcy, bool light)
{
    bool const restored = vw_restorechunk(lcx, lcy);
    if (!restored)
    {
        int const gx0 = vw_ox + lcx*VW_CHUNK, gy0 = vw_oy + lcy*VW_CHUNK;
        vw_genscratch(gx0, gy0);
        for (int x = 0; x < VW_CHUNK; x++)
            for (int y = 0; y < VW_CHUNK; y++)
                Bmemcpy(vw_colblocks(lcx*VW_CHUNK + x, lcy*VW_CHUNK + y), vw_gcol(gx0 + x, gy0 + y), vw_sz);
    }
    for (int x = lcx*VW_CHUNK; x < (lcx+1)*VW_CHUNK; x++)
        for (int y = lcy*VW_CHUNK; y < (lcy+1)*VW_CHUNK; y++)
        {
            vw_rebuildcolumn(x, y);
            if (vw_dmg) Bmemset(&vw_dmg[((size_t)x*vw_sy + y)*vw_sz], 0, vw_sz);
        }
    vw_cready[lcx*vw_ncy + lcy] = 1;
    vw_cdirty[lcx*vw_ncy + lcy] = 0;
    if (light) vw_lightchunk(lcx, lcy);

    // A restored chunk may have been left mid-flow: let any fluid with somewhere to go carry on.
    if (restored)
        for (int x = lcx*VW_CHUNK; x < (lcx+1)*VW_CHUNK; x++)
            for (int y = lcy*VW_CHUNK; y < (lcy+1)*VW_CHUNK; y++)
            {
                uint8_t const *b = vw_colblocks(x, y);
                for (int z = 1; z < vw_sz; z++)
                {
                    if (!(vw_flags(b[z]) & VW_BF_FLUID)) continue;
                    auto open = [](int x, int y, int z) { return vw_inxy(x, y) && vw_isopen(vw_colblocks(x, y)[z]); };
                    if (vw_isopen(b[z-1]) || open(x-1, y, z) || open(x+1, y, z) || open(x, y-1, z) || open(x, y+1, z))
                        vw_fluidqueue(x, y, z);
                }
            }
}

// Shift a w x h grid of columns (colbytes each) so the content at (x, y) moves to (x - dx, y - dy).
static void vw_shiftgrid(void *base, size_t colbytes, int w, int h, int dx, int dy)
{
    if (abs(dx) >= w || abs(dy) >= h) return;  // all of it moved out (the caller refills everything)
    uint8_t *const a = (uint8_t *)base;
    size_t const row = colbytes * h;
    if (dx > 0) Bmemmove(a, a + dx*row, (w - dx)*row);
    else if (dx < 0) Bmemmove(a - dx*row, a, (w + dx)*row);
    if (dy)
        for (int x = 0; x < w; x++)
        {
            uint8_t *const r = a + x*row;
            if (dy > 0) Bmemmove(r, r + dy*colbytes, (h - dy)*colbytes);
            else Bmemmove(r - dy*colbytes, r, (h + dy)*colbytes);
        }
}

// Slide the window by (kx, ky) chunks.
static void vw_movewindow(int kx, int ky)
{
    // save the changed chunks that are leaving
    for (int cx = 0; cx < vw_ncx; cx++)
        for (int cy = 0; cy < vw_ncy; cy++)
        {
            int const nx = cx - kx, ny = cy - ky;
            if ((nx < 0 || nx >= vw_ncx || ny < 0 || ny >= vw_ncy) && vw_cready[cx*vw_ncy + cy] && vw_cdirty[cx*vw_ncy + cy])
                vw_savechunk(cx, cy);
        }

    int const dx = kx*VW_CHUNK, dy = ky*VW_CHUNK;
    size_t const sz = vw_sz;
    vw_shiftgrid(vw_blocks, sz, vw_sx, vw_sy, dx, dy);
    vw_shiftgrid(vw_cols, sizeof(vwcolumn_t), vw_sx, vw_sy, dx, dy);
    if (vw_light) vw_shiftgrid(vw_light, sz, vw_sx, vw_sy, dx, dy);
    if (vw_ltint) vw_shiftgrid(vw_ltint, sz, vw_sx, vw_sy, dx, dy);
    if (vw_dmg) vw_shiftgrid(vw_dmg, sz, vw_sx, vw_sy, dx, dy);
    vw_shiftgrid(vw_cready, 1, vw_ncx, vw_ncy, kx, ky);
    vw_shiftgrid(vw_cdirty, 1, vw_ncx, vw_ncy, kx, ky);

    vw_ox += dx; vw_oy += dy;
    vw_x0 = vw_wx0 + vw_ox * VW_BLOCKXY;
    vw_y0 = vw_wy0 + vw_oy * VW_BLOCKXY;

    // the chunks that came in
    for (int cx = 0; cx < vw_ncx; cx++)
        for (int cy = 0; cy < vw_ncy; cy++)
        {
            int const ox = cx + kx, oy = cy + ky;
            if (ox < 0 || ox >= vw_ncx || oy < 0 || oy >= vw_ncy) vw_placeholder(cx, cy);
        }

    // things that remember window block coordinates
    for (int i = 0; i < vw_nfallers; i++)
    {
        vwfaller_t &f = vw_fallers[i];
        f.x -= dx; f.y -= dy;
        if (!vw_inxy(f.x, f.y)) { f = vw_fallers[--vw_nfallers]; i--; }
    }
    for (int k = 0; k < 2; k++)
    {
        size_t m = 0;
        for (size_t i = 0; i < vw_nfluidq[k]; i++)
        {
            uint32_t const idx = vw_fluidq[k][i];
            int const bz = idx % vw_sz, col = idx / vw_sz, bx = col / vw_sy - dx, by = col % vw_sy - dy;
            if (vw_inxy(bx, by)) vw_fluidq[k][m++] = vw_lidx(bx, by, bz);
        }
        vw_nfluidq[k] = m;
    }
    vw_lasthit.valid = false;
    vw_nshotplants = 0;
}

void voxworld_stream(int32_t x, int32_t y)
{
    if (!vw_active || (vw_sx >= vw_wsx && vw_sy >= vw_wsy)) return;  // the whole world fits
    double const t0 = timerGetFractionalTicks();

    // Keep the player within a chunk and a half of the window's centre.
    int const gbx = (x - vw_wx0) / VW_BLOCKXY, gby = (y - vw_wy0) / VW_BLOCKXY;
    int const ddx = gbx - (vw_ox + vw_sx/2), ddy = gby - (vw_oy + vw_sy/2);
    int kx = abs(ddx) > VW_CHUNK*3/2 ? ddx / VW_CHUNK : 0;
    int ky = abs(ddy) > VW_CHUNK*3/2 ? ddy / VW_CHUNK : 0;
    kx = clamp(kx, -vw_ox / VW_CHUNK, (vw_wsx - vw_sx) / VW_CHUNK - vw_ox / VW_CHUNK);
    ky = clamp(ky, -vw_oy / VW_CHUNK, (vw_wsy - vw_sy) / VW_CHUNK - vw_oy / VW_CHUNK);
    double tmove = 0;
    if (kx || ky)
    {
        vw_movewindow(kx, ky);
        tmove = timerGetFractionalTicks() - t0;
    }

    // Generate waiting chunks: right away for any close to the player (after a teleport, say), then the nearest
    // ones first within a small time budget.
    int made = 0;
    int const pcx = (gbx - vw_ox) / VW_CHUNK, pcy = (gby - vw_oy) / VW_CHUNK;
    for (int cx = max(0, pcx - 2); cx <= min(vw_ncx - 1, pcx + 2); cx++)
        for (int cy = max(0, pcy - 2); cy <= min(vw_ncy - 1, pcy + 2); cy++)
            if (!vw_cready[cx*vw_ncy + cy]) { vw_loadchunk(cx, cy, true); made++; }
    for (; made < 8; made++)
    {
        int best = -1, bestd = INT_MAX;
        for (int cx = 0; cx < vw_ncx; cx++)
            for (int cy = 0; cy < vw_ncy; cy++)
                if (!vw_cready[cx*vw_ncy + cy])
                {
                    int const d = (cx - pcx)*(cx - pcx) + (cy - pcy)*(cy - pcy);
                    if (d < bestd) { bestd = d; best = cx*vw_ncy + cy; }
                }
        if (best < 0) break;
        vw_loadchunk(best / vw_ncy, best % vw_ncy, true);
        if (timerGetFractionalTicks() - t0 > 4.0) { made++; break; }
    }

    if (vw_stats && (kx || ky || made))
        LOG_F(INFO, "voxworld: stream: moved %d,%d chunks (%.2f ms), made %d chunks, %.2f ms in all; %d saved chunks",
              kx, ky, tmove, made, timerGetFractionalTicks() - t0, (int)vw_saved.size());
}

// Dev check: does the light built up chunk by chunk match lighting the whole window from scratch?
static int osdcmd_voxlightcheck(osdcmdptr_t UNUSED(parm))
{
    if (!vw_active || !vw_light) return OSDCMD_OK;
    size_t const total = (size_t)vw_sx*vw_sy*vw_sz;
    uint8_t *const was = (uint8_t *)Xmalloc(total);
    Bmemcpy(was, vw_light, total);
    vw_computelight();
    size_t diff[2] = {}, cells = 0;
    int worst = 0;
    for (size_t i = 0; i < total; i++)
    {
        int const col = (int)(i / vw_sz), bx = col / vw_sy, by = col % vw_sy;
        if (!vw_cready[vw_chunkof(bx, by)] || !vw_lightpass(vw_blocks[i])) continue;
        cells++;
        for (int ch = 0; ch < 2; ch++)
        {
            int const a = ch ? was[i] >> 4 : was[i] & 15, b = vw_lget((uint32_t)i, ch);
            if (a != b) { diff[ch]++; worst = max(worst, abs(a - b)); }
        }
    }
    Xfree(was);
    OSD_Printf("voxworld: lightcheck: %d of %d open cells differ in sky light, %d in block light (worst by %d); now relit\n",
               (int)diff[0], (int)cells, (int)diff[1], worst);
    return OSDCMD_OK;
}

static void vw_registerosd(void)
{
    OSD_RegisterFunction("r_voxlightcheck", "r_voxlightcheck: dev - compare streamed lighting with a full relight", osdcmd_voxlightcheck);
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

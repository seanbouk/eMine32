// eMine32: Duke-side glue for the block world - what happens, game-wise, when blocks get shot or blown up.
// The engine (voxworld) owns blocks and damage; this file owns the Duke-flavoured effects.
// Copyright (C) 2026 the eMine32 project. GPLv2.

#include "duke3d.h"
#include "voxworld.h"
#include "emine.h"

// Throw some chunks of the block, a puff of dust and a crunch.
static void Emine_BlockDestroyedFX(int bx, int by, int bz, int type, int ownerSprite, int numChunks, bool withSound)
{
    vec3_t c;
    voxworld_blockcenter(bx, by, bz, &c);
    int const sect = voxworld_sector();

    for (int i = 0; i < numChunks; i++)
    {
        // Same launch as Duke's RANDOMSCRAP, so the existing scrap code flies and spins them.
        int const j = A_InsertSprite(sect, c.x + (krand() & 255) - 128, c.y + (krand() & 255) - 128, c.z + (krand() & 4095) - 2048,
                                     SCRAP6 + (krand() & 15), -8, 20, 20, krand() & 2047, (krand() & 63) + 48, -512 - (krand() & 2047),
                                     ownerSprite, STAT_MISC);
        sprite[j].lotag = VW_DEBRIS_TAG | type;  // drawn as a chunk of this block (voxworld)
    }

    int const smoke = A_Spawn(ownerSprite, SMALLSMOKE);
    setsprite(smoke, &c);

    if (withSound)
    {
        bool const hard = voxworld_hardness(type) >= 24;
        A_PlaySound(hard ? VENT_BUST : DUKE_LAND, smoke);
    }
}

void Emine_HitscanBlock(int shotSprite, const hitdata_t *hit)
{
    int bx, by, bz;
    if (!voxworld_active() || !voxworld_lasthit(&hit->xyz, &bx, &by, &bz))
        return;

    int const damage = sprite[shotSprite].extra > 0 ? sprite[shotSprite].extra : 6;
    int const type = voxworld_damage(bx, by, bz, damage);
    if (type != VW_AIR)
        Emine_BlockDestroyedFX(bx, by, bz, type, sprite[shotSprite].owner >= 0 ? sprite[shotSprite].owner : shotSprite, 5, true);
}

// A shot smashes the plants it passes through: a few bits of leaf fly, no smoke or crunch.
static void Emine_OnPlantSmashed(int bx, int by, int bz, int type, void *ud)
{
    int const owner = *(int *)ud;
    vec3_t c;
    voxworld_blockcenter(bx, by, bz, &c);
    for (int i = 0; i < 3; i++)
    {
        int const j = A_InsertSprite(voxworld_sector(), c.x + (krand() & 255) - 128, c.y + (krand() & 255) - 128, c.z + (krand() & 2047),
                                     SCRAP6 + (krand() & 15), -8, 12, 12, krand() & 2047, (krand() & 31) + 24, -256 - (krand() & 1023),
                                     owner, STAT_MISC);
        sprite[j].lotag = VW_DEBRIS_TAG | type;
    }
}

void Emine_ShotPlants(int shooterSprite, const vec3_t *start, int32_t reach)
{
    if (!voxworld_active()) return;
    voxworld_smashplants(start, reach, Emine_OnPlantSmashed, &shooterSprite);
}

struct emine_blastctx { int owner, count; };

static void Emine_OnBlastDestroyed(int bx, int by, int bz, int type, void *ud)
{
    auto &ctx = *(emine_blastctx *)ud;
    // An explosion can take out dozens of blocks: keep the debris (sprites) and sounds bounded.
    if (ctx.count < 24)
        Emine_BlockDestroyedFX(bx, by, bz, type, ctx.owner, (ctx.count & 1) ? 1 : 2, ctx.count == 0);
    ctx.count++;
}

void Emine_ExplosionBlocks(int spriteNum, int blastRadius, int maxDamage)
{
    if (!voxworld_active() || sprite[spriteNum].sectnum != voxworld_sector())
        return;

    // Blocks get the explosion's full damage at the centre falling to zero at the edge, scaled up so a
    // rocket digs a proper crater (RPG: ~140 damage, 1780 radius -> ~2 blocks of stone).
    emine_blastctx ctx = { spriteNum, 0 };
    voxworld_explode(&sprite[spriteNum].xyz, blastRadius, maxDamage * 2, Emine_OnBlastDestroyed, &ctx);
}

int32_t Emine_FloorZ(int spriteNum, int32_t sectorFloorZ)
{
    if (!voxworld_active() || sprite[spriteNum].sectnum != voxworld_sector())
        return sectorFloorZ;
    auto const &s = sprite[spriteNum];
    return voxworld_floorz(s.x, s.y, s.z, sectorFloorZ);
}

// Dev console: `emine_cave [maxlight]` teleports the player to the nearest dark cave floor.
// `emine_glow [minlight]`: the same, to the nearest dark spot lit by a glowing block.
static int osdcmd_emine_cave(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (!voxworld_active() || p->i < 0) return OSDCMD_OK;
    bool const glow = !Bstrcmp(parm->name, "emine_glow");
    int const arg = parm->numparms >= 1 ? clamp(Batol(parm->parms[0]), 0, VW_LIGHTMAX) : (glow ? 8 : 2);
    vec3_t feet;
    if (!voxworld_findspot(p->pos.x, p->pos.y, glow ? 4 : arg, glow ? arg : 0, &feet))
    {
        OSD_Printf("emine_cave: no cave found\n");
        return OSDCMD_OK;
    }
    p->pos   = { feet.x, feet.y, feet.z - PHEIGHT };
    p->opos  = p->pos;
    p->bobpos = { feet.x, feet.y };
    p->vel   = { 0, 0, 0 };
    setsprite(p->i, &feet);
    OSD_Printf("emine_cave: moved to %d, %d, %d\n", feet.x, feet.y, feet.z);
    return OSDCMD_OK;
}

static int emine_tic;  // game tics since startup
static bool emine_where;  // `emine_where`: print the player's surroundings

// Is any enemy or player overlapping block cell (bx,by,bz)? Crush them.
static bool Emine_CrushCell(int bx, int by, int bz, float speed)
{
    vec3_t c;
    voxworld_blockcenter(bx, by, bz, &c);
    int32_t const top = c.z - VW_BLOCKZ/2, bot = c.z + VW_BLOCKZ/2;
    bool hit = false;

    static const int stats[] = { STAT_PLAYER, STAT_ACTOR, STAT_ZOMBIEACTOR };
    for (int stat : stats)
        for (int i = headspritestat[stat]; i >= 0; i = nextspritestat[i])
        {
            auto const &s = sprite[i];
            if (s.sectnum != voxworld_sector() || s.extra <= 0) continue;
            if (s.picnum != APLAYER && !A_CheckEnemySprite(&s)) continue;
            int32_t const r = max<int32_t>(s.clipdist << 2, 64);
            if (klabs(s.x - c.x) >= VW_BLOCKXY/2 + r || klabs(s.y - c.y) >= VW_BLOCKXY/2 + r) continue;
            int32_t const h = s.picnum == APLAYER ? PHEIGHT + (8<<8) : (tilesiz[s.picnum].y * s.yrepeat) << 2;
            if (s.z - h >= bot || s.z <= top) continue;

            // Crushed: same damage path as an explosion. Harder the further it fell.
            auto &a = actor[i];
            a.htpicnum = RADIUSEXPLOSION;
            a.htextra  = 10 + (int)(speed * 25.f);
            a.htowner  = i;
            a.htang    = krand() & 2047;
            hit = true;
        }
    return hit;
}

static bool Emine_OnWorldEvent(int event, int bx, int by, int bz, int type, float speed)
{
    int const playerSprite = g_player[myconnectindex].ps->i;
    switch (event)
    {
        case VW_EV_FALLHIT:
            return Emine_CrushCell(bx, by, bz, speed);

        case VW_EV_FALLBROKE:
            if (playerSprite >= 0) Emine_BlockDestroyedFX(bx, by, bz, type, playerSprite, 4, true);
            break;

        case VW_EV_FALLLANDED:
        {
            // a thud, but not a wall of them when a whole hillside comes down
            static int lastThud = -1;
            if (playerSprite >= 0 && speed > 0.15f && lastThud != emine_tic)
            {
                lastThud = emine_tic;
                vec3_t c;
                voxworld_blockcenter(bx, by, bz, &c);
                S_PlaySound3D(DUKE_LAND, playerSprite, c);
            }
            break;
        }
    }
    return false;
}

// Is this sprite standing in lava (feet or waist)?
static bool Emine_InLava(spritetype const &s)
{
    return (voxworld_blockflags(voxworld_blockat(s.x, s.y, s.z - 512)) & VW_BF_LAVA) ||
           (voxworld_blockflags(voxworld_blockat(s.x, s.y, s.z - VW_BLOCKZ)) & VW_BF_LAVA);
}

// Lava burns, Duke hurt-floor style: a few points every few tics, pain sound and a red-orange flash.
static void Emine_LavaBurn(void)
{
    if (!voxworld_active() || emine_tic % 6) return;

    auto const p = g_player[myconnectindex].ps;
    if (p->i >= 0)
    {
        auto &s = sprite[p->i];
        if (s.extra > 0 && Emine_InLava(s))
        {
            if (!ud.god) s.extra -= 3 + (krand() & 3);
            P_PalFrom(p, 20, 63, 20, 0);
            if (!A_CheckSoundPlaying(p->i, DUKE_LONGTERM_PAIN))
                A_PlaySound(DUKE_LONGTERM_PAIN, p->i);
        }
    }

    for (int i = headspritestat[STAT_ACTOR]; i >= 0; i = nextspritestat[i])
    {
        auto const &s = sprite[i];
        if (s.extra > 0 && A_CheckEnemySprite(&s) && Emine_InLava(s))
        {
            auto &a = actor[i];
            a.htpicnum = RADIUSEXPLOSION;
            a.htextra  = 6;
            a.htowner  = i;
            a.htang    = s.ang;
        }
    }
}

bool Emine_InWaterAt(vec3_t const &pos)
{
    return voxworld_active() && (voxworld_blockflags(voxworld_blockat(pos.x, pos.y, pos.z)) & VW_BF_WATER);
}

// Swimming starts when the water is a bit over knee deep: this point, measured down from the eye.
#define EMINE_SWIMPOINT (PHEIGHT - 6144)

int Emine_PlayerLotag(int playerNum, int lotag, uint32_t playerBits)
{
    auto const p = g_player[playerNum].ps;
    if (lotag != 0 || !voxworld_active() || p->cursectnum != voxworld_sector())
        return lotag;

    // Ticks left in which jump can still climb a bank after leaving the water.
    static int climbTics[MAXPLAYERS];
    vec3_t const body = { p->pos.x, p->pos.y, p->pos.z + EMINE_SWIMPOINT };
    bool const swimming = Emine_InWaterAt(body) || Emine_InWaterAt(p->pos);

    if (swimming)
    {
        p->falling_counter = 0;  // water breaks a fall
        if (!Emine_InWaterAt(p->pos))
            p->airleft = 15 * GAMETICSPERSEC;  // head above water: breathe
        climbTics[playerNum] = GAMETICSPERSEC + GAMETICSPERSEC / 2;
    }
    else if (climbTics[playerNum] > 0)
    {
        climbTics[playerNum]--;
        if (TEST_SYNC_KEY(playerBits, SK_JUMP))
        {
            // Swimming up and out. Facing a bank: climb (like a ladder) until the feet are over its top, so walking
            // forward steps onto it. Open water: just bob at the surface.
            int const ang = fix16_to_int(p->q16ang);
            auto solid = [](int t) { return t != VW_AIR && !(voxworld_blockflags(t) & VW_BF_NONSOLID); };
            bool bank = false;
            for (int r = 256; r <= 512 && !bank; r += 256)
            {
                int32_t const ax = p->pos.x + mulscale14(sintable[(ang + 512) & 2047], r);
                int32_t const ay = p->pos.y + mulscale14(sintable[ang & 2047], r);
                bank = solid(voxworld_blockat(ax, ay, p->pos.z + PHEIGHT - 512)) ||
                       solid(voxworld_blockat(ax, ay, p->pos.z + EMINE_SWIMPOINT));
            }
            if (bank)
            {
                p->vel.z = -1280;  // gravity is added after this, so a steady climb of about 1100 a tick
                p->jumping_counter = 0;
                p->jumping_toggle  = 1;
                p->on_ground       = 0;
            }
            else if (p->vel.z < -384)
                p->vel.z = -384;
        }
    }
    return swimming ? ST_2_UNDERWATER : lotag;
}

void Emine_SwimSink(int playerNum, uint32_t playerBits, int32_t floorZ)
{
    auto const p = g_player[playerNum].ps;
    if (sector[p->cursectnum].lotag == ST_2_UNDERWATER) return;  // a real Duke underwater sector
    if (TEST_SYNC_KEY(playerBits, SK_JUMP) || TEST_SYNC_KEY(playerBits, SK_CROUCH)) return;
    int32_t const stand = floorZ - PHEIGHT;
    if (p->pos.z < stand)
        p->pos.z = min(p->pos.z + 160, stand);
}

void Emine_Tick(void)
{
    emine_tic++;
    Emine_LavaBurn();

    auto const p = g_player[myconnectindex].ps;
    if (p->i >= 0) voxworld_stream(p->pos.x, p->pos.y);
    voxworld_tick();

    if (emine_where && p->i >= 0 && emine_tic % 15 == 0)
    {
        auto at = [&](int32_t dz) { return voxworld_blockat(p->pos.x, p->pos.y, p->pos.z + dz); };
        OSD_Printf("emine_where: z %d vel %d  eye %d body %d feet %d  pal %d air %d\n", p->pos.z, p->vel.z,
                   at(0), at(EMINE_SWIMPOINT), at(PHEIGHT - 256), p->palette, p->airleft);
    }
}

// Dev console: `emine_drop [height] [type]` drops a block (sand by default) onto the player from `height` blocks up.
static int osdcmd_emine_drop(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (!voxworld_active() || p->i < 0) return OSDCMD_OK;
    int const height = parm->numparms >= 1 ? clamp(Batol(parm->parms[0]), 2, 60) : 6;
    int const type   = parm->numparms >= 2 ? clamp(Batol(parm->parms[1]), 1, VW_NUMTYPES-1) : VW_SAND;
    int bx, by, bz;
    voxworld_toblock(sprite[p->i].x, sprite[p->i].y, sprite[p->i].z - 1, &bx, &by, &bz);
    if (voxworld_get(bx, by, bz + height) == VW_AIR)
        voxworld_set(bx, by, bz + height, type);
    return OSDCMD_OK;
}

// Dev console: `emine_water` drops the player into the nearest open water; `emine_water shore` stands them on the
// nearest bank, facing the water.
static int osdcmd_emine_water(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    vec3_t surf;
    int ang;
    bool const shore = parm->numparms >= 1;
    if (!voxworld_active() || p->i < 0 || !voxworld_findwater(p->pos.x, p->pos.y, shore, &surf, &ang))
    {
        OSD_Printf("emine_water: no water found\n");
        return OSDCMD_OK;
    }
    vec3_t const feet = { surf.x, surf.y, surf.z - (shore ? 0 : 1024) };
    if (shore) p->q16ang = fix16_from_int(ang);
    p->pos   = { feet.x, feet.y, feet.z - PHEIGHT };
    p->opos  = p->pos;
    p->bobpos = { feet.x, feet.y };
    p->vel   = { 0, 0, 0 };
    setsprite(p->i, &feet);
    OSD_Printf("emine_water: moved to %d, %d, %d\n", feet.x, feet.y, feet.z);
    return OSDCMD_OK;
}

// Dev console: `emine_go <dx> <dy>` moves the player that many blocks across the world, onto the ground there.
static int osdcmd_emine_go(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (!voxworld_active() || p->i < 0 || parm->numparms < 2) return OSDCMD_SHOWHELP;
    int32_t const x = p->pos.x + Batol(parm->parms[0]) * VW_BLOCKXY, y = p->pos.y + Batol(parm->parms[1]) * VW_BLOCKXY;
    voxworld_stream(x, y);  // bring the world there in
    int bx, by, bz;
    if (!voxworld_toblock(x, y, sector[voxworld_sector()].floorz - 1, &bx, &by, &bz))
    {
        OSD_Printf("emine_go: that's outside the world\n");
        return OSDCMD_OK;
    }
    vec3_t const feet = { x, y, voxworld_floorz(x, y, sector[voxworld_sector()].ceilingz + 1024, sector[voxworld_sector()].floorz) };
    p->pos   = { feet.x, feet.y, feet.z - PHEIGHT };
    p->opos  = p->pos;
    p->bobpos = { feet.x, feet.y };
    p->vel   = { 0, 0, 0 };
    setsprite(p->i, &feet);
    OSD_Printf("emine_go: moved to %d, %d, %d\n", feet.x, feet.y, feet.z);
    return OSDCMD_OK;
}

// Dev console: `emine_where` toggles printing what the player is standing/swimming in.
static int osdcmd_emine_where(osdcmdptr_t UNUSED(parm))
{
    emine_where = !emine_where;
    return OSDCMD_OK;
}

void Emine_RegisterOSD(void)
{
    OSD_RegisterFunction("emine_go", "emine_go <dx> <dy>: dev - move that many blocks across the world", osdcmd_emine_go);
    OSD_RegisterFunction("emine_where", "emine_where: dev - toggle printing the blocks at the player's eye/body/feet", osdcmd_emine_where);
    OSD_RegisterFunction("emine_water", "emine_water [shore]: dev - jump into the nearest open water (or stand beside it)", osdcmd_emine_water);
    voxworld_seteventfn(Emine_OnWorldEvent);
    OSD_RegisterFunction("emine_glow", "emine_glow [minlight 1-15]: dev - teleport next to the nearest glowing block", osdcmd_emine_cave);
    OSD_RegisterFunction("emine_drop", "emine_drop [height] [blocktype]: dev - drop a block (sand) on yourself", osdcmd_emine_drop);
    OSD_RegisterFunction("emine_cave", "emine_cave [maxlight 0-15]: dev - teleport to the nearest dark cave", osdcmd_emine_cave);
}

int Emine_LightShade(int spriteNum)
{
    if (!voxworld_active() || sprite[spriteNum].sectnum != voxworld_sector())
        return 0;
    auto const &s = sprite[spriteNum];
    return voxworld_lightshade(s.x, s.y, s.z - VW_BLOCKZ/2);
}

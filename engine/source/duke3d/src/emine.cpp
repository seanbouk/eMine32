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

void Emine_Tick(void)
{
    emine_tic++;
    Emine_LavaBurn();
    voxworld_tick();
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

void Emine_RegisterOSD(void)
{
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

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

    if (type == VW_GLASS)
    {
        // glass smashes like Duke's windows: shards and that sound
        int const smoke = A_Spawn(ownerSprite, SMALLSMOKE);
        setsprite(smoke, &c);
        sprite[smoke].xrepeat = sprite[smoke].yrepeat = 0;  // just the source of the shards
        A_SpawnGlass(smoke, numChunks + 4);
        if (withSound) A_PlaySound(GLASS_BREAKING, smoke);
        return;
    }

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

// Ores drop ammo, one kind each: the commoner, shallower ores the cheaper ammo. Each ore block drops this many
// pickups, each worth what Duke's pickup gives (tune here). 0: not an ore.
static int Emine_OreAmmo(int type, int *count)
{
    *count = 1;
    switch (type)
    {
        case VW_COAL:     *count = 4; return AMMO;         // pistol clips (breaking coal takes most of one)
        case VW_IRON:     *count = 3; return SHOTGUNAMMO;  // shotgun shells
        case VW_GOLD:     *count = 2; return BATTERYAMMO;  // chaingun ammo
        case VW_REDSTONE: return HBOMBAMMO;    // pipe bombs
        case VW_LAPIS:    return FREEZEAMMO;   // freeze thrower
        case VW_DIAMOND:  return RPGAMMO;      // rockets
        default:          *count = 0; return 0;
    }
}

// Dropped pickups pop up out of the broken block, fall and bounce to rest: straight up and down only.
#define EMINE_MAXDROPS   64
#define EMINE_DROPGRAV   96    // z units per tic per tic
#define EMINE_DROPBOUNCE 40    // percent of the landing speed kept for a bounce
#define EMINE_DROPBOUNCES 2
static struct { int16_t sprite, picnum; int32_t z, zvel; uint8_t bounces; } emine_drops[EMINE_MAXDROPS];
static int emine_ndrops;

static void Emine_MoveDrops(void)
{
    for (int i = 0; i < emine_ndrops; i++)
    {
        auto &d = emine_drops[i];
        auto &s = sprite[d.sprite];
        bool done = s.statnum == MAXSTATUS || s.picnum != d.picnum;  // picked up / gone
        if (!done)
        {
            d.zvel += EMINE_DROPGRAV;
            int32_t const floorz = voxworld_floorz(s.x, s.y, d.z - 16, sector[s.sectnum].floorz);
            d.z += d.zvel;
            if (d.z >= floorz)
            {
                d.z = floorz;
                if (d.bounces++ >= EMINE_DROPBOUNCES) done = true;
                else d.zvel = -d.zvel * EMINE_DROPBOUNCE / 100;
            }
            s.zvel = 0;  // ours to move, not Duke's
            vec3_t const at = { s.x, s.y, d.z };
            setsprite(d.sprite, &at);
        }
        if (done) emine_drops[i--] = emine_drops[--emine_ndrops];
    }
}

void Emine_Bounce(int spriteNum, int32_t z)
{
    if (emine_ndrops < EMINE_MAXDROPS)
        emine_drops[emine_ndrops++] = { (int16_t)spriteNum, sprite[spriteNum].picnum, z, -500 - (krand() & 255), 0 };
}

static bool Emine_OreDrop(int bx, int by, int bz, int type, int ownerSprite)
{
    int count;
    int const tile = Emine_OreAmmo(type, &count);
    if (!count) return false;
    Emine_VoiceOre(type);
    vec3_t c;
    voxworld_blockcenter(bx, by, bz, &c);
    int const sect = voxworld_sector();
    for (int i = 0; i < count; i++)
    {
        // out of the middle of the block, with a little pop upwards; Emine_MoveDrops does the rest
        int32_t const x = c.x + (count > 1 ? (krand() & 127) - 64 : 0), y = c.y + (count > 1 ? (krand() & 127) - 64 : 0);
        int const j = A_Spawn(ownerSprite, tile);
        vec3_t const at = { x, y, min(c.z + VW_BLOCKZ/4, voxworld_floorz(x, y, c.z, sector[sect].floorz)) };
        setsprite(j, &at);
        if (emine_ndrops < EMINE_MAXDROPS)
            emine_drops[emine_ndrops++] = { (int16_t)j, sprite[j].picnum, at.z, -500 - (krand() & 255), 0 };  // pop up about a quarter of a block
    }
    return true;
}

void Emine_HitscanBlock(int shotSprite, const hitdata_t *hit)
{
    int bx, by, bz;
    if (!voxworld_active() || !voxworld_lasthit(&hit->xyz, &bx, &by, &bz))
        return;

    int const damage = sprite[shotSprite].extra > 0 ? sprite[shotSprite].extra : 6;
    int const owner0 = sprite[shotSprite].owner;
    if (voxworld_get(bx, by, bz) == VW_BEDROCK && owner0 >= 0 && sprite[owner0].picnum == APLAYER)
        Emine_Say("bedrock", 6);
    int const type = voxworld_damage(bx, by, bz, damage);
    if (type != VW_AIR)
    {
        int const owner = sprite[shotSprite].owner >= 0 ? sprite[shotSprite].owner : shotSprite;
        Emine_BlockDestroyedFX(bx, by, bz, type, owner, 5, true);
        Emine_OreDrop(bx, by, bz, type, owner);
        Emine_FoodBlockBroken(bx, by, bz, type, owner);
    }
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

struct emine_blastctx { int owner, count, drops, food; };

static void Emine_OnBlastDestroyed(int bx, int by, int bz, int type, void *ud)
{
    auto &ctx = *(emine_blastctx *)ud;
    // An explosion can take out dozens of blocks: keep the debris (sprites) and sounds bounded.
    if (ctx.count < 24)
        Emine_BlockDestroyedFX(bx, by, bz, type, ctx.owner, (ctx.count & 1) ? 1 : 2, ctx.count == 0);
    ctx.count++;
    // ores blown out drop their ammo too, up to a few per blast (a rocket into a coal seam shouldn't carpet the floor)
    if (ctx.drops < 6 && Emine_OreDrop(bx, by, bz, type, ctx.owner))
        ctx.drops++;
    if (ctx.food < 8 && (type == VW_LEAVES || type == VW_MELON))
    {
        Emine_FoodBlockBroken(bx, by, bz, type, ctx.owner);
        ctx.food++;
    }
}

void Emine_ExplosionBlocks(int spriteNum, int blastRadius, int maxDamage)
{
    if (!voxworld_active() || sprite[spriteNum].sectnum != voxworld_sector())
        return;

    // Blocks get the explosion's full damage at the centre falling to zero at the edge, scaled up so a
    // rocket digs a proper crater (RPG: ~140 damage, 1780 radius -> ~2 blocks of stone).
    emine_blastctx ctx = { spriteNum, 0, 0, 0 };
    voxworld_explode(&sprite[spriteNum].xyz, blastRadius, maxDamage * 2, Emine_OnBlastDestroyed, &ctx);
}

int32_t Emine_FloorZ(int spriteNum, int32_t sectorFloorZ)
{
    if (!voxworld_active() || sprite[spriteNum].sectnum != voxworld_sector())
        return sectorFloorZ;
    auto const &s = sprite[spriteNum];
    return voxworld_floorz(s.x, s.y, s.z, sectorFloorZ);
}

int32_t Emine_ProjectileBlocks(int spriteNum, int32_t dx, int32_t dy, int32_t dz)
{
    auto &s = sprite[spriteNum];
    if (!voxworld_active() || s.sectnum != voxworld_sector() || !(dx|dy|dz))
        return 0;
    // Already inside a block (e.g. fired by someone buried by falling gravel): the trace skips the block it starts
    // in, so hit it here and now.
    int bx, by, bz;
    if (voxworld_toblock(s.x, s.y, s.z, &bx, &by, &bz))
    {
        int const type = voxworld_get(bx, by, bz);
        if (type != VW_AIR && !(voxworld_blockflags(type) & VW_BF_CLEAR))
        {
            voxworld_notehit(&s.xyz, bx, by, bz);
            return 32768 | sector[voxworld_sector()].wallptr;
        }
    }

    vwhit_t vh;
    if (!voxworld_raycast(&s.xyz, dx, dy, dz, 1.f, &vh))
        return 0;
    // stop just short of the face, like a hitscan hit, and report it as a wall hit (so it explodes etc.)
    float const len = sqrtf((float)dx*dx + (float)dy*dy + ((float)dz*dz)*(1.f/256.f));
    float const t = max(0.f, vh.dist - 24.f / len);
    vec3_t const at = { s.x + (int32_t)(dx * t), s.y + (int32_t)(dy * t), s.z + (int32_t)(dz * t) };
    setsprite(spriteNum, &at);
    voxworld_notehit(&at, vh.bx, vh.by, vh.bz);
    return 32768 | sector[voxworld_sector()].wallptr;
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
            if (s.picnum == APLAYER) Emine_Say("gravel", 3);
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
    double t = timerGetFractionalTicks(), t1;
    auto lap = [&](int sys) { t1 = timerGetFractionalTicks(); Emine_PerfSystem(sys, t1 - t); t = t1; };
    Emine_MobTick();
    Emine_LavaBurn();
    lap(EMINE_PERF_MOBS);

    auto const p = g_player[myconnectindex].ps;
    if (p->i >= 0) voxworld_stream(p->pos.x, p->pos.y);
    voxworld_tick();
    Emine_MoveDrops();
    lap(EMINE_PERF_WORLD);
    Emine_FoodTick();
    lap(EMINE_PERF_FOOD);
    Emine_VoiceTick();
    lap(EMINE_PERF_VOICE);

    if (emine_where && p->i >= 0 && emine_tic % 15 == 0)
    {
        auto at = [&](int32_t dz) { return voxworld_blockat(p->pos.x, p->pos.y, p->pos.z + dz); };
        OSD_Printf("emine_where: x %d y %d z %d vel %d  eye %d body %d feet %d  pal %d air %d\n", p->pos.x, p->pos.y, p->pos.z,
                   p->vel.z, at(0), at(EMINE_SWIMPOINT), at(PHEIGHT - 256), p->palette, p->airleft);
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

// Duke has every weapon, all the time; what he can use depends on the ammo he's dug up. He starts with a clip.
void Emine_ResetWeapons(int playerNum)
{
    auto &p = *g_player[playerNum].ps;
    for (int w = 0; w < MAX_WEAPONS; w++)
        p.gotweapon |= 1 << w;
    p.ammo_amount[PISTOL_WEAPON] = min<int16_t>(p.max_ammo_amount[PISTOL_WEAPON], 12);
}

// Dev console: `emine_ore <blocktype>` drops what breaking that ore would, a couple of blocks in front of the player.
static int osdcmd_emine_ore(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (!voxworld_active() || p->i < 0 || parm->numparms < 1) return OSDCMD_SHOWHELP;
    int const ang = fix16_to_int(p->q16ang);
    int bx, by, bz;
    voxworld_toblock(p->pos.x + (sintable[(ang + 512) & 2047] >> 4), p->pos.y + (sintable[ang & 2047] >> 4), p->pos.z, &bx, &by, &bz);
    if (!Emine_OreDrop(bx, by, bz, Batol(parm->parms[0]), p->i)) OSD_Printf("emine_ore: not an ore\n");
    return OSDCMD_OK;
}

// Dev console: `emine_go <dx> <dy> [angle]` moves the player that many blocks across the world, onto the ground there.
// Dev console: `emine_spawn <tile> [blocks]` puts that sprite/actor on the ground in front of the player, facing them.
static int osdcmd_emine_spawn(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (!voxworld_active() || p->i < 0 || parm->numparms < 1) return OSDCMD_SHOWHELP;
    int const pic = Batol(parm->parms[0]);
    int const dist = (parm->numparms >= 2 ? Batol(parm->parms[1]) : 4) * VW_BLOCKXY;
    if ((unsigned)pic >= MAXTILES) return OSDCMD_SHOWHELP;
    int const ang = fix16_to_int(p->q16ang) & 2047;
    int32_t const dx = mulscale14(sintable[(ang + 512) & 2047], dist), dy = mulscale14(sintable[ang], dist);
    // Level from the eye: stop a block short of anything in the way (a hill, a tree, a cave wall), then drop to the
    // ground below that point. (Starting from the eye keeps it out of the rock above a cave or under an overhang.)
    float t = 1.f;
    vwhit_t vh;
    if (voxworld_raycast(&p->pos, dx, dy, 0, 1.f, &vh))
        t = max(0.f, vh.dist - (float)VW_BLOCKXY / dist);
    int32_t const x = p->pos.x + (int32_t)(dx * t), y = p->pos.y + (int32_t)(dy * t);
    int32_t const z = voxworld_floorz(x, y, p->pos.z, sector[voxworld_sector()].floorz);
    int const j = A_Spawn(p->i, pic);
    vec3_t const at = { x, y, z };
    setsprite(j, &at);
    sprite[j].ang = (ang + 1024) & 2047;
    OSD_Printf("emine_spawn: tile %d at %d, %d, %d\n", pic, x, y, z);
    return OSDCMD_OK;
}

// Dev console: `emine_list [blocks]` prints the enemies within that many blocks (default 32).
static int osdcmd_emine_list(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (p->i < 0) return OSDCMD_OK;
    int32_t const range = (parm->numparms >= 1 ? Batol(parm->parms[0]) : 32) * VW_BLOCKXY;
    int n = 0;
    for (int i = 0; i < MAXSPRITES; i++)
    {
        auto const &s = sprite[i];
        if (s.statnum == MAXSTATUS || !A_CheckEnemySprite(&s)) continue;
        int32_t const d = FindDistance2D(s.x - p->pos.x, s.y - p->pos.y);
        if (d > range) continue;
        OSD_Printf("emine_list: #%d tile %d stat %d at %d,%d,%d (%d blocks, %d up) size %d hp %d\n", i, s.picnum, s.statnum,
                   s.x, s.y, s.z, d / VW_BLOCKXY, (p->pos.z - s.z) / VW_BLOCKZ, s.xrepeat, s.extra);
        n++;
    }
    OSD_Printf("emine_list: %d enemies\n", n);
    return OSDCMD_OK;
}

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
    if (parm->numparms >= 3) p->q16ang = fix16_from_int(Batol(parm->parms[2]) & 2047);
    setsprite(p->i, &feet);
    OSD_Printf("emine_go: moved to %d, %d, %d\n", feet.x, feet.y, feet.z);
    return OSDCMD_OK;
}

// Dev console: `emine_find <blocktype>` stands the player on the nearest column topped with that block type.
static int osdcmd_emine_find(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (!voxworld_active() || p->i < 0 || parm->numparms < 1) return OSDCMD_SHOWHELP;
    vec3_t feet;
    if (!voxworld_findtop(p->pos.x, p->pos.y, Batol(parm->parms[0]), &feet))
    {
        OSD_Printf("emine_find: none nearby\n");
        return OSDCMD_OK;
    }
    p->pos   = { feet.x, feet.y, feet.z - PHEIGHT };
    p->opos  = p->pos;
    p->bobpos = { feet.x, feet.y };
    p->vel   = { 0, 0, 0 };
    setsprite(p->i, &feet);
    OSD_Printf("emine_find: moved to %d, %d, %d\n", feet.x, feet.y, feet.z);
    return OSDCMD_OK;
}

// Dev console: `emine_look <horiz>` sets where the player looks up/down (Build's horiz: 100 is level, lower looks
// down: the horizon sits at about horiz/200 of the way down the screen). For screenshots.
int emine_viewtrace;  // frames left to trace (game.cpp G_DrawRooms prints them)
static int osdcmd_emine_viewtrace(osdcmdptr_t parm)
{
    emine_viewtrace = parm->numparms >= 1 ? Batol(parm->parms[0]) : 120;
    return OSDCMD_OK;
}

static int osdcmd_emine_look(osdcmdptr_t parm)
{
    auto const p = g_player[myconnectindex].ps;
    if (parm->numparms < 1 || p->i < 0) return OSDCMD_SHOWHELP;
    p->q16horiz = fix16_from_int(Batol(parm->parms[0]));
    p->q16horizoff = 0;
    p->oq16horiz = p->q16horiz;
    OSD_Printf("emine_look: horiz %d\n", fix16_to_int(p->q16horiz));
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
    OSD_RegisterFunction("emine_viewtrace", "emine_viewtrace [frames]: dev - print the clock, tic fraction and view angle of each frame drawn", osdcmd_emine_viewtrace);
    OSD_RegisterFunction("emine_look", "emine_look <horiz>: dev - set the view pitch (100 level, lower looks down)", osdcmd_emine_look);
    OSD_RegisterFunction("emine_find", "emine_find <blocktype>: dev - stand on the nearest column topped with that block (31 = snow)", osdcmd_emine_find);
    OSD_RegisterFunction("emine_ore", "emine_ore <blocktype>: dev - drop what breaking that ore gives (11 coal, 12 iron, 13 gold, 14 redstone, 33 lapis, 34 diamond)", osdcmd_emine_ore);
    OSD_RegisterFunction("emine_spawn", "emine_spawn <tile> [blocks]: dev - spawn that sprite/actor on the ground in front of you (default 4 blocks)", osdcmd_emine_spawn);
    OSD_RegisterFunction("emine_list", "emine_list [blocks]: dev - list the enemies near you (default 32 blocks)", osdcmd_emine_list);
    OSD_RegisterFunction("emine_go", "emine_go <dx> <dy> [angle]: dev - move that many blocks across the world (and face Build angle 0-2047)", osdcmd_emine_go);
    OSD_RegisterFunction("emine_where", "emine_where: dev - toggle printing the blocks at the player's eye/body/feet", osdcmd_emine_where);
    OSD_RegisterFunction("emine_water", "emine_water [shore]: dev - jump into the nearest open water (or stand beside it)", osdcmd_emine_water);
    voxworld_seteventfn(Emine_OnWorldEvent);
    Emine_RegisterMobOSD();
    Emine_RegisterFoodOSD();
    Emine_RegisterVoiceOSD();
    Emine_RegisterPerfOSD();
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

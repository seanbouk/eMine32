// eMine32: food. Duke has no hunger bar, so food heals, Duke-pickup style: walk over it and it's eaten, if you're
// hurt (at full health it stays where it is).
//
//  - zombies drop meat when they die: raw when shot, cooked when an explosion kills them (pipe bomb, rocket, a
//    creeper going off) - they burn as they fall (emine_mobs.cpp).
//  - melons break into slices, and grow back a couple of minutes later (out of sight, not while you're close).
//  - leaves now and then drop an apple. Trees don't grow back, so apples run out.
//  - mushrooms grow on cave floors in the dark and give off a little blue light. They're plants: walk into one to
//    eat it, shots go past them. New ones keep appearing in dark spots around you.
//
// Pickups are plain sprites (tiles 7150+, made by tools/mksprite.py) that pop out and bounce like the ore drops;
// they rot away after a few minutes.
//
// Copyright (C) 2026 the eMine32 project. GPLv2.

#include "duke3d.h"
#include "voxworld.h"
#include "emine.h"

#include <vector>

namespace
{

struct FoodDef { const char *name; int tile, heal; const char *quote; };

// heal: health points (Duke's 100). Minecraft's hunger points, doubled and then some: a melon slice is a snack, a
// cooked steak is a medkit.
FoodDef const foodDefs[EMINE_FOOD_COUNT] = {
    { "melon",    7150,  5, "Melon. Refreshing!" },
    { "apple",    7151, 10, "An apple a day..." },
    { "meat",     7152, 10, "Raw zombie. Tastes like chicken." },
    { "cooked",   7153, 30, "Zombie steak. Well done." },
    { "mushroom",    0,  8, "Glowing mushroom. Groovy." },
};

int constexpr FOOD_REPEAT  = 20;            // sprite size: about 0.4 block across
int constexpr FOOD_ROT     = 180 * 30;      // tics a dropped item lasts
int constexpr FOOD_REACH   = 420;           // xy distance at which you pick it up
int constexpr MELON_REGROW = 120 * 30;      // tics before a melon grows back (plus up to half as much again)
int constexpr MELON_NEAR   = 10 * VW_BLOCKXY;  // ...and not while you're this close
int constexpr APPLE_CHANCE = 10;            // 1 in this many leaf blocks drops an apple
int constexpr SHROOM_EVERY = 4 * 30;        // tics between tries at growing a new mushroom near you
int constexpr SHROOM_SPACE = 4;             // no two closer than this (blocks)

struct Item { int sprite, kind, picnum, born; };
std::vector<Item> items;

struct Regrow { vec3_t at; int due; };      // a melon to put back: block centre (Build coords stay put as the
std::vector<Regrow> regrow;                 // streamed window moves) and when

int food_tic;

bool solidAt(int bx, int by, int bz)
{
    int const t = voxworld_get(bx, by, bz);
    return t != VW_AIR && !(voxworld_blockflags(t) & VW_BF_NONSOLID);
}

bool eat(DukePlayer_t *ps, int kind)
{
    auto &health = sprite[ps->i].extra;
    if (health <= 0 || health >= ps->max_player_health) return false;
    int const before = health;
    health = min(ps->max_player_health, health + foodDefs[kind].heal);
    Emine_VoiceAte(kind, before, health, ps->max_player_health);
    Bstrcpy(apStrings[QUOTE_RESERVED4], foodDefs[kind].quote);
    P_DoQuote(QUOTE_RESERVED4, ps);
    S_PlaySound(DUKE_GET);        // the sound and green flash of Duke's health pickups (GAME.CON's quikget)
    P_PalFrom(ps, 16, 0, 32, 0);
    return true;
}

// Mushrooms you're standing in.
void eatMushrooms(DukePlayer_t *ps)
{
    auto const &s = sprite[ps->i];
    for (int i = 0; i < 4; i++)
    {
        int32_t const x = s.x + ((i & 1) ? 160 : -160), y = s.y + ((i & 2) ? 160 : -160);
        int bx, by, bz;
        if (!voxworld_toblock(x, y, s.z - 256, &bx, &by, &bz) || voxworld_get(bx, by, bz) != VW_MUSHROOM) continue;
        if (!eat(ps, EMINE_FOOD_MUSHROOM)) return;
        voxworld_set(bx, by, bz, VW_AIR);
        return;
    }
}

void pickUp(DukePlayer_t *ps)
{
    auto const &ps_s = sprite[ps->i];
    for (size_t i = 0; i < items.size(); i++)
    {
        auto const &it = items[i];
        auto const &s = sprite[it.sprite];
        bool gone = s.statnum == MAXSTATUS || s.picnum != it.picnum;
        if (!gone && food_tic - it.born > FOOD_ROT)
        {
            A_DeleteSprite(it.sprite);
            gone = true;
        }
        if (!gone && FindDistance2D(s.x - ps_s.x, s.y - ps_s.y) < FOOD_REACH && klabs(s.z - ps_s.z) < VW_BLOCKZ * 3 / 2 &&
            eat(ps, it.kind))
        {
            A_DeleteSprite(it.sprite);
            gone = true;
        }
        if (gone) { items[i--] = items.back(); items.pop_back(); }
    }
}

void growMelons(DukePlayer_t *ps)
{
    for (size_t i = 0; i < regrow.size(); i++)
    {
        auto const &r = regrow[i];
        if (food_tic < r.due) continue;
        int bx, by, bz;
        if (!voxworld_toblock(r.at.x, r.at.y, r.at.z, &bx, &by, &bz)) continue;  // not streamed in: wait
        if (FindDistance2D(r.at.x - ps->pos.x, r.at.y - ps->pos.y) < MELON_NEAR) continue;
        int const t = voxworld_get(bx, by, bz);
        if ((t == VW_AIR || (voxworld_blockflags(t) & VW_BF_PLANT)) && solidAt(bx, by, bz - 1))
            voxworld_set(bx, by, bz, VW_MELON);
        regrow[i--] = regrow.back(); regrow.pop_back();
    }
}

// Now and then, a new mushroom somewhere dark 10-28 blocks away, unless there's one close by already.
void growMushroom(DukePlayer_t *ps)
{
    int const a = krand() & 2047, d = (10 + krand() % 19) * VW_BLOCKXY;
    int32_t const x = ps->pos.x + mulscale14(sintable[(a + 512) & 2047], d), y = ps->pos.y + mulscale14(sintable[a], d);
    int bx, by, pz, sx, sy, sz;
    voxworld_size(&sx, &sy, &sz);
    if (!voxworld_toblock(x, y, ps->pos.z, &bx, &by, &pz)) return;
    // a dark floor in this column near the player's height
    int best = -1;
    for (int z = max(1, pz - 12); z < min(sz - 1, pz + 12); z++)
        if (voxworld_get(bx, by, z) == VW_AIR && solidAt(bx, by, z - 1) && voxworld_get(bx, by, z - 1) != VW_LEAVES &&
            voxworld_lightlevel(bx, by, z) <= 1 && (best < 0 || abs(z - pz) < abs(best - pz)))
            best = z;
    if (best < 0) return;
    for (int dx = -SHROOM_SPACE; dx <= SHROOM_SPACE; dx++)
        for (int dy = -SHROOM_SPACE; dy <= SHROOM_SPACE; dy++)
            for (int dz = -2; dz <= 2; dz++)
                if (voxworld_get(bx + dx, by + dy, best + dz) == VW_MUSHROOM) return;
    voxworld_set(bx, by, best, VW_MUSHROOM);
}

}  // namespace

void Emine_FoodDrop(int kind, vec3_t const &at, int owner)
{
    if (!voxworld_active() || (unsigned)kind >= EMINE_FOOD_MUSHROOM) return;
    int32_t const x = at.x + (krand() & 127) - 64, y = at.y + (krand() & 127) - 64;
    int const j = A_InsertSprite(voxworld_sector(), x, y, at.z, foodDefs[kind].tile, 0, FOOD_REPEAT, FOOD_REPEAT,
                                 krand() & 2047, 0, 0, owner >= 0 ? owner : 0, STAT_DEFAULT);
    Emine_Bounce(j, at.z);
    items.push_back({ j, kind, sprite[j].picnum, food_tic });
}

void Emine_FoodBlockBroken(int bx, int by, int bz, int type, int owner)
{
    vec3_t c;
    voxworld_blockcenter(bx, by, bz, &c);
    if (type == VW_LEAVES && krand() % APPLE_CHANCE == 0)
    {
        Emine_FoodDrop(EMINE_FOOD_APPLE, c, owner);
        Emine_Say("apple", 3);
    }
    else if (type == VW_MELON)
    {
        for (int i = 0, n = 3 + krand() % 3; i < n; i++) Emine_FoodDrop(EMINE_FOOD_MELON, c, owner);
        regrow.push_back({ c, food_tic + MELON_REGROW + (int)(krand() % (MELON_REGROW / 2)) });
    }
}

void Emine_FoodTick(void)
{
    food_tic++;
    auto const ps = g_player[myconnectindex].ps;
    if (!voxworld_active() || ps->i < 0) return;
    pickUp(ps);
    eatMushrooms(ps);
    if (food_tic % 30 == 0) growMelons(ps);
    if (food_tic % SHROOM_EVERY == 0) growMushroom(ps);
}

void Emine_FoodLevelStart(void)
{
    items.clear();
    regrow.clear();
}

// Dev console: `emine_food <kind>` drops that food in front of you (melon, apple, meat, cooked); `emine_food hurt`
// takes 50 health so you can eat it; `emine_food grow` brings back every eaten melon now; `emine_food shroom` tries
// to grow a mushroom 20 times.
static int osdcmd_emine_food(osdcmdptr_t parm)
{
    auto const ps = g_player[myconnectindex].ps;
    if (parm->numparms < 1 || ps->i < 0) return OSDCMD_SHOWHELP;
    char const *const a = parm->parms[0];
    if (!Bstrcasecmp(a, "hurt")) { sprite[ps->i].extra = max(1, sprite[ps->i].extra - 50); return OSDCMD_OK; }
    if (!Bstrcasecmp(a, "grow")) { for (auto &r : regrow) r.due = 0; growMelons(ps); OSD_Printf("emine_food: %d melons waiting\n", (int)regrow.size()); return OSDCMD_OK; }
    if (!Bstrcasecmp(a, "shroom")) { for (int i = 0; i < 20; i++) growMushroom(ps); return OSDCMD_OK; }
    if (!Bstrcasecmp(a, "smash"))  // break the nearest melon within 6 blocks, as a shot would
    {
        int px, py, pz;
        if (!voxworld_toblock(ps->pos.x, ps->pos.y, sprite[ps->i].z - 256, &px, &py, &pz)) return OSDCMD_OK;
        for (int r = 0; r <= 6; r++)
            for (int dx = -r; dx <= r; dx++)
                for (int dy = -r; dy <= r; dy++)
                    for (int dz = -2; dz <= 2; dz++)
                        if (max(abs(dx), abs(dy)) == r && voxworld_get(px + dx, py + dy, pz + dz) == VW_MELON)
                        {
                            voxworld_set(px + dx, py + dy, pz + dz, VW_AIR);
                            Emine_FoodBlockBroken(px + dx, py + dy, pz + dz, VW_MELON, ps->i);
                            OSD_Printf("emine_food: smashed the melon at %d %d %d; %d items out\n", dx, dy, dz, (int)items.size());
                            return OSDCMD_OK;
                        }
        return OSDCMD_OK;
    }
    for (int k = 0; k < EMINE_FOOD_MUSHROOM; k++)
        if (!Bstrcasecmp(a, foodDefs[k].name))
        {
            int16_t const ang = fix16_to_int(ps->q16ang);
            vec3_t const at = { ps->pos.x + (sintable[(ang + 512) & 2047] >> 4), ps->pos.y + (sintable[ang & 2047] >> 4),
                                ps->pos.z };
            Emine_FoodDrop(k, at, ps->i);
            return OSDCMD_OK;
        }
    return OSDCMD_SHOWHELP;
}

void Emine_RegisterFoodOSD(void)
{
    OSD_RegisterFunction("emine_food", "emine_food <melon|apple|meat|cooked|hurt|grow|shroom|smash>: food dev helpers", osdcmd_emine_food);
}

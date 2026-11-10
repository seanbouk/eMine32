// eMine32: Minecraft-style mobs, in C++ rather than Duke's CON scripts (they need to see the blocks: paths, steps,
// climbing, light). An actor handled here skips its CON code; A_Execute calls Emine_ActorThink first. Duke's CON
// *actions* still say which tiles each pose uses (APIGWALK etc.); everything else is here.
//
// Numbers follow Minecraft Java, converted to Build units: a block is 512 wide and 8192 tall, and there are 30 game
// tics a second against Minecraft's 20. Health is doubled into Duke's scale (a 20-health mob has 40) so Duke's guns
// take about as many shots as Minecraft's swords take hits.
//
// Shared by all mobs:
//  - sight: notices a player it can see within its follow range (checked twice a second); keeps after them while it
//    has seen them in the last 3 seconds (Minecraft's 60-tick memory), walking on to where it last saw them.
//  - paths: A* over the blocks (hop up one block, drop up to three, room to stand, no lava); spiders climb.
//  - when hurt: knocked back with a little hop, flinches, and turns on the player even if it can't see them.
//  - idle: strolls somewhere within 10 blocks every ~6 seconds.
//  - dies with its fall-over frames, lies there 10 seconds, then puffs away. Explosives gib it.
//
// Zombie   (PIGCOP)   follow 35 blocks, melee within 1.4 blocks once a second, alerts other zombies when hurt.
// Skeleton (LIZTROOP) follow 16 blocks; shoots arrows (they arc and stick in blocks) every 2 seconds from up to 15
//                     blocks; strafes, closes in when far and backs off when you're close.
// Spider   (LIZMAN)   follow 16 blocks, fast, climbs walls, leaps at you from 2-4 blocks; leaves you alone in bright
//                     light (light level 10+) unless you hurt it; hostile in the dark.
// Creeper  (DRONE)    follow 16 blocks, silent; within 3 blocks it stops, hisses and swells for 1.5 seconds, then
//                     explodes (blocks too). Walk away (past 7 blocks or out of sight) and the fuse winds back down.
//                     Shot dead, it just goes up in smoke.
//
// Spawning (Emine_MobTick), Minecraft's rules bent for a world that's always daylight:
//  - caves follow Minecraft: monsters appear in total darkness (light level 0), on solid ground with room to stand,
//    24-48 blocks from the player, in packs, up to a cap.
//  - the surface gets waves: a quiet spell (40-80 s), then two packs turn up 24-48 blocks away, somewhere you can't
//    see (the first 20 s in). Never on leaves, in water or in lava.
//  - despawning: gone at once beyond 128 blocks; past 32 blocks, one that's been idle for 30 seconds may vanish
//    (1 in 800 a tic), so far-off monsters get recycled into ones near you.
//  - Duke's own enemies placed in the map are cleared when the level starts.
//
// Copyright (C) 2026 the eMine32 project. GPLv2.

#include "duke3d.h"
#include "voxworld.h"
#include "emine.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <vector>

// Duke's sounds for now (soundefs.h numbers): the mobs get their own with the audio pass.
enum
{
    SND_PIG_ROAM = 120, SND_PIG_PAIN = 123, SND_PIG_DYING = 124, SND_PIG_ROAM2 = 205,
    SND_PRED_ROAM = 110, SND_PRED_ATTACK = 112, SND_PRED_PAIN = 113, SND_PRED_DYING = 114,
    SND_LIZ_BEG = 61, SND_LIZ_PAIN = 62, SND_LIZ_DEATH = 63,
    SND_STEAM_HISSING = 93, SND_SQUISHED = 69, SND_THUD = 158, SND_PIPEBOMB_EXPLODE = 85,
};

namespace
{

constexpr int BXY = VW_BLOCKXY, BZ = VW_BLOCKZ;

constexpr int MOB_TURN    = 160;   // max turn per tic (2048 = full circle)
constexpr int MOB_MEMORY  = 90;    // tics a lost target is remembered (3 s)
constexpr int MOB_STOP    = 400;   // melee mobs stop pushing forward this close
constexpr int MOB_JUMP    = 1780;  // zvel for a hop up one block (peaks ~1.1 blocks with Duke's gravity)
constexpr int MOB_KNOCK   = 137;   // knockback when hit, units/tic at the start (~0.8 blocks in all)
constexpr int MOB_HOP     = 1000;  // the little hop when hit
constexpr int MOB_WANDER  = 120;   // 1 in this many tics, an idle mob strolls off (~6 s on average)
constexpr int MOB_STROLL  = 10;    // ...to somewhere within this many blocks
constexpr int MOB_CORPSE  = 300;   // tics a body lies there before puffing away
constexpr int MOB_MAGIC   = 0x2B1E;  // in t_data[9]: this mob has its state set up
constexpr int MOB_CLIMB   = 1100;  // spider climbing speed, z units per tic (Minecraft: 0.2 blocks a tick)

constexpr int ARROW_TILE    = 7100;
constexpr int ARROW_SPEED   = 546;   // units/tic (Minecraft: 1.6 blocks a tick)
constexpr int ARROW_GRAVITY = 182;   // z units/tic^2 (Minecraft: 0.05 blocks a tick^2)

constexpr int CREEPER_FUSE   = 45;          // 1.5 s
constexpr int CREEPER_LIGHT  = 3 * BXY;     // starts swelling this close
constexpr int CREEPER_CANCEL = 7 * BXY;     // winds back down past this
constexpr int CREEPER_RADIUS = 3 * BXY;
constexpr int CREEPER_DAMAGE = 90;          // at the centre; Minecraft's creeper nearly kills you point blank

enum MobType { MT_ZOMBIE, MT_SKELETON, MT_SPIDER, MT_CREEPER, MT_COUNT, MT_NONE = -1 };
enum { MS_LIVE, MS_DYING, MS_DEAD };

struct MobDef
{
    char const *name;
    char const *aWalk, *aStand, *aHit, *aAttack, *aDying, *aDead;  // CON action labels
    int dyingFrames;
    int health, speed, sight;
    int reach, cooldown, dmg[4];  // melee (reach 0: none); dmg per Duke skill
    int headroom, maxUp;          // cells it needs to stand; how high a path may step up (climbers: a wall)
    int repeat;                   // sprite size (Duke spawns enemies at 40)
    bool alerts, bleeds, ranged, climbs, explodes;
    int sndRoam, sndRoam2, sndPain, sndDie;
};

MobDef const mobDefs[MT_COUNT] = {
    { "zombie", "APIGWALK", "APIGSTAND", "APIGHIT", "APIGSTAND", "APIGDYING", "APIGDEAD", 5,
      40, 40, 35 * BXY, 717, 30, { 12, 15, 18, 22 }, 2, 1, 40, true, true, false, false, false,
      SND_PIG_ROAM, SND_PIG_ROAM2, SND_PIG_PAIN, SND_PIG_DYING },
    { "skeleton", "ATROOPWALKING", "ATROOPSTAND", "ATROOPFLINTCH", "ATROOPSHOOT", "ATROOPDYING", "ATROOPDEAD", 5,
      40, 43, 16 * BXY, 0, 60, { 10, 14, 17, 20 }, 2, 1, 40, false, false, true, false, false,
      SND_PRED_ROAM, 0, SND_PRED_PAIN, SND_PRED_DYING },
    { "spider", "ALIZWALKING", "ALIZTHINK", "ALIZTHINK", "ALIZSCREAM", "ALIZDYING", "ALIZLYINGDEAD", 6,
      32, 52, 16 * BXY, 820, 30, { 8, 10, 12, 15 }, 1, 10, 40, false, true, false, true, false,
      SND_LIZ_BEG, 0, SND_LIZ_PAIN, SND_LIZ_DEATH },
    { "creeper", "DRONEFRAMES", "DRONEFRAMES", "DRONEFRAMES", "DRONEFRAMES", nullptr, nullptr, 0,
      40, 43, 16 * BXY, 0, 0, { 0, 0, 0, 0 }, 2, 1, 48, false, false, false, false, true,
      0, 0, 0, 0 },
};

struct Node { int16_t x, y, z; };  // a cell a mob can stand in: block coords of its feet

struct Mob
{
    int type, state;
    int target;          // player sprite it's after, or -1
    int lastSeen;        // tic it last saw the target
    int cooldown, flinch, pose, repath, stuck, deadTics;
    int kx, ky;          // knockback / leap velocity, units/tic << 8
    int idle;            // tics with nothing to do (for despawning)
    bool provoked;       // hurt by the player (a spider in daylight fights back)
    bool spawned, cave;  // by the spawner, and where
    bool climbing;
    int fuse;            // creeper
    int strafe, strafeTics, draw;  // skeleton: strafe direction (+1/-1), when to maybe switch, bow draw
    std::vector<Node> path;
    size_t step;         // next path node
};

struct Arrow
{
    int sprite;
    float x, y, z, vx, vy, vz;  // z in z units
    int life;
    bool stuck;
};

std::unordered_map<int, Mob> mobs;
std::vector<Arrow> arrows;
int mob_tic;
int mob_debug;
int path_expanded;  // (debug) nodes the last search expanded
bool mob_purge = true;  // clear Duke's map-placed enemies on the next tic (set at level start)

// spawner
bool spawning = true;
int wave_timer = 20 * 30;   // tics to the next surface wave (the first comes soon)
int spawn_surfaceCap = 12, spawn_caveCap = 10;

#define MOBLOG(...) do { if (mob_debug) OSD_Printf("emine_mobs: " __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Blocks, as a walker sees them

bool solidAt(int bx, int by, int bz)
{
    uint8_t const t = voxworld_get(bx, by, bz);
    return t != VW_AIR && !(voxworld_blockflags(t) & (VW_BF_NONSOLID | VW_BF_PLANT));
}

bool lavaAt(int bx, int by, int bz) { return voxworld_blockflags(voxworld_get(bx, by, bz)) & VW_BF_LAVA; }
bool waterAt(int bx, int by, int bz) { return voxworld_blockflags(voxworld_get(bx, by, bz)) & VW_BF_WATER; }
bool openAt(int bx, int by, int bz) { return !solidAt(bx, by, bz) && !lavaAt(bx, by, bz); }

// room to stand (headroom cells) and something solid underfoot
bool standable(int bx, int by, int bz, int headroom)
{
    if (!solidAt(bx, by, bz - 1) || lavaAt(bx, by, bz - 1)) return false;
    for (int h = 0; h < headroom; h++)
        if (!openAt(bx, by, bz + h)) return false;
    return true;
}

struct Walker { int headroom, maxUp, maxDrop; };

// From standing cell n into the column (nx, ny): where would it end up? Same level; up one (a hop: needs headroom
// above where it stands) or, for a climber, up a wall to the first place to stand; or down (up to maxDrop).
bool stepInto(Walker const &w, Node const &n, int nx, int ny, int *nz)
{
    if (standable(nx, ny, n.z, w.headroom)) { *nz = n.z; return true; }
    for (int up = 1; up <= w.maxUp; up++)
    {
        // climbing (or hopping) up its own column: that has to be open
        if (!openAt(n.x, n.y, n.z + w.headroom + up - 1)) break;
        if (standable(nx, ny, n.z + up, w.headroom)) { *nz = n.z + up; return true; }
        if (!solidAt(nx, ny, n.z + up - 1) && w.maxUp > 1) break;  // a climber needs wall all the way up
    }
    for (int h = 0; h < w.headroom; h++)
        if (!openAt(nx, ny, n.z + h)) return false;
    for (int dz = 1; dz <= w.maxDrop; dz++)
    {
        if (standable(nx, ny, n.z - dz, w.headroom)) { *nz = n.z - dz; return true; }
        if (!openAt(nx, ny, n.z - dz)) return false;
    }
    return false;
}

uint32_t nodeKey(int x, int y, int z) { return (uint32_t)(x & 0xFFF) | ((uint32_t)(y & 0xFFF) << 12) | ((uint32_t)(z & 0xFF) << 24); }

// A* over standing cells. Fills `out` with the path from `from` to `to`, or, if `to` can't be reached within
// maxNodes, to the reachable cell nearest it (Minecraft does the same: a partial path). Returns whether it got there.
bool findPath(Walker const &w, Node from, Node to, int maxNodes, std::vector<Node> &out)
{
    struct Rec { float g; uint32_t parent; Node n; bool closed; };
    std::unordered_map<uint32_t, Rec> recs;
    recs.reserve(maxNodes * 4);
    typedef std::pair<float, uint32_t> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;

    auto h = [&](Node const &a) {
        int const dx = klabs(a.x - to.x), dy = klabs(a.y - to.y), dz = klabs(a.z - to.z);
        return (float)max(dx, dy) + 0.414f * (float)min(dx, dy) + (float)dz;
    };

    uint32_t const k0 = nodeKey(from.x, from.y, from.z);
    recs[k0] = { 0.f, k0, from, false };
    open.push({ h(from), k0 });
    uint32_t best = k0;
    float bestH = h(from);
    bool reached = false;

    static int const dirs[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

    int expanded = 0;
    for (; !open.empty() && expanded < maxNodes;)
    {
        uint32_t const k = open.top().second;
        open.pop();
        Rec &r = recs[k];
        if (r.closed) continue;
        r.closed = true;
        expanded++;
        Node const n = r.n;
        float const g = r.g;  // (copied: inserting below can move r)

        float const hn = h(n);
        if (hn < bestH) { bestH = hn; best = k; }
        if (n.x == to.x && n.y == to.y && klabs(n.z - to.z) <= 1) { best = k; reached = true; break; }

        for (auto const &d : dirs)
        {
            int const nx = n.x + d[0], ny = n.y + d[1];
            bool const diag = d[0] && d[1];
            int nz;
            if (diag)
            {
                // diagonals only on the level, and not cutting a corner
                if (!standable(nx, ny, n.z, w.headroom)) continue;
                bool clear = true;
                for (int hh = 0; hh < w.headroom && clear; hh++)
                    clear = openAt(n.x + d[0], n.y, n.z + hh) && openAt(n.x, n.y + d[1], n.z + hh);
                if (!clear) continue;
                nz = n.z;
            }
            else if (!stepInto(w, n, nx, ny, &nz))
                continue;

            float const cost = g + (diag ? 1.414f : 1.f) + (nz > n.z ? 0.5f * (nz - n.z) : 0.f) + (waterAt(nx, ny, nz) ? 2.f : 0.f);
            uint32_t const nk = nodeKey(nx, ny, nz);
            auto const it = recs.find(nk);
            if (it != recs.end() && (it->second.closed || it->second.g <= cost)) continue;
            Node const nn = { (int16_t)nx, (int16_t)ny, (int16_t)nz };
            recs[nk] = { cost, k, nn, false };
            open.push({ cost + h(nn), nk });
        }
    }

    path_expanded = expanded;
    out.clear();
    for (uint32_t k = best;;)
    {
        Rec const &r = recs[k];
        out.push_back(r.n);
        if (r.parent == k) break;
        k = r.parent;
    }
    std::reverse(out.begin(), out.end());
    return reached;
}

bool cellOf(int32_t x, int32_t y, int32_t feetZ, Node *n)
{
    int bx, by, bz;
    bool const ok = voxworld_toblock(x, y, feetZ - 16, &bx, &by, &bz);
    *n = { (int16_t)bx, (int16_t)by, (int16_t)bz };
    return ok;
}

vec3_t nodeFeet(Node const &n)
{
    vec3_t c;
    voxworld_blockcenter(n.x, n.y, n.z, &c);
    c.z += BZ / 2;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------
// Duke-side helpers

int typeOf(int pic)
{
    if (pic == PIGCOP || pic == PIGCOPSTAYPUT || pic == PIGCOPDIVE) return MT_ZOMBIE;
    if (pic == LIZTROOP || pic == LIZTROOPSTAYPUT || pic == LIZTROOPSHOOT || pic == LIZTROOPJETPACK || pic == LIZTROOPDUCKING
        || pic == LIZTROOPRUNNING || pic == LIZTROOPONTOILET || pic == LIZTROOPJUSTSIT)
        return MT_SKELETON;
    if (pic == LIZMAN || pic == LIZMANSTAYPUT || pic == LIZMANSPITTING || pic == LIZMANFEEDING || pic == LIZMANJUMP) return MT_SPIDER;
    if (pic == DRONE) return MT_CREEPER;
    return MT_NONE;
}

int basePic(int type)
{
    switch (type)
    {
        case MT_ZOMBIE: return PIGCOP;
        case MT_SKELETON: return LIZTROOP;
        case MT_SPIDER: return LIZMAN;
        default: return DRONE;
    }
}

// The CON action label -> its offset in the script (actions say which tiles a pose uses and how fast it animates).
int32_t actionOfs(char const *name)
{
    if (!name) return 0;
    for (int i = 0; i < g_labelCnt; i++)
        if ((labeltype[i] & LABEL_ACTION) && !Bstrcmp(label + (i << 6), name))
            return labelcode[i];
    return 0;
}

struct MobActions { int32_t walk, stand, hit, attack, dying, dead; };
MobActions const &mobActions(int type)
{
    static MobActions acts[MT_COUNT];
    static int32_t forScript = -1;
    if (forScript != g_scriptSize)
    {
        for (int t = 0; t < MT_COUNT; t++)
        {
            auto const &d = mobDefs[t];
            acts[t] = { actionOfs(d.aWalk), actionOfs(d.aStand), actionOfs(d.aHit), actionOfs(d.aAttack), actionOfs(d.aDying), actionOfs(d.aDead) };
        }
        forScript = g_scriptSize;
    }
    return acts[type];
}

void setAction(int i, int32_t ofs)
{
    auto const t = actor[i].t_data;
    if (!ofs || AC_ACTION_ID(t) == ofs) return;
    AC_ACTION_ID(t) = ofs;
    AC_ACTION_COUNT(t) = 0;
    AC_CURFRAME(t) = 0;
    sprite[i].lotag = 0;  // action tics
}

bool playerAlive(DukePlayer_t const *ps) { return ps && ps->i >= 0 && sprite[ps->i].extra > 0; }

int32_t spriteHeight(spritetype const &s) { return tilesiz[s.picnum].y * s.yrepeat << 2; }

bool seesPlayer(spritetype const &s, DukePlayer_t const *ps)
{
    return cansee(s.x, s.y, s.z - spriteHeight(s) * 7 / 8, s.sectnum, ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum);
}

int32_t dist3(spritetype const &s, int32_t x, int32_t y, int32_t z)
{
    float const dx = (float)(x - s.x), dy = (float)(y - s.y), dz = (float)(z - s.z) * (1.f / 16.f);
    return (int32_t)sqrtf(dx * dx + dy * dy + dz * dz);
}

bool isExplosive(int weapon) { return weapon == RADIUSEXPLOSION || weapon == RPG || weapon == DRONE; }

int skillDmg(int const dmg[4]) { return dmg[clamp(ud.player_skill - 1, 0, 3)]; }

// Hurt the player: a red flash, a grunt, and a push of `push` (units/tic at the start; Duke friction makes ~5x that).
void hurtPlayer(DukePlayer_t *ps, int damage, int ang, int push)
{
    if (!ud.god)
        sprite[ps->i].extra = max(0, sprite[ps->i].extra - damage);
    P_PalFrom(ps, 32, 32, 0, 0);
    A_PlaySound(DUKE_GRUNT, ps->i);
    ps->vel.x += sintable[(ang + 512) & 2047] * push;
    ps->vel.y += sintable[ang & 2047] * push;
}

void setTarget(Mob &m, int playerSprite)
{
    if (m.target != playerSprite) m.path.clear();
    m.target = playerSprite;
    m.lastSeen = mob_tic;
}

// Minecraft's "alert others": zombies near one that got hurt turn on the attacker too.
void alertOthers(int i, int playerSprite)
{
    auto const &s = sprite[i];
    int const type = mobs[i].type;
    for (auto &kv : mobs)
    {
        Mob &o = kv.second;
        int const j = kv.first;
        if (j == i || o.type != type || o.state != MS_LIVE || o.target >= 0) continue;
        if (FindDistance2D(sprite[j].x - s.x, sprite[j].y - s.y) > mobDefs[type].sight || klabs(sprite[j].z - s.z) > 10 * BZ) continue;
        setTarget(o, playerSprite);
        MOBLOG("%s %d: alerted by %d\n", mobDefs[type].name, j, i);
    }
}

void startDying(int i, Mob &m, DukePlayer_t *ps)
{
    auto &s = sprite[i];
    auto const &d = mobDefs[m.type];
    P_AddKills(ps, 1);
    m.path.clear();
    s.cstat &= ~257;  // the body doesn't block or catch shots
    if (d.sndDie) A_PlaySound(d.sndDie, i);
    if (!mobActions(m.type).dying)
    {
        // no death frames (creeper): straight up in smoke
        A_Spawn(i, SMALLSMOKE);
        m.state = MS_DEAD;
        m.deadTics = MOB_CORPSE;
        return;
    }
    m.state = MS_DYING;
    if (krand() & 1) s.cstat ^= 4;
    setAction(i, mobActions(m.type).dying);
}

void gib(int i, Mob &m)
{
    A_PlaySound(SND_SQUISHED, i);
    A_DoGuts(i, JIBS6, 8);
    m.state = MS_DEAD;
    m.deadTics = MOB_CORPSE;  // removed on the next check
}

void hurt(int i, Mob &m, int weapon, DukePlayer_t *ps)
{
    auto &s = sprite[i];
    auto const &d = mobDefs[m.type];
    if (d.bleeds) A_Spawn(i, BLOOD);
    if (s.extra <= 0)
    {
        if (isExplosive(weapon) && d.bleeds) { P_AddKills(ps, 1); gib(i, m); return; }
        startDying(i, m, ps);
        return;
    }
    if (d.sndPain) A_PlaySound(d.sndPain, i);
    m.flinch = 6;
    m.draw = 0;
    // knocked away from the attacker, with a little hop
    int const from = (s.owner >= 0 && s.owner < MAXSPRITES && s.owner != i) ? s.owner : ps->i;
    int const a = getangle(s.x - sprite[from].x, s.y - sprite[from].y);
    m.kx = (sintable[(a + 512) & 2047] * MOB_KNOCK) >> 6;
    m.ky = (sintable[a & 2047] * MOB_KNOCK) >> 6;
    if (s.zvel == 0 && !m.climbing) { s.z -= 512; s.zvel = -MOB_HOP; }
    if (playerAlive(ps))
    {
        m.provoked = true;
        setTarget(m, ps->i);
        if (d.alerts) alertOthers(i, ps->i);
    }
}

// Pick a random spot within MOB_STROLL blocks to walk to.
void startStroll(int i, Mob &m, Walker const &w)
{
    auto const &s = sprite[i];
    Node from;
    if (!cellOf(s.x, s.y, s.z, &from)) return;
    Node to = { (int16_t)(from.x + (krand() % (2 * MOB_STROLL + 1)) - MOB_STROLL),
                (int16_t)(from.y + (krand() % (2 * MOB_STROLL + 1)) - MOB_STROLL), from.z };
    for (int dz = 3; dz >= -7; dz--)
        if (standable(to.x, to.y, from.z + dz, w.headroom)) { to.z = from.z + dz; break; }
    findPath(w, from, to, 200, m.path);
    m.step = 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Arrows: plain sprites we move ourselves - they arc, stick in blocks for a few seconds, and hurt the player.

void shootArrow(int i, DukePlayer_t *ps)
{
    auto const &s = sprite[i];
    float const x0 = (float)s.x, y0 = (float)s.y, z0 = (float)(s.z - spriteHeight(s) * 7 / 10);
    float const tx = (float)ps->pos.x, ty = (float)ps->pos.y, tz = (float)(ps->pos.z + 2048);  // their chest
    float const d = sqrtf((tx - x0) * (tx - x0) + (ty - y0) * (ty - y0));
    if (d < 1.f) return;
    // a little inaccuracy (Minecraft: worse on easier skills)
    float const spread = (float)(krand() % 61 - 30) * (0.0015f + 0.0005f * (4 - clamp<int>(ud.player_skill, 1, 4)));
    float const ca = cosf(spread), sa = sinf(spread);
    float const ux = (tx - x0) / d, uy = (ty - y0) / d;
    float const t = d / ARROW_SPEED;  // tics to get there; aim up so the drop lands it on them
    Arrow a;
    a.x = x0; a.y = y0; a.z = z0;
    a.vx = (ux * ca - uy * sa) * ARROW_SPEED;
    a.vy = (ux * sa + uy * ca) * ARROW_SPEED;
    a.vz = (tz - z0 - 0.5f * ARROW_GRAVITY * t * t) / t + (float)(krand() % 201 - 100);
    a.life = 120;
    a.stuck = false;
    a.sprite = A_InsertSprite(s.sectnum, s.x, s.y, (int32_t)z0, ARROW_TILE, 0, 42, 42, getangle((int)a.vx, (int)a.vy), 0, 0, i, STAT_DEFAULT);
    sprite[a.sprite].cstat = 128;  // centred, no collision
    arrows.push_back(a);
    A_PlaySound(SND_PRED_ATTACK, i);
}

void moveArrows(void)
{
    auto const ps = g_player[myconnectindex].ps;
    for (size_t k = 0; k < arrows.size();)
    {
        Arrow &a = arrows[k];
        bool done = --a.life <= 0 || sprite[a.sprite].picnum != ARROW_TILE || sprite[a.sprite].statnum != STAT_DEFAULT;
        if (!done && !a.stuck)
        {
            a.vz += ARROW_GRAVITY;
            vec3_t const p0 = { (int32_t)a.x, (int32_t)a.y, (int32_t)a.z };
            float t = 1.f;
            vwhit_t vh;
            bool const hitBlock = voxworld_raycast(&p0, (int32_t)a.vx, (int32_t)a.vy, (int32_t)a.vz, 1.f, &vh);
            if (hitBlock) t = max(0.f, vh.dist - 0.05f);

            // the player: closest approach of this step to their axis, within their height
            if (playerAlive(ps))
            {
                float const px = (float)ps->pos.x - a.x, py = (float)ps->pos.y - a.y;
                float const vv = a.vx * a.vx + a.vy * a.vy;
                float u = vv > 0.f ? (px * a.vx + py * a.vy) / vv : 0.f;
                u = clamp(u, 0.f, t);
                float const cx = a.x + a.vx * u - ps->pos.x, cy = a.y + a.vy * u - ps->pos.y, cz = a.z + a.vz * u;
                if (cx * cx + cy * cy < 220.f * 220.f && cz > ps->pos.z - 1536 && cz < sprite[ps->i].z)
                {
                    hurtPlayer(ps, skillDmg(mobDefs[MT_SKELETON].dmg), getangle((int)a.vx, (int)a.vy), 30);
                    done = true;
                }
            }
            if (!done)
            {
                a.x += a.vx * t; a.y += a.vy * t; a.z += a.vz * t;
                if (hitBlock) { a.stuck = true; a.life = 90; }
                vec3_t const at = { (int32_t)a.x, (int32_t)a.y, (int32_t)a.z };
                setsprite(a.sprite, &at);
            }
        }
        if (done)
        {
            if (sprite[a.sprite].picnum == ARROW_TILE && sprite[a.sprite].statnum == STAT_DEFAULT) A_DeleteSprite(a.sprite);
            arrows[k] = arrows.back();
            arrows.pop_back();
            continue;
        }
        k++;
    }
}

// ---------------------------------------------------------------------------------------------------------------

void initMob(int i, int type)
{
    auto &s = sprite[i];
    actor[i].t_data[9] = MOB_MAGIC;
    Mob m = {};
    m.type = type;
    m.state = MS_LIVE;
    m.target = -1;
    m.stuck = krand() & 15;
    m.strafe = (krand() & 1) ? 1 : -1;
    mobs[i] = m;
    s.picnum = basePic(type);
    s.extra = mobDefs[type].health;
    s.hitag = 0;  // no CON move flags
    s.xrepeat = s.yrepeat = mobDefs[type].repeat;
    setAction(i, mobActions(type).stand);
}

// Is this mob hostile right now? (A spider in good light only fights back.)
bool wantsToHunt(int i, Mob const &m)
{
    if (m.type != MT_SPIDER || m.provoked) return true;
    Node n;
    cellOf(sprite[i].x, sprite[i].y, sprite[i].z, &n);
    return voxworld_lightlevel(n.x, n.y, n.z) < 10;
}

void explode(int i)
{
    A_Spawn(i, EXPLOSION2);
    A_PlaySound(SND_PIPEBOMB_EXPLODE, i);
    int const d = CREEPER_DAMAGE;
    A_RadiusDamage(i, CREEPER_RADIUS, d >> 2, d >> 1, d - (d >> 2), d);
}

int mobThink(int i, int playerNum)
{
    auto &s = sprite[i];
    auto const t = actor[i].t_data;
    auto const ps = g_player[playerNum].ps;

    if (t[9] != MOB_MAGIC || mobs.find(i) == mobs.end())
        initMob(i, typeOf(s.picnum));
    Mob &m = mobs[i];
    auto const &d = mobDefs[m.type];
    auto const &act = mobActions(m.type);
    // (Minecraft lets a mob drop further when it's after something; ours take no fall damage anyway)
    Walker const w = { d.headroom, d.maxUp, m.target >= 0 ? 8 : 3 };

    int const weapon = A_IncurDamage(i);

    if (m.state != MS_LIVE)
    {
        A_Fall(i);
        if (isExplosive(weapon) && d.bleeds) gib(i, m);  // blow the body apart
        if (m.state == MS_DYING && AC_ACTION_COUNT(t) >= d.dyingFrames)
        {
            m.state = MS_DEAD;
            setAction(i, act.dead);
            A_PlaySound(SND_THUD, i);
            if (d.bleeds && (krand() & 3) == 0) A_Spawn(i, BLOODPOOL);
        }
        if (m.state == MS_DEAD && ++m.deadTics > MOB_CORPSE)
        {
            if (m.deadTics == MOB_CORPSE + 1) A_Spawn(i, SMALLSMOKE);
            mobs.erase(i);
            return 2;
        }
        return 1;
    }

    if (weapon >= 0)
    {
        hurt(i, m, weapon, ps);
        if (m.state != MS_LIVE)
        {
            if (m.deadTics >= MOB_CORPSE) { mobs.erase(i); return 2; }
            return 1;
        }
    }

    // --- targeting
    bool sees = false;
    int32_t const pdist = playerAlive(ps) ? dist3(s, ps->pos.x, ps->pos.y, sprite[ps->i].z) : INT32_MAX;
    if (m.target < 0)
    {
        if (pdist < d.sight && (mob_tic + i) % 15 == 0 && wantsToHunt(i, m) && seesPlayer(s, ps))
        {
            setTarget(m, ps->i);
            sees = true;
            MOBLOG("%s %d: sees you at %d blocks\n", d.name, i, pdist / BXY);
        }
    }
    else
    {
        sees = playerAlive(ps) && seesPlayer(s, ps);
        // (within 4 blocks it knows you're there, seen or not: e.g. you on a pillar right above it)
        if (sees || pdist < 4 * BXY) m.lastSeen = mob_tic;
        bool calm = !m.provoked && !wantsToHunt(i, m) && krand() % 150 == 0;  // a spider in the light loses interest
        if (!playerAlive(ps) || pdist > d.sight || mob_tic - m.lastSeen > MOB_MEMORY || calm)
        {
            MOBLOG("%s %d: lost you\n", d.name, i);
            m.target = -1;
            m.provoked = false;
            m.path.clear();
        }
    }
    m.idle = (m.target >= 0) ? 0 : m.idle + 1;

    // --- where to go
    int32_t gx = s.x, gy = s.y;
    bool go = false, direct = false;
    Node here;
    bool const inWorld = cellOf(s.x, s.y, s.z, &here);
    int32_t const hdist = playerAlive(ps) ? FindDistance2D(ps->pos.x - s.x, ps->pos.y - s.y) : INT32_MAX;
    int32_t const pfeet = playerAlive(ps) ? sprite[ps->i].z : 0;
    bool const level = playerAlive(ps) && klabs(pfeet - s.z) < BZ * 3 / 2;
    int speed = d.speed;
    int faceAng = -1;  // turn to face this instead of the way it walks
    int32_t sideX = 0, sideY = 0;  // skeleton strafing (units/tic)

    if (m.target >= 0)
    {
        bool holdRange = false;
        if (d.ranged && sees && hdist < 15 * BXY)
        {
            // in bow range: stand off and strafe (Minecraft's RangedBowAttackGoal)
            holdRange = true;
            faceAng = getangle(ps->pos.x - s.x, ps->pos.y - s.y);
            if (--m.strafeTics <= 0) { m.strafeTics = 30; if (krand() % 10 < 3) m.strafe = -m.strafe; }
            int fwd = 0;
            if (hdist > 11 * BXY) fwd = 1;
            else if (hdist < 4 * BXY) fwd = -1;
            int const v = d.speed / 2;
            sideX = (sintable[(faceAng + 512 + 512 * m.strafe) & 2047] * v + sintable[(faceAng + 512) & 2047] * v * fwd) >> 14;
            sideY = (sintable[(faceAng + 512 * m.strafe) & 2047] * v + sintable[faceAng & 2047] * v * fwd) >> 14;
            m.path.clear();
        }
        else if (d.explodes && m.fuse > 0)
            ;  // swelling: stand still
        else if (sees && hdist < 2 * BXY && level && !d.ranged)
            direct = true;  // close and in plain sight: straight at them
        else if (inWorld && ((sees && --m.repath <= 0) || m.path.empty() || m.stuck > 15))
        {
            Node to;
            cellOf(ps->pos.x, ps->pos.y, pfeet, &to);
            bool const got = findPath(w, here, to, 800, m.path);
            m.step = 1;
            m.stuck = 0;
            m.repath = (hdist > 16 * BXY) ? 20 : 10;
            MOBLOG("%s %d: path %d nodes%s (expanded %d; from %d,%d,%d standable %d, z %d floorz %d; to %d,%d,%d)\n", d.name, i, (int)m.path.size(),
                   got ? "" : " (partial)", path_expanded, here.x, here.y, here.z, (int)standable(here.x, here.y, here.z, d.headroom), (int)s.z,
                   (int)actor[i].floorz, to.x, to.y, to.z);
        }
        if (direct) { gx = ps->pos.x; gy = ps->pos.y; go = hdist > MOB_STOP; }
        if (holdRange) go = false;
    }
    else if (m.path.empty() || m.step >= m.path.size())
    {
        m.path.clear();
        if (inWorld && krand() % MOB_WANDER == 0) startStroll(i, m, w);
    }

    bool hopUp = false;
    if (!direct && m.step < m.path.size() && !(d.explodes && m.fuse > 0) && !(sideX | sideY))
    {
        vec3_t const wp = nodeFeet(m.path[m.step]);
        if (FindDistance2D(wp.x - s.x, wp.y - s.y) < 160 && klabs(wp.z - s.z) < BZ)
        {
            m.step++;
            if (m.step >= m.path.size() && m.target < 0) m.path.clear();
        }
        if (m.step < m.path.size())
        {
            vec3_t const w2 = nodeFeet(m.path[m.step]);
            gx = w2.x; gy = w2.y; go = true;
            hopUp = m.path[m.step].z == here.z + 1 && FindDistance2D(w2.x - s.x, w2.y - s.y) < BXY;
        }
    }

    // --- move
    bool const onGround = s.z >= actor[i].floorz - (2 << 8);
    if (go)
    {
        int const want = getangle(gx - s.x, gy - s.y);
        s.ang = (s.ang + clamp(G_GetAngleDelta(s.ang, want), -MOB_TURN, MOB_TURN)) & 2047;
    }
    else if (faceAng >= 0)
        s.ang = (s.ang + clamp(G_GetAngleDelta(s.ang, faceAng), -MOB_TURN, MOB_TURN)) & 2047;

    int32_t vx = (m.kx >> 8) + sideX, vy = (m.ky >> 8) + sideY;
    if (go && m.flinch == 0)
    {
        vx += (sintable[(s.ang + 512) & 2047] * speed) >> 14;
        vy += (sintable[s.ang & 2047] * speed) >> 14;
    }
    if (onGround || m.climbing) { m.kx = m.kx * 2 / 3; m.ky = m.ky * 2 / 3; }
    else { m.kx = m.kx * 31 / 32; m.ky = m.ky * 31 / 32; }  // in the air (a leap) it keeps going

    bool wallAhead = false;
    if (vx | vy)
    {
        vec2_t const before = s.xy;
        int16_t const ang = s.ang;
        int const r = A_MoveSprite(i, { vx * 2, vy * 2, 0 }, CLIPMASK0);  // (A_MoveSprite moves half of what it's given)
        s.ang = ang;  // (A_MoveSprite turns enemies that hit a wall)
        int32_t const moved = FindDistance2D(s.x - before.x, s.y - before.y);
        bool const blocked = (r & 49152) == 32768 || moved < speed / 3;
        m.stuck = ((go || sideX || sideY) && blocked) ? m.stuck + 1 : 0;
        if (blocked && (sideX | sideY)) m.strafe = -m.strafe;
        if (go && blocked && inWorld)
        {
            int bx, by, bz;
            int const ma = getangle(vx, vy);
            voxworld_toblock(s.x + (sintable[(ma + 512) & 2047] >> 5), s.y + (sintable[ma & 2047] >> 5), s.z - 16, &bx, &by, &bz);
            wallAhead = solidAt(bx, by, bz) || solidAt(bx, by, bz + 1);
            // a step it could hop up
            if (onGround && solidAt(bx, by, bz) && openAt(bx, by, bz + 1) && (d.headroom < 2 || openAt(bx, by, bz + 2))
                && openAt(here.x, here.y, here.z + d.headroom))
                hopUp = true;
        }
    }

    // spiders climb what's in their way (until there's no room above)
    m.climbing = d.climbs && go && wallAhead && inWorld && openAt(here.x, here.y, here.z + d.headroom);
    if (m.climbing)
    {
        s.z -= MOB_CLIMB;
        s.zvel = 0;
    }
    else
    {
        if (hopUp && onGround && s.zvel == 0 && !d.climbs)
        {
            s.z -= 512;
            s.zvel = -MOB_JUMP;
        }
        A_Fall(i);
    }

    // Duke lets you stand on a monster whose head is within step height (one a block lower than you, say); in
    // Minecraft you can't. Slide the player off.
    if (playerAlive(ps) && hdist < 512)
    {
        int32_t const top = s.z - spriteHeight(s);
        if (pfeet <= top + 1024 && pfeet >= top - 2048)
        {
            int const a = (hdist > 8) ? getangle(ps->pos.x - s.x, ps->pos.y - s.y) : s.ang;
            ps->vel.x += sintable[(a + 512) & 2047] * 24;
            ps->vel.y += sintable[a & 2047] * 24;
        }
    }

    // --- attack
    if (m.cooldown > 0) m.cooldown--;
    if (m.target >= 0 && d.reach && m.cooldown == 0 && hdist <= d.reach && level && sees)
    {
        hurtPlayer(ps, skillDmg(d.dmg), getangle(ps->pos.x - s.x, ps->pos.y - s.y), 60);  // ~0.6 blocks
        m.cooldown = d.cooldown;
        m.pose = 10;
        MOBLOG("%s %d: hit you (health %d)\n", d.name, i, (int)sprite[ps->i].extra);
    }

    // spider: leap at the player from 2-4 blocks (Minecraft's LeapAtTargetGoal)
    if (m.type == MT_SPIDER && m.target >= 0 && sees && onGround && !m.climbing && hdist > 2 * BXY && hdist < 4 * BXY
        && krand() % 7 == 0)
    {
        int const a = getangle(ps->pos.x - s.x, ps->pos.y - s.y);
        int const hv = clamp(hdist / 17, 60, 140);
        m.kx = (sintable[(a + 512) & 2047] * hv) >> 6;
        m.ky = (sintable[a & 2047] * hv) >> 6;
        s.z -= 512;
        s.zvel = -1500;
        m.pose = 17;
        MOBLOG("spider %d: leaps\n", i);
    }

    // skeleton: draw the bow for half a second, then loose
    if (d.ranged)
    {
        if (m.target >= 0 && sees && hdist < 15 * BXY && m.cooldown == 0)
        {
            if (++m.draw >= 15)
            {
                shootArrow(i, ps);
                m.draw = 0;
                static int const interval[4] = { 75, 60, 50, 40 };
                m.cooldown = interval[clamp(ud.player_skill - 1, 0, 3)];
                MOBLOG("skeleton %d: shoots\n", i);
            }
        }
        else
            m.draw = 0;
    }

    // creeper: swell within 3 blocks, wind back down past 7 or out of sight; blow up when the fuse runs out
    if (d.explodes)
    {
        bool const close = m.target >= 0 && sees && hdist < CREEPER_LIGHT && klabs(pfeet - s.z) < 2 * BZ;
        bool const keep = m.fuse > 0 && m.target >= 0 && sees && hdist < CREEPER_CANCEL;
        if (close || keep)
        {
            if (m.fuse == 0) { A_PlaySound(SND_STEAM_HISSING, i); MOBLOG("creeper %d: hisses\n", i); }
            if (++m.fuse >= CREEPER_FUSE)
            {
                MOBLOG("creeper %d: boom\n", i);
                explode(i);
                mobs.erase(i);
                return 2;
            }
        }
        else if (m.fuse > 0)
            m.fuse--;
        // swell and flash
        int const r = d.repeat + m.fuse * 12 / CREEPER_FUSE;
        s.xrepeat = r + ((m.fuse && (m.fuse & 4)) ? 2 : 0);
        s.yrepeat = r;
        s.shade = (m.fuse && ((m.fuse >> 2) & 1)) ? -40 : 0;
    }

    // --- look
    if (m.flinch > 0) { m.flinch--; setAction(i, act.hit); }
    else if (m.pose > 0) { m.pose--; setAction(i, act.attack); }
    else if (d.ranged && m.draw > 0) setAction(i, act.attack);
    else setAction(i, (go || sideX || sideY || m.climbing) ? act.walk : act.stand);

    if (d.sndRoam && (krand() & 255) == 0)
        A_PlaySound((d.sndRoam2 && (krand() & 1)) ? d.sndRoam2 : d.sndRoam, i);

    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Spawning

int pickType(bool cave)
{
    // weights: zombie, skeleton, spider, creeper
    // (zombies come in bigger packs, so they get a smaller share of the packs)
    static int const surface[MT_COUNT] = { 30, 30, 15, 25 };
    static int const under[MT_COUNT] = { 25, 25, 30, 20 };
    int const *wts = cave ? under : surface;
    int r = krand() % 100;
    for (int t = 0; t < MT_COUNT; t++)
        if ((r -= wts[t]) < 0) return t;
    return MT_ZOMBIE;
}

int packSize(int type)
{
    switch (type)
    {
        case MT_ZOMBIE: return 2 + krand() % 2;    // 2-3
        case MT_SKELETON: return 1 + krand() % 3;  // 1-3
        default: return 1 + krand() % 2;           // 1-2
    }
}

bool goodGround(int bx, int by, int bz)
{
    uint8_t const below = voxworld_get(bx, by, bz - 1);
    return below != VW_LEAVES && below != VW_WOOD && below != VW_BEDROCK && !waterAt(bx, by, bz) && !waterAt(bx, by, bz + 1);
}

// The highest solid (or liquid) layer in a column, or -1.
int columnTop(int bx, int by)
{
    int sx, sy, sz;
    voxworld_size(&sx, &sy, &sz);
    for (int bz = sz - 1; bz >= 0; bz--)
        if (solidAt(bx, by, bz) || waterAt(bx, by, bz) || lavaAt(bx, by, bz)) return bz;
    return -1;
}

// The highest place to stand in a column (open sky above it).
bool surfaceCell(int bx, int by, Node *n)
{
    int const top = columnTop(bx, by);
    if (top < 0 || !standable(bx, by, top + 1, 2) || !goodGround(bx, by, top + 1)) return false;
    *n = { (int16_t)bx, (int16_t)by, (int16_t)(top + 1) };
    return true;
}

// A dark place to stand somewhere down this column (a cave).
bool caveCell(int bx, int by, Node *n)
{
    int const hi = columnTop(bx, by) - 3;
    if (hi < 3) return false;
    int bz = 2 + krand() % (hi - 1);
    for (int k = 0; k < 16 && bz > 1; k++, bz--)
        if (standable(bx, by, bz, 2) && goodGround(bx, by, bz) && voxworld_lightlevel(bx, by, bz) == 0)
        {
            *n = { (int16_t)bx, (int16_t)by, (int16_t)bz };
            return true;
        }
    return false;
}

bool playerSees(DukePlayer_t const *ps, Node const &n)
{
    vec3_t const f = nodeFeet(n);
    return cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum, f.x, f.y, f.z - BZ, voxworld_sector());
}

int spawnMob(int type, Node const &n, bool cave)
{
    vec3_t const f = nodeFeet(n);
    int const j = A_InsertSprite(voxworld_sector(), f.x, f.y, f.z, basePic(type), 0, 40, 40, krand() & 2047, 0, 0, 0, STAT_DEFAULT);
    A_Spawn(-1, j);  // set up as Duke would a map-placed one (asleep)...
    if (sprite[j].statnum != STAT_ZOMBIEACTOR && sprite[j].statnum != STAT_ACTOR) { A_DeleteSprite(j); return -1; }
    changespritestat(j, STAT_ACTOR);  // ...but awake (and no "I see you" yell)
    actor[j].timetosleep = 0;
    initMob(j, type);
    mobs[j].spawned = true;
    mobs[j].cave = cave;
    return j;
}

// A pack around a spot: same type, within 4 blocks, each on its own valid cell.
int spawnPack(DukePlayer_t const *ps, Node const &at, bool cave)
{
    int const type = pickType(cave);
    int const want = packSize(type);
    int n = 0;
    for (int tries = 0; tries < want * 4 && n < want; tries++)
    {
        Node c = at;
        if (tries > 0)
        {
            int const bx = at.x + krand() % 9 - 4, by = at.y + krand() % 9 - 4;
            bool ok = false;
            for (int dz = 2; dz >= -3 && !ok; dz--)
                if (standable(bx, by, at.z + dz, 2) && goodGround(bx, by, at.z + dz)
                    && (!cave || voxworld_lightlevel(bx, by, at.z + dz) == 0))
                {
                    c = { (int16_t)bx, (int16_t)by, (int16_t)(at.z + dz) };
                    ok = true;
                }
            if (!ok) continue;
        }
        if (!cave && playerSees(ps, c)) continue;
        if (spawnMob(type, c, cave) >= 0) n++;
    }
    if (n) MOBLOG("spawned %d %s%s at %d blocks\n", n, mobDefs[type].name, n > 1 ? "s" : "",
                  FindDistance2D(nodeFeet(at).x - ps->pos.x, nodeFeet(at).y - ps->pos.y) / BXY);
    return n;
}

bool randomSpot(DukePlayer_t const *ps, bool cave, Node *out)
{
    Node me;
    if (!cellOf(ps->pos.x, ps->pos.y, sprite[ps->i].z, &me)) return false;
    int const a = krand() & 2047, r = 24 + krand() % 25;  // 24-48 blocks
    int const bx = me.x + (sintable[(a + 512) & 2047] * r >> 14), by = me.y + (sintable[a & 2047] * r >> 14);
    return cave ? caveCell(bx, by, out) : surfaceCell(bx, by, out);
}

void purgeDukeEnemies(void)
{
    int n = 0;
    for (int stat : { STAT_ZOMBIEACTOR, STAT_ACTOR })
        for (int j = headspritestat[stat], nx; j >= 0; j = nx)
        {
            nx = nextspritestat[j];
            if (sprite[j].sectnum == voxworld_sector() && A_CheckEnemySprite(&sprite[j]) && mobs.find(j) == mobs.end())
            {
                A_DeleteSprite(j);
                n++;
            }
        }
    MOBLOG("cleared %d map enemies\n", n);
}

void spawnerTick(void)
{
    auto const ps = g_player[myconnectindex].ps;
    if (!voxworld_active() || !ps || ps->i < 0) return;

    if (mob_purge)
    {
        mobs.clear();
        for (auto const &a : arrows)
            if (sprite[a.sprite].picnum == ARROW_TILE && sprite[a.sprite].statnum == STAT_DEFAULT) A_DeleteSprite(a.sprite);
        arrows.clear();
        purgeDukeEnemies();
        mob_purge = false;
        wave_timer = 20 * 30;
    }

    // forget sprites that aren't our mobs any more (deleted, reused)
    for (auto it = mobs.begin(); it != mobs.end();)
    {
        auto const &s = sprite[it->first];
        if (s.statnum != STAT_ACTOR || actor[it->first].t_data[9] != MOB_MAGIC || typeOf(s.picnum) != it->second.type)
            it = mobs.erase(it);
        else
            ++it;
    }

    // despawning (Minecraft: at once past 128 blocks; past 32, idle for 30 s, 1 in 800 a tic)
    int surface = 0, cave = 0;
    for (auto it = mobs.begin(); it != mobs.end();)
    {
        int const j = it->first;
        Mob const &m = it->second;
        int32_t const dd = FindDistance2D(sprite[j].x - ps->pos.x, sprite[j].y - ps->pos.y);
        if (m.state == MS_LIVE && (dd > 128 * BXY || (dd > 32 * BXY && m.idle > 900 && krand() % 800 == 0)))
        {
            MOBLOG("%s %d despawned at %d blocks\n", mobDefs[m.type].name, j, dd / BXY);
            A_DeleteSprite(j);
            it = mobs.erase(it);
            continue;
        }
        if (m.spawned && m.state == MS_LIVE) (m.cave ? cave : surface)++;
        ++it;
    }

    if (!spawning || ud.monsters_off || !playerAlive(ps) || mob_tic % 20) return;

    // caves: whenever there's room under the cap, a few tries at a dark spot
    if (cave < spawn_caveCap)
        for (int k = 0; k < 6; k++)
        {
            Node n;
            if (randomSpot(ps, true, &n)) { spawnPack(ps, n, true); break; }
        }

    // surface: waves
    if ((wave_timer -= 20) <= 0)
    {
        int packs = 0;
        for (int k = 0; k < 24 && packs < 2 && surface < spawn_surfaceCap; k++)
        {
            Node n;
            if (randomSpot(ps, false, &n) && !playerSees(ps, n))
            {
                int const got = spawnPack(ps, n, false);
                surface += got;
                packs += got > 0;
            }
        }
        wave_timer = (40 + krand() % 41) * 30;
        MOBLOG("wave: %d packs; next in %d s\n", packs, wave_timer / 30);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Console

int osdcmd_emine_mobs(osdcmdptr_t parm)
{
    if (parm->numparms >= 1 && !Bstrcasecmp(parm->parms[0], "anger"))
    {
        // every mob turns on you (as if you'd hit it): for testing
        auto const ps = g_player[myconnectindex].ps;
        for (auto &kv : mobs)
            if (kv.second.state == MS_LIVE && ps->i >= 0) { kv.second.provoked = true; setTarget(kv.second, ps->i); }
        return OSDCMD_OK;
    }
    if (parm->numparms >= 1) { mob_debug = Batol(parm->parms[0]); return OSDCMD_OK; }
    auto const ps = g_player[myconnectindex].ps;
    int n[MT_COUNT] = {};
    for (auto const &kv : mobs)
    {
        auto const &s = sprite[kv.first];
        auto const &m = kv.second;
        n[m.type]++;
        OSD_Printf("emine_mobs: %s %d: %s hp %d target %d path %d/%d dist %d blocks %s%s\n", mobDefs[m.type].name, kv.first,
                   m.state == MS_LIVE ? (m.target >= 0 ? "hunting" : (m.path.empty() ? "idle" : "strolling")) : "dead",
                   (int)s.extra, m.target, (int)m.step, (int)m.path.size(), FindDistance2D(ps->pos.x - s.x, ps->pos.y - s.y) / BXY,
                   m.spawned ? (m.cave ? "cave" : "surface") : "placed", m.climbing ? " climbing" : "");
    }
    OSD_Printf("emine_mobs: %d zombies, %d skeletons, %d spiders, %d creepers; %d arrows; next wave in %d s\n", n[0], n[1], n[2], n[3],
               (int)arrows.size(), wave_timer / 30);
    return OSDCMD_OK;
}

// `emine_cell <bx> <by> <bz>`: the block types around a cell, layer by layer (dev: why can't a mob get anywhere?)
int osdcmd_emine_cell(osdcmdptr_t parm)
{
    if (parm->numparms < 3) return OSDCMD_SHOWHELP;
    int const bx = Batol(parm->parms[0]), by = Batol(parm->parms[1]), bz = Batol(parm->parms[2]);
    for (int z = bz + 3; z >= bz - 3; z--)
    {
        char line[128];
        int o = Bsprintf(line, "emine_cell: z %d:", z);
        for (int y = by - 2; y <= by + 2; y++)
        {
            o += Bsprintf(line + o, " ");
            for (int x = bx - 2; x <= bx + 2; x++)
                o += Bsprintf(line + o, "%c", "0123456789abcdefghijklmnopqrstuvwxyz"[min<int>(voxworld_get(x, y, z), 35)]);
        }
        OSD_Printf("%s\n", line);
    }
    return OSDCMD_OK;
}

// `emine_pillar [height]`: stand the player on a stone pillar (dev: something for mobs to climb or get round).
int osdcmd_emine_pillar(osdcmdptr_t parm)
{
    auto const ps = g_player[myconnectindex].ps;
    if (!voxworld_active() || ps->i < 0) return OSDCMD_OK;
    int const h = parm->numparms >= 1 ? clamp(Batol(parm->parms[0]), 1, 20) : 3;
    Node n;
    cellOf(ps->pos.x, ps->pos.y, sprite[ps->i].z, &n);
    for (int k = 0; k < h; k++)
        voxworld_set(n.x, n.y, n.z + k, VW_STONE);
    vec3_t const feet = nodeFeet({ n.x, n.y, (int16_t)(n.z + h) });  // on top, in the middle
    ps->pos = { feet.x, feet.y, feet.z - PHEIGHT };
    ps->opos = ps->pos;
    ps->bobpos = { feet.x, feet.y };
    ps->vel = { 0, 0, 0 };
    setsprite(ps->i, &feet);
    OSD_Printf("emine_pillar: %d blocks at %d,%d,%d\n", h, n.x, n.y, n.z);
    return OSDCMD_OK;
}

// `emine_spawning [0|1|wave|caps <surface> <cave>]`
int osdcmd_emine_spawning(osdcmdptr_t parm)
{
    if (parm->numparms >= 1)
    {
        if (!Bstrcasecmp(parm->parms[0], "wave")) wave_timer = 0;
        else if (!Bstrcasecmp(parm->parms[0], "caps") && parm->numparms >= 3)
        {
            spawn_surfaceCap = Batol(parm->parms[1]);
            spawn_caveCap = Batol(parm->parms[2]);
        }
        else spawning = Batol(parm->parms[0]) != 0;
    }
    OSD_Printf("emine_spawning: %s, caps surface %d cave %d, next wave in %d s\n", spawning ? "on" : "off", spawn_surfaceCap,
               spawn_caveCap, wave_timer / 30);
    return OSDCMD_OK;
}

}  // namespace

int Emine_ActorThink(int spriteNum, int playerNum)
{
    auto const &s = sprite[spriteNum];
    if (!voxworld_active() || s.sectnum != voxworld_sector() || typeOf(s.picnum) == MT_NONE)
        return 0;
    return mobThink(spriteNum, playerNum);
}

void Emine_MobTick(void)
{
    mob_tic++;
    moveArrows();
    spawnerTick();
}

void Emine_MobsLevelStart(void) { mob_purge = true; }

void Emine_RegisterMobOSD(void)
{
    OSD_RegisterFunction("emine_mobs", "emine_mobs [0|1|anger]: list the mobs and what they're doing; 1 = log their decisions; anger = all of them come for you",
                         osdcmd_emine_mobs);
    OSD_RegisterFunction("emine_pillar", "emine_pillar [height]: dev - stand on a stone pillar (default 3 blocks)", osdcmd_emine_pillar);
    OSD_RegisterFunction("emine_cell","emine_cell <bx> <by> <bz>: dev - the block types around a cell (rows of y, 5x5)", osdcmd_emine_cell);
    OSD_RegisterFunction("emine_spawning", "emine_spawning [0|1|wave|caps <surface> <cave>]: mob spawning on/off, a surface wave now, or the caps",
                         osdcmd_emine_spawning);
}

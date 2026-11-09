// eMine32: Minecraft-style mob behaviour, in C++ rather than Duke's CON scripts (it needs to see the blocks: paths,
// steps, jumps). An actor handled here skips its CON code; A_Execute calls Emine_ActorThink first.
//
// Zombie (art on PIGCOP's tiles; Duke's CON actions APIG* still say which tiles each pose uses).
// Behaviour follows Minecraft Java's zombie, numbers converted to Build units (a block is 512 wide, 8192 tall;
// 30 game tics a second against Minecraft's 20):
//  - notices a player it can see within 35 blocks (checked every half second), and keeps chasing while it has seen
//    them in the last 3 seconds (Minecraft's 60-tick "unseen memory"); when it loses them it walks on to where it
//    last saw them, then gives up and wanders.
//  - walks a path found over the blocks (A*: steps up one block with a hop, drops down up to three, needs two blocks
//    of headroom, keeps out of lava), so it comes round walls and up hills instead of pushing into them.
//  - melee: hits when within ~1.4 blocks, once a second, 15 health on Let's Rock (Minecraft Normal: 3 of 20 hearts),
//    with a small knockback.
//  - when hurt: knocked back with a little hop, flinches, turns on whoever hurt it even if it can't see them, and
//    alerts the other zombies within 35 blocks (Minecraft's HurtByTarget "alert others").
//  - idle: every ~6 seconds on average, strolls to a random spot within 10 blocks.
//  - dies with the fall-over frames, lies there 10 seconds, then puffs away (Minecraft removes the body; we let it
//    lie for a bit first). Explosives gib it.
// Not yet: burning in daylight (no day/night yet), breaking doors (no doors), drops (rotten flesh comes with
// wildlife and meat), its own sounds (still the pig cop's).
//
// Copyright (C) 2026 the eMine32 project. GPLv2.

#include "duke3d.h"
#include "voxworld.h"
#include "emine.h"

enum { ZB_SND_ROAM = 120, ZB_SND_PAIN = 123, ZB_SND_DYING = 124, ZB_SND_ROAM2 = 205 };  // the pig cop's, for now (soundefs.h)

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <vector>

namespace
{

constexpr int BXY = VW_BLOCKXY, BZ = VW_BLOCKZ;

constexpr int ZB_HEALTH   = 40;               // 5ish pistol shots, one shotgun blast up close
constexpr int ZB_SPEED    = 40;               // build units per tic (tuned against the player's walk; see below)
constexpr int ZB_TURN     = 160;              // max turn per tic (2048 = full circle)
constexpr int ZB_SIGHT    = 35 * BXY;         // follow range
constexpr int ZB_MEMORY   = 90;               // tics a lost target is remembered (3 s)
constexpr int ZB_REACH    = 717;              // 1.4 blocks, centre to centre
constexpr int ZB_STOP     = 400;              // stops pushing forward this close
constexpr int ZB_COOLDOWN = 30;               // 1 s between hits
constexpr int ZB_EYE      = 10600;            // eye height above its feet (z units)
constexpr int ZB_JUMP     = 1780;             // zvel for a hop up one block (peaks ~1.1 blocks with Duke's gravity)
constexpr int ZB_KNOCK    = 137;              // knockback when hit, units/tic at the start (~0.8 blocks in all)
constexpr int ZB_HOP      = 1000;             // the little hop when hit
constexpr int ZB_WANDER   = 120;              // 1 in this many tics, an idle zombie strolls off (~6 s on average)
constexpr int ZB_STROLL   = 10;               // ...to somewhere within this many blocks
constexpr int ZB_CORPSE   = 300;              // tics the body lies there before puffing away
constexpr int ZB_MAGIC    = 0x2B1E;           // in t_data[9]: this zombie has its state set up

enum { ZS_LIVE, ZS_DYING, ZS_DEAD };

struct Node { int16_t x, y, z; };  // a cell a mob can stand in: block coords of its feet

struct Zombie
{
    int state;
    int target;          // player sprite it's after, or -1
    int lastSeen;        // emine tic it last saw the target
    int cooldown, flinch, attackPose, repath, stuck, deadTics;
    int kx, ky;          // knockback velocity, units/tic << 8
    std::vector<Node> path;
    size_t step;         // next path node
};

std::unordered_map<int, Zombie> zombies;
int mob_tic;
int mob_debug;

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

// two cells of room (a zombie is taller than a block) and something solid underfoot
bool standable(int bx, int by, int bz)
{
    return openAt(bx, by, bz) && openAt(bx, by, bz + 1) && solidAt(bx, by, bz - 1) && !lavaAt(bx, by, bz - 1);
}

// From standing cell n into the column (nx, ny): where would it end up? Same level, one up (a hop: needs headroom
// above where it stands), or down up to three (walking off an edge).
bool stepInto(Node const &n, int nx, int ny, int *nz)
{
    if (standable(nx, ny, n.z)) { *nz = n.z; return true; }
    if (standable(nx, ny, n.z + 1) && openAt(n.x, n.y, n.z + 2)) { *nz = n.z + 1; return true; }
    if (!openAt(nx, ny, n.z) || !openAt(nx, ny, n.z + 1)) return false;
    for (int dz = 1; dz <= 3; dz++)
    {
        if (standable(nx, ny, n.z - dz)) { *nz = n.z - dz; return true; }
        if (!openAt(nx, ny, n.z - dz)) return false;
    }
    return false;
}

uint32_t nodeKey(int x, int y, int z) { return (uint32_t)(x & 0xFFF) | ((uint32_t)(y & 0xFFF) << 12) | ((uint32_t)(z & 0xFF) << 24); }

// A* over standing cells. Fills `out` with the path from `from` to `to`, or, if `to` can't be reached within
// maxNodes, to the reachable cell nearest it (Minecraft does the same: a partial path). Returns whether it got there.
bool findPath(Node from, Node to, int maxNodes, std::vector<Node> &out)
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

    for (int expanded = 0; !open.empty() && expanded < maxNodes;)
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
                if (!standable(nx, ny, n.z) || !openAt(n.x + d[0], n.y, n.z) || !openAt(n.x + d[0], n.y, n.z + 1)
                    || !openAt(n.x, n.y + d[1], n.z) || !openAt(n.x, n.y + d[1], n.z + 1))
                    continue;
                nz = n.z;
            }
            else if (!stepInto(n, nx, ny, &nz))
                continue;

            float const cost = g + (diag ? 1.414f : 1.f) + (nz > n.z ? 0.5f : 0.f) + (waterAt(nx, ny, nz) ? 2.f : 0.f);
            uint32_t const nk = nodeKey(nx, ny, nz);
            auto const it = recs.find(nk);
            if (it != recs.end() && (it->second.closed || it->second.g <= cost)) continue;
            Node const nn = { (int16_t)nx, (int16_t)ny, (int16_t)nz };
            recs[nk] = { cost, k, nn, false };
            open.push({ cost + h(nn), nk });
        }
    }

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

// The CON action label -> its offset in the script (actions say which tiles a pose uses and how fast it animates).
int32_t actionOfs(char const *name)
{
    for (int i = 0; i < g_labelCnt; i++)
        if ((labeltype[i] & LABEL_ACTION) && !Bstrcmp(label + (i << 6), name))
            return labelcode[i];
    return 0;
}

struct ZombieActions { int32_t walk, stand, hit, dying, dead; };
ZombieActions const &zombieActions(void)
{
    static ZombieActions za;
    static int32_t forScript = -1;
    if (forScript != g_scriptSize)
    {
        za = { actionOfs("APIGWALK"), actionOfs("APIGSTAND"), actionOfs("APIGHIT"), actionOfs("APIGDYING"), actionOfs("APIGDEAD") };
        forScript = g_scriptSize;
    }
    return za;
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

bool seesPlayer(spritetype const &s, DukePlayer_t const *ps)
{
    return cansee(s.x, s.y, s.z - ZB_EYE, s.sectnum, ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum);
}

int32_t dist3(spritetype const &s, int32_t x, int32_t y, int32_t z)
{
    float const dx = (float)(x - s.x), dy = (float)(y - s.y), dz = (float)(z - s.z) * (1.f / 16.f);
    return (int32_t)sqrtf(dx * dx + dy * dy + dz * dz);
}

void hitPlayer(int i, DukePlayer_t *ps)
{
    static int const dmg[4] = { 12, 15, 18, 22 };  // per Duke skill: Minecraft Easy 2.5, Normal 3, Hard 4.5 of 20
    int const d = dmg[clamp(ud.player_skill - 1, 0, 3)];
    auto &ps_s = sprite[ps->i];
    if (!ud.god)
        ps_s.extra = max(0, ps_s.extra - d);
    P_PalFrom(ps, 32, 32, 0, 0);
    A_PlaySound(DUKE_GRUNT, ps->i);
    int const a = getangle(ps->pos.x - sprite[i].x, ps->pos.y - sprite[i].y);
    ps->vel.x += sintable[(a + 512) & 2047] * 60;  // ~0.6 blocks (Duke friction 0.81/tic)
    ps->vel.y += sintable[a & 2047] * 60;
}

void setTarget(Zombie &z, int playerSprite)
{
    if (z.target != playerSprite) z.path.clear();
    z.target = playerSprite;
    z.lastSeen = mob_tic;
}

// Minecraft's "alert others": zombies near one that got hurt turn on the attacker too.
void alertOthers(int i, int playerSprite)
{
    auto const &s = sprite[i];
    for (int j = headspritestat[STAT_ACTOR]; j >= 0; j = nextspritestat[j])
    {
        if (j == i || sprite[j].picnum != PIGCOP || sprite[j].extra <= 0) continue;
        auto const it = zombies.find(j);
        if (it == zombies.end() || it->second.state != ZS_LIVE || it->second.target >= 0) continue;
        if (FindDistance2D(sprite[j].x - s.x, sprite[j].y - s.y) > ZB_SIGHT || klabs(sprite[j].z - s.z) > 10 * BZ) continue;
        setTarget(it->second, playerSprite);
        if (mob_debug) OSD_Printf("emine_mobs: zombie %d: alerted by %d\n", j, i);
    }
}

void die(int i, Zombie &z, int weapon, DukePlayer_t *ps)
{
    auto &s = sprite[i];
    P_AddKills(ps, 1);
    z.state = ZS_DYING;
    z.path.clear();
    s.cstat &= ~257;  // the body doesn't block or catch shots
    if (krand() & 1) s.cstat ^= 4;
    A_PlaySound(ZB_SND_DYING, i);
    setAction(i, zombieActions().dying);
    (void)weapon;
}

void hurt(int i, Zombie &z, int weapon, DukePlayer_t *ps)
{
    auto &s = sprite[i];
    A_Spawn(i, BLOOD);
    if (s.extra <= 0)
    {
        if (weapon == RADIUSEXPLOSION || weapon == RPG)
        {
            P_AddKills(ps, 1);
            A_PlaySound(SQUISHED, i);
            A_DoGuts(i, JIBS6, 8);
            z.state = ZS_DEAD;
            z.deadTics = ZB_CORPSE;  // delete now
            return;
        }
        die(i, z, weapon, ps);
        return;
    }
    A_PlaySound(ZB_SND_PAIN, i);
    z.flinch = 6;
    // knocked away from the attacker, with a little hop
    int const from = (s.owner >= 0 && s.owner < MAXSPRITES) ? s.owner : ps->i;
    int const a = getangle(s.x - sprite[from].x, s.y - sprite[from].y);
    z.kx = (sintable[(a + 512) & 2047] * ZB_KNOCK) >> 6;
    z.ky = (sintable[a & 2047] * ZB_KNOCK) >> 6;
    if (s.zvel == 0) { s.z -= 512; s.zvel = -ZB_HOP; }
    if (playerAlive(ps))
    {
        setTarget(z, ps->i);
        alertOthers(i, ps->i);
    }
}

// Pick a random spot within ZB_STROLL blocks to walk to.
void startStroll(int i, Zombie &z)
{
    auto const &s = sprite[i];
    Node from;
    if (!cellOf(s.x, s.y, s.z, &from)) return;
    Node to = { (int16_t)(from.x + (krand() % (2 * ZB_STROLL + 1)) - ZB_STROLL),
                (int16_t)(from.y + (krand() % (2 * ZB_STROLL + 1)) - ZB_STROLL), from.z };
    for (int dz = 3; dz >= -7; dz--)
        if (standable(to.x, to.y, from.z + dz)) { to.z = from.z + dz; break; }
    findPath(from, to, 200, z.path);
    z.step = 1;
}

// ---------------------------------------------------------------------------------------------------------------

int zombieThink(int i, int playerNum)
{
    auto &s = sprite[i];
    auto const t = actor[i].t_data;
    auto const ps = g_player[playerNum].ps;
    auto const &za = zombieActions();

    if (t[9] != ZB_MAGIC)
    {
        t[9] = ZB_MAGIC;
        zombies[i] = Zombie{ ZS_LIVE, -1, 0, 0, 0, 0, krand() & 15, 0, 0, 0, 0, {}, 0 };
        s.extra = ZB_HEALTH;
        s.hitag = 0;  // no CON move flags
        setAction(i, za.stand);
    }
    Zombie &z = zombies[i];

    int const weapon = A_IncurDamage(i);

    if (z.state != ZS_LIVE)
    {
        A_Fall(i);
        if (weapon == RADIUSEXPLOSION || weapon == RPG)  // blow the body apart
        {
            A_PlaySound(SQUISHED, i);
            A_DoGuts(i, JIBS6, 8);
            z.deadTics = ZB_CORPSE;
        }
        if (z.state == ZS_DYING && AC_ACTION_COUNT(t) >= 5)
        {
            z.state = ZS_DEAD;
            setAction(i, za.dead);
            A_PlaySound(THUD, i);
            if ((krand() & 3) == 0) A_Spawn(i, BLOODPOOL);
        }
        if (z.state == ZS_DEAD && ++z.deadTics > ZB_CORPSE)
        {
            if (z.deadTics == ZB_CORPSE + 1) A_Spawn(i, SMALLSMOKE);
            zombies.erase(i);
            return 2;
        }
        return 1;
    }

    if (weapon >= 0)
    {
        hurt(i, z, weapon, ps);
        if (z.state != ZS_LIVE)
            return z.deadTics >= ZB_CORPSE ? (zombies.erase(i), 2) : 1;
    }

    // --- targeting
    bool sees = false;
    int32_t const pdist = playerAlive(ps) ? dist3(s, ps->pos.x, ps->pos.y, sprite[ps->i].z) : INT32_MAX;
    if (z.target < 0)
    {
        if (pdist < ZB_SIGHT && (mob_tic + i) % 15 == 0 && seesPlayer(s, ps))
        {
            setTarget(z, ps->i);
            sees = true;
            if (mob_debug) OSD_Printf("emine_mobs: zombie %d: sees you at %d blocks\n", i, pdist / BXY);
        }
    }
    else
    {
        sees = playerAlive(ps) && seesPlayer(s, ps);
        if (mob_debug >= 2 && mob_tic % 10 == 0)
            OSD_Printf("emine_mobs: zombie %d: sees %d  me %d,%d,%d sect %d eye-block %d  you %d,%d,%d sect %d  vox %d\n", i, sees, (int)s.x, (int)s.y,
                       (int)s.z, (int)s.sectnum, voxworld_blockat(s.x, s.y, s.z - ZB_EYE), ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                       (int)voxworld_cansee(s.x, s.y, s.z - ZB_EYE, ps->pos.x, ps->pos.y, ps->pos.z));
        if (sees) z.lastSeen = mob_tic;
        if (!playerAlive(ps) || pdist > ZB_SIGHT || mob_tic - z.lastSeen > ZB_MEMORY)
        {
            if (mob_debug) OSD_Printf("emine_mobs: zombie %d: lost you\n", i);
            z.target = -1;
        }
    }

    // --- where to go
    int32_t gx = s.x, gy = s.y;
    bool go = false, direct = false;
    Node here;
    bool const inWorld = cellOf(s.x, s.y, s.z, &here);
    int32_t const hdist = playerAlive(ps) ? FindDistance2D(ps->pos.x - s.x, ps->pos.y - s.y) : INT32_MAX;
    int32_t const pfeet = playerAlive(ps) ? sprite[ps->i].z : 0;
    bool const level = playerAlive(ps) && klabs(pfeet - s.z) < BZ * 3 / 2;

    if (z.target >= 0)
    {
        if (sees && hdist < 2 * BXY && level)
            direct = true;  // close and in plain sight: straight at them
        else if (inWorld && ((sees && --z.repath <= 0) || z.path.empty() || z.stuck > 15))
        {
            Node to;
            cellOf(ps->pos.x, ps->pos.y, pfeet, &to);
            bool const got = findPath(here, to, 800, z.path);
            z.step = 1;
            z.stuck = 0;
            z.repath = (hdist > 16 * BXY) ? 20 : 10;
            if (mob_debug) OSD_Printf("emine_mobs: zombie %d: path %d nodes%s\n", i, (int)z.path.size(), got ? "" : " (partial)");
        }
        if (direct) { gx = ps->pos.x; gy = ps->pos.y; go = hdist > ZB_STOP; }
    }
    else if (z.path.empty() || z.step >= z.path.size())
    {
        z.path.clear();
        if (inWorld && krand() % ZB_WANDER == 0) startStroll(i, z);
    }

    bool hopUp = false;
    if (!direct && z.step < z.path.size())
    {
        vec3_t const w = nodeFeet(z.path[z.step]);
        if (FindDistance2D(w.x - s.x, w.y - s.y) < 160)
        {
            z.step++;
            if (z.step >= z.path.size() && z.target < 0) z.path.clear();
        }
        if (z.step < z.path.size())
        {
            vec3_t const w2 = nodeFeet(z.path[z.step]);
            gx = w2.x; gy = w2.y; go = true;
            hopUp = z.path[z.step].z > here.z && FindDistance2D(w2.x - s.x, w2.y - s.y) < BXY;
        }
    }

    // --- move
    bool const onGround = s.z >= actor[i].floorz - (2 << 8);
    if (go)
    {
        int const want = getangle(gx - s.x, gy - s.y);
        int const d = G_GetAngleDelta(s.ang, want);
        s.ang = (s.ang + clamp(d, -ZB_TURN, ZB_TURN)) & 2047;
    }
    int32_t vx = z.kx >> 8, vy = z.ky >> 8;
    if (go && z.flinch == 0)
    {
        int const v = ZB_SPEED;
        vx += (sintable[(s.ang + 512) & 2047] * v) >> 14;
        vy += (sintable[s.ang & 2047] * v) >> 14;
    }
    z.kx = z.kx * 2 / 3;
    z.ky = z.ky * 2 / 3;
    if (vx | vy)
    {
        vec2_t const before = s.xy;
        int16_t const ang = s.ang;
        int const r = A_MoveSprite(i, { vx * 2, vy * 2, 0 }, CLIPMASK0);  // (A_MoveSprite moves half of what it's given)
        s.ang = ang;  // (A_MoveSprite turns enemies that hit a wall)
        int32_t const moved = FindDistance2D(s.x - before.x, s.y - before.y);
        bool const blocked = (r & 49152) == 32768 || moved < ZB_SPEED / 3;
        z.stuck = (go && blocked) ? z.stuck + 1 : 0;
        // blocked by a step it could climb: hop
        if (go && blocked && onGround && inWorld)
        {
            int bx, by, bz;
            voxworld_toblock(s.x + (sintable[(s.ang + 512) & 2047] >> 5), s.y + (sintable[s.ang & 2047] >> 5), s.z - 16, &bx, &by, &bz);
            if (solidAt(bx, by, bz) && openAt(bx, by, bz + 1) && openAt(bx, by, bz + 2) && openAt(here.x, here.y, here.z + 2))
                hopUp = true;
        }
    }
    if (hopUp && onGround && s.zvel == 0)
    {
        s.z -= 512;
        s.zvel = -ZB_JUMP;
    }
    A_Fall(i);

    // Duke lets you stand on a monster whose head is within step height (one a block lower than you, say); in
    // Minecraft you can't. Slide the player off.
    if (playerAlive(ps) && hdist < 512)
    {
        int32_t const top = s.z - (tilesiz[s.picnum].y * s.yrepeat << 2);
        int32_t const feet = sprite[ps->i].z;
        if (feet <= top + 1024 && feet >= top - 2048)
        {
            int const a = (hdist > 8) ? getangle(ps->pos.x - s.x, ps->pos.y - s.y) : s.ang;
            ps->vel.x += sintable[(a + 512) & 2047] * 24;
            ps->vel.y += sintable[a & 2047] * 24;
        }
    }

    // --- attack
    if (z.cooldown > 0) z.cooldown--;
    if (z.target >= 0 && z.cooldown == 0 && hdist <= ZB_REACH && level && sees)
    {
        hitPlayer(i, ps);
        z.cooldown = ZB_COOLDOWN;
        z.attackPose = 10;
        if (mob_debug) OSD_Printf("emine_mobs: zombie %d: hit you (health %d)\n", i, (int)sprite[ps->i].extra);
    }

    // --- look
    if (z.flinch > 0) { z.flinch--; setAction(i, za.hit); }
    else if (z.attackPose > 0) { z.attackPose--; setAction(i, za.stand); }
    else setAction(i, go ? za.walk : za.stand);

    if ((krand() & 255) == 0)
        A_PlaySound((krand() & 1) ? ZB_SND_ROAM : ZB_SND_ROAM2, i);

    return 1;
}

int osdcmd_emine_mobs(osdcmdptr_t parm)
{
    if (parm->numparms >= 1) { mob_debug = Batol(parm->parms[0]); return OSDCMD_OK; }
    auto const ps = g_player[myconnectindex].ps;
    for (auto const &kv : zombies)
    {
        auto const &s = sprite[kv.first];
        auto const &z = kv.second;
        OSD_Printf("emine_mobs: zombie %d: %s hp %d target %d path %d/%d dist %d blocks\n", kv.first,
                   z.state == ZS_LIVE ? (z.target >= 0 ? "chasing" : (z.path.empty() ? "idle" : "strolling")) : "dead",
                   (int)s.extra, z.target, (int)z.step, (int)z.path.size(), FindDistance2D(ps->pos.x - s.x, ps->pos.y - s.y) / BXY);
    }
    return OSDCMD_OK;
}

}  // namespace

int Emine_ActorThink(int spriteNum, int playerNum)
{
    auto &s = sprite[spriteNum];
    if (!voxworld_active() || s.sectnum != voxworld_sector())
        return 0;
    if (s.picnum == PIGCOPSTAYPUT || s.picnum == PIGCOPDIVE)
        s.picnum = PIGCOP;
    if (s.picnum == PIGCOP)
        return zombieThink(spriteNum, playerNum);
    return 0;
}

void Emine_MobTick(void) { mob_tic++; }

void Emine_RegisterMobOSD(void)
{
    OSD_RegisterFunction("emine_mobs", "emine_mobs [debug 0/1]: list the zombies and what they're doing; 1 = log their decisions",
                         osdcmd_emine_mobs);
}

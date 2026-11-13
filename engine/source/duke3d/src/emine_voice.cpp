// eMine32: Duke's voice, our lines. The lines come from the voice sheet (art/voice/lines.json) via tools/mkvoice.py,
// which writes game/sounds/voice/<take>.wav and takes.txt ("<take> <line>" per row). Here:
//
//  - Duke's own voice slots (DUKE_KILLED1, JIBBED_ACTOR3, ...) play our takes, so the triggers Duke's code still has
//    (dying, idling, landing hard, ...) say our lines. Every Duke voice slot we don't reuse plays a moment of
//    silence: none of his old lines can play.
//  - the moments Duke never had (meeting each monster, the first cave, each ore and food, ...) get new sounds, and
//    the game calls Emine_Say() where they happen.
//
// Talk sounds never overlap (the engine's own rule for Duke's talk); a line that can't play because Duke is already
// talking doesn't count as said, so a first-time line waits for its next chance.
//
// Copyright (C) 2026 the eMine32 project. GPLv2.

#include "duke3d.h"
#include "voxworld.h"
#include "emine.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace
{

int constexpr FIRST_ID = 2000;  // our sounds go here (Duke's end well below)

struct Line { std::vector<int> ids; int said = 0, last = -1; };
std::unordered_map<std::string, Line> lines;
std::unordered_map<std::string, int> takeId;   // take name -> sound id
bool ready;

// Duke's voice slots (soundefs.h) and the take each plays now; "" = silence (the line is cut).
struct Slot { int id; const char *take; };
Slot const dukeSlots[] = {
    { 195, "start" },           // BONUS_SPEECH1: New Game
    { 196, "ready" },           // BONUS_SPEECH2
    { 197, "" }, { 199, "" },   // BONUS_SPEECH3/4: end-of-level bonus screen (cut)
    { 215, "idle-crack" },      // DUKE_CRACK_FIRST
    { 33, "idle-1" },           // DUKE_CRACK
    { 208, "idle-2" },          // DUKE_CRACK2
    { 209, "use-nothing" },     // DUKE_SEARCH
    { 207, "use-nothing_2" },   // DUKE_SEARCH2
    { 38, "hurt" },             // DUKE_GRUNT
    { 211, "burning" }, { 274, "burning_2" }, { 275, "burning_3" }, { 276, "burning_4" },  // DUKE_LONGTERM_PAIN1-4
    { 304, "burning" }, { 305, "burning_2" }, { 306, "burning_3" }, { 307, "burning_4" },  // DUKE_LONGTERM_PAIN5-8
    { 200, "land-hurt" },       // DUKE_LAND_HURT
    { 245, "fall-scream" },     // DUKE_SCREAM
    { 25, "gasp" },             // DUKE_GASP
    { 37, "death-1" }, { 203, "death-2" }, { 72, "death-3" }, { 271, "death-4" },  // DUKE_KILLED1/2/3/5
    { 270, "death-gibbed" }, { 41, "death-gibbed" },  // DUKE_KILLED4, DUKE_DEAD
    { 216, "medkit" },          // DUKE_USEMEDKIT
    { 222, "gib-1" }, { 223, "gib-2" }, { 224, "gib-3" }, { 225, "gib-4" }, { 226, "gib-5" }, { 227, "gib-6" },
    { 228, "gib-7" }, { 265, "gib-8" }, { 266, "gib-9" }, { 267, "gib-10" }, { 268, "gib-11" }, { 269, "gib-12" },
    { 263, "gib-13" }, { 390, "" }, { 391, "" },  // JIBBED_ACTOR1-15
    { 206, "weapon-1" }, { 107, "weapon-2" }, { 78, "weapon-3" }, { 221, "weapon-4" }, { 264, "weapon-6" },  // DUKE_GETWEAPON*
    { 229, "heal-low" },        // DUKE_GOTHEALTHATLOW
    { 218, "relief" },          // DUKE_PISSRELIEF
    { 273, "happens" },         // DUKE_STEPONFECES: "Shift happens", on the title screen
    { 217, "gulp" },            // DUKE_TAKEPILLS
    // cut: toilets, drinking, strippers, mirrors, bosses, lines placed in Duke's maps
    { 28, "" }, { 36, "" }, { 202, "" }, { 67, "" }, { 201, "" }, { 214, "" }, { 250, "" }, { 252, "" },
    { 56, "" }, { 251, "" }, { 239, "" }, { 240, "" }, { 189, "" }, { 190, "" }, { 192, "" }, { 193, "" },
    { 177, "" }, { 302, "" }, { 254, "" },
};

std::string voiceFile(char const *take) { return std::string("sounds/voice/") + (take && *take ? take : "silence") + ".wav"; }

int pick(Line &l)
{
    int k = krand() % (int)l.ids.size();
    if ((int)l.ids.size() > 1 && k == l.last) k = (k + 1) % (int)l.ids.size();
    l.last = k;
    return l.ids[k];
}

bool voiceLog;  // `emine_voice log`

bool play(Line &l)
{
    if (l.ids.empty() || S_TalkPlaying()) return false;
    int const id = pick(l);
    if (S_PlaySound(id) < 0) return false;
    l.said++;
    if (voiceLog) OSD_Printf("emine_voice: says %s\n", g_sounds[id]->filename);
    return true;
}

// --- what the voice keeps track of in play
struct Seen { bool cave, deep, lava, water, peak, mushroom, melon; } seen;
int16_t ammoKnown[MAX_WEAPONS];  // -1: never had any yet
int lastHealth = -1, lowHealthTic = -100000, outOfAmmoTic = -100000, lastAmmo = -1, lastWeapon = -1;
bool died;
int blastTic = -100000; vec3_t blastAt;
int killTics[8]; int killHead;
int voice_tic;

bool nearVisible(DukePlayer_t *ps, int type, int r, int rz)
{
    int px, py, pz;
    if (!voxworld_toblock(ps->pos.x, ps->pos.y, ps->pos.z, &px, &py, &pz)) return false;
    for (int dx = -r; dx <= r; dx++)
        for (int dy = -r; dy <= r; dy++)
            for (int dz = -rz; dz <= rz; dz++)
                if (voxworld_get(px + dx, py + dy, pz + dz) == type)
                {
                    vec3_t c;
                    voxworld_blockcenter(px + dx, py + dy, pz + dz, &c);
                    // look at the near side of the block, so the block itself doesn't hide it
                    vec3_t const t = { c.x + (ps->pos.x > c.x ? 300 : -300), c.y + (ps->pos.y > c.y ? 300 : -300), c.z - 2048 };
                    if (voxworld_cansee(ps->pos.x, ps->pos.y, ps->pos.z, t.x, t.y, t.z)) return true;
                }
    return false;
}

}  // namespace

bool Emine_Say(const char *line, int oneIn)
{
    if (!ready) return false;
    auto it = lines.find(line);
    if (it == lines.end()) return false;
    Line &l = it->second;
    if (l.said) { if (oneIn <= 0 || krand() % oneIn) return false; }
    return play(l);
}

bool Emine_SayChance(const char *prefix, int oneIn)
{
    if (!ready || krand() % max(1, oneIn)) return false;
    // every line whose name starts with prefix: one of them, at random
    std::vector<Line *> pool;
    size_t const n = strlen(prefix);
    for (auto &kv : lines)
        if (!kv.first.compare(0, n, prefix) && !kv.second.ids.empty()) pool.push_back(&kv.second);
    if (pool.empty()) return false;
    return play(*pool[krand() % pool.size()]);
}

void Emine_VoiceFoley(const char *take)
{
    auto it = takeId.find(take);
    if (it != takeId.end()) S_PlaySound(it->second);
}

void Emine_VoiceInit(void)
{
    // our takes, as tools/mkvoice.py made them
    buildvfs_kfd const fil = kopen4load("sounds/voice/takes.txt", 0);
    if (fil == buildvfs_kfd_invalid) { LOG_F(WARNING, "eMine32: no voice lines (sounds/voice/takes.txt)"); return; }
    int32_t const len = kfilelength(fil);
    std::string text(len, '\0');
    kread(fil, &text[0], len);
    kclose(fil);
    int id = FIRST_ID;
    for (size_t p = 0; p < text.size();)
    {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string row = text.substr(p, e - p);
        p = e + 1;
        if (!row.empty() && row.back() == '\r') row.pop_back();
        size_t const sp = row.find(' ');
        if (sp == std::string::npos) continue;
        std::string const take = row.substr(0, sp), line = row.substr(sp + 1);
        bool const foley = take == "gulp" || take == "idle-crack";
        S_DefineSound(id, voiceFile(take.c_str()).c_str(), 0, 0, foley ? 128 : 250, foley ? 0 : SF_TALK, 0, 1.f);
        takeId[take] = id;
        lines[line].ids.push_back(id);
        id++;
    }
    // Duke's slots: ours, or silence
    for (auto const &s : dukeSlots)
    {
        char const *take = (s.take[0] && takeId.count(s.take)) ? s.take : "";
        if ((unsigned)s.id < MAXSOUNDS && g_sounds[s.id] != &nullsound)
            S_SetSoundFile(s.id, voiceFile(take).c_str());
    }
    for (auto &w : ammoKnown) w = -1;
    ready = true;
    LOG_F(INFO, "eMine32: %d voice takes for %d lines", id - FIRST_ID, (int)lines.size());
}

void Emine_VoiceLevelStart(void)
{
    if (died)
    {
        died = false;
        if (krand() & 1) { if (!Emine_Say("respawn")) Emine_Say("ready"); }
        else if (!Emine_Say("ready")) Emine_Say("respawn");
    }
    else
        Emine_Say("level-start");
    lastHealth = -1; lastAmmo = -1; lastWeapon = -1;
}

void Emine_VoiceKill(int type, bool explosive, bool burnt, bool fused)
{
    // three explosive kills within half a second: one line for the lot
    if (explosive)
    {
        killTics[killHead++ & 7] = voice_tic;
        int recent = 0;
        for (int t : killTics) recent += t && voice_tic - t < 15;
        if (recent >= 3 && Emine_Say("multi-kill")) { for (int &t : killTics) t = 0; return; }
    }
    if (fused && Emine_Say("creeper-kill", 2)) return;
    if (burnt && Emine_Say("cooked", 3)) return;
    if (explosive) { Emine_SayChance("gib-", 3); return; }
    static char const *const kill[] = { "zombie-kill", "skeleton-kill", "spider-kill", nullptr };
    if ((unsigned)type < 3) Emine_Say(kill[type], 4);
}

void Emine_VoiceBlast(vec3_t const &at) { blastTic = voice_tic; blastAt = at; }

void Emine_VoiceAte(int kind, int before, int after, int maxHealth)
{
    Emine_VoiceFoley("gulp");
    if (before < maxHealth * 3 / 10 && Emine_Say("heal-low", 2)) return;
    static char const *const first[] = { "eat-melon-first", "eat-apple-first", "eat-raw-first", "eat-cooked-first", "eat-mushroom-first" };
    if ((unsigned)kind < 5)
    {
        // raw meat has a second line, mixed in once the first has been heard
        if (kind == EMINE_FOOD_MEAT && lines[first[kind]].said && (krand() & 1)) { if (Emine_Say("eat-raw", 4)) return; }
        else if (Emine_Say(first[kind], 4)) return;
    }
    if (after >= maxHealth && before < maxHealth - 20) Emine_Say("relief", 3);
}

void Emine_VoiceOre(int type)
{
    char const *line = nullptr;
    switch (type)
    {
        case VW_COAL: line = "ore-coal"; break;
        case VW_IRON: line = "ore-iron"; break;
        case VW_GOLD: line = "ore-gold"; break;
        case VW_REDSTONE: line = "ore-redstone"; break;
        case VW_LAPIS: line = "ore-lapis"; break;
        case VW_DIAMOND: line = "ore-diamond"; break;
        default: return;
    }
    if (!Emine_Say(line, 0)) Emine_SayChance("ore-any", 5);
}

void Emine_VoiceTick(void)
{
    voice_tic++;
    auto const ps = g_player[myconnectindex].ps;
    if (!ready || !voxworld_active() || ps->i < 0) return;
    int const health = sprite[ps->i].extra;

    // dying: by a creeper's blast, its own line (it comes first, so Duke's death line keeps quiet)
    if (health <= 0)
    {
        if (!died && voice_tic - blastTic < 30) Emine_Say("death-creeper");
        died = true;
        return;
    }
    if (voice_tic - blastTic == 25 && FindDistance2D(ps->pos.x - blastAt.x, ps->pos.y - blastAt.y) < 6 * VW_BLOCKXY)
        Emine_Say("creeper-boom", 2);

    // low health: once on the way down, at most every 45 s
    if (lastHealth >= 25 && health < 25 && voice_tic - lowHealthTic > 45 * 30 && Emine_Say("low-health")) lowHealthTic = voice_tic;
    lastHealth = health;

    // ammo: the first time each weapon has some, and running dry
    for (int w = 0; w < MAX_WEAPONS; w++)
    {
        int const a = ps->ammo_amount[w];
        // (what Duke starts with doesn't count; a line that can't play yet tries again next tic)
        if (ammoKnown[w] < 0 && a > 0 && (voice_tic <= 1 || Emine_SayChance("weapon-", 1))) ammoKnown[w] = 1;
    }
    int const cur = ps->curr_weapon, ca = ps->ammo_amount[cur];
    if (cur == lastWeapon && lastAmmo > 0 && ca == 0 && cur != KNEE_WEAPON && voice_tic - outOfAmmoTic > 30 * 30 && Emine_Say("out-of-ammo", 2))
        outOfAmmoTic = voice_tic;
    lastWeapon = cur; lastAmmo = ca;

    if (voice_tic % 15) return;
    // places and finds, the first time
    int bx, by, bz;
    if (!voxworld_toblock(ps->pos.x, ps->pos.y, ps->pos.z, &bx, &by, &bz)) return;
    int fx, fy, fz;
    voxworld_toblock(ps->pos.x, ps->pos.y, sprite[ps->i].z - 256, &fx, &fy, &fz);
    if (!seen.cave && voxworld_lightlevel(bx, by, bz) <= 3) seen.cave = Emine_Say("cave-first", 0);
    if (!seen.deep && bz <= 16) seen.deep = Emine_Say("cave-deep", 0);
    if (!seen.water && Emine_InWaterAt(sprite[ps->i].xyz)) seen.water = Emine_Say("water-first", 0);
    int const under = voxworld_get(fx, fy, fz - 1);
    if (!seen.peak && (under == VW_SNOW || under == VW_SNOWBLOCK)) seen.peak = Emine_Say("peak", 0);
    if (voice_tic % 30) return;
    if (!seen.lava && nearVisible(ps, VW_LAVA, 6, 3)) seen.lava = Emine_Say("lava-first", 0);
    else if (!seen.mushroom && nearVisible(ps, VW_MUSHROOM, 5, 2)) seen.mushroom = Emine_Say("mushroom-first", 0);
    else if (!seen.melon && nearVisible(ps, VW_MELON, 7, 2)) seen.melon = Emine_Say("melon-first", 0);
}

void Emine_VoiceReset(void)
{
    seen = {};
    for (auto &kv : lines) kv.second.said = 0;
    for (auto &w : ammoKnown) w = -1;
}

// Dev console: `emine_voice` lists the lines and checks Duke's voice slots (any still on a file of Duke's is named);
// `emine_voice <line>` says that line now (ignoring "first time" bookkeeping); `emine_voice log` prints each line as
// it's said in play.
static int osdcmd_emine_voice(osdcmdptr_t parm)
{
    if (parm->numparms >= 1 && !Bstrcasecmp(parm->parms[0], "log"))
    {
        voiceLog = !voiceLog;
        OSD_Printf("emine_voice: logging %s\n", voiceLog ? "on" : "off");
        return OSDCMD_OK;
    }
    if (parm->numparms >= 1)
    {
        auto it = lines.find(parm->parms[0]);
        if (it == lines.end()) { OSD_Printf("emine_voice: no line %s\n", parm->parms[0]); return OSDCMD_OK; }
        int const id = pick(it->second);
        int const v = S_PlaySound(id);
        OSD_Printf("emine_voice: %s -> sound %d (%s): %s\n", parm->parms[0], id, g_sounds[id]->filename, v >= 0 ? "playing" : "did not play");
        return OSDCMD_OK;
    }
    int takes = 0;
    for (auto const &kv : lines) takes += (int)kv.second.ids.size();
    OSD_Printf("emine_voice: %d lines, %d takes\n", (int)lines.size(), takes);
    int ours = 0, silent = 0, old = 0;
    for (auto const &s : dukeSlots)
    {
        if ((unsigned)s.id >= MAXSOUNDS || g_sounds[s.id] == &nullsound) continue;
        char const *fn = g_sounds[s.id]->filename;
        if (!fn || Bstrncmp(fn, "sounds/voice/", 13)) { old++; OSD_Printf("emine_voice: slot %d still plays %s\n", s.id, fn ? fn : "(none)"); }
        else if (Bstrstr(fn, "silence")) silent++;
        else ours++;
    }
    OSD_Printf("emine_voice: Duke's voice slots: %d ours, %d silenced, %d old\n", ours, silent, old);
    return OSDCMD_OK;
}

void Emine_RegisterVoiceOSD(void)
{
    OSD_RegisterFunction("emine_voice", "emine_voice [line]: list our voice lines and check Duke's slots, or say a line", osdcmd_emine_voice);
}

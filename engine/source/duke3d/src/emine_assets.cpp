// eMine32: what of Duke's does the game still use? For the launch, nothing of DUKE3D.GRP may ship, so this keeps a
// tally of every tile drawn that still comes from the GRP's ART files, every sound played that's still one of
// Duke's VOC files, every actor whose CON script ran, and every quote shown. `emine_assets` in the console prints
// the lists (`emine_assets clear` starts over); play a while first, the more of the game the better.

#include "duke3d.h"
#include "emine.h"

namespace
{
uint8_t seenSound[bitmap_size(MAXSOUNDS)];
uint8_t seenActor[bitmap_size(MAXTILES)];
uint8_t seenQuote[bitmap_size(MAXQUOTES)];

bool isDukeFile(char const *fn)
{
    if (!fn) return false;
    size_t const n = Bstrlen(fn);
    return n > 4 && !Bstrcasecmp(fn + n - 4, ".voc");
}

int osdcmd_assets(osdcmdptr_t parm)
{
    if (parm->numparms > 0 && !Bstrcasecmp(parm->parms[0], "clear"))
    {
        Bmemset(emine_seenpic, 0, sizeof(emine_seenpic));
        Bmemset(seenSound, 0, sizeof(seenSound));
        Bmemset(seenActor, 0, sizeof(seenActor));
        Bmemset(seenQuote, 0, sizeof(seenQuote));
        OSD_Printf("emine_assets: cleared\n");
        return OSDCMD_OK;
    }

    int n = 0;
    OSD_Printf("emine_assets: Duke's tiles drawn (from the GRP's ART):\n");
    for (int i = 0; i < MAXTILES; i++)
        if (bitmap_test(emine_seenpic, i) && !bitmap_test(faketile, i) && tilefilenum[i] < 200 /* MAXARTFILES_BASE (engine_priv.h): TILES000-199.ART */
            && tilesiz[i].x > 0 && tilesiz[i].y > 0)
            OSD_Printf("  tile %d %dx%d\n", i, tilesiz[i].x, tilesiz[i].y), n++;
    OSD_Printf("  (%d tiles)\nemine_assets: Duke's sounds played:\n", n);
    n = 0;
    for (int i = 0; i < MAXSOUNDS && i < g_highestSoundIdx + 1; i++)
        if (bitmap_test(seenSound, i) && g_sounds[i] && isDukeFile(g_sounds[i]->filename))
            OSD_Printf("  sound %d %s\n", i, g_sounds[i]->filename), n++;
    OSD_Printf("  (%d sounds)\nemine_assets: actors whose CON script ran:\n", n);
    n = 0;
    for (int i = 0; i < MAXTILES; i++)
        if (bitmap_test(seenActor, i))
            OSD_Printf("  actor %d\n", i), n++;
    OSD_Printf("  (%d actors)\nemine_assets: quotes shown:\n", n);
    n = 0;
    for (int i = 0; i < MAXQUOTES; i++)
        if (bitmap_test(seenQuote, i))
            OSD_Printf("  quote %d %s\n", i, apStrings[i] ? apStrings[i] : ""), n++;
    OSD_Printf("  (%d quotes)\n", n);
    return OSDCMD_OK;
}
}

void Emine_AssetSound(int num) { if ((unsigned)num < MAXSOUNDS) bitmap_set(seenSound, num); }
void Emine_AssetActor(int pic) { if ((unsigned)pic < MAXTILES) bitmap_set(seenActor, pic); }
void Emine_AssetQuote(int q)   { if ((unsigned)q < MAXQUOTES) bitmap_set(seenQuote, q); }

void Emine_RegisterAssetsOSD(void)
{
    OSD_RegisterFunction("emine_assets", "emine_assets [clear]: Duke's tiles, sounds, actor scripts and quotes the game has used",
                         osdcmd_assets);
}

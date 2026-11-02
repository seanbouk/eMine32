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

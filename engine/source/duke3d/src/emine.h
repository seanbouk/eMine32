// eMine32: Duke-side glue for the block world (see source/build/include/voxworld.h).
// Copyright (C) 2026 the eMine32 project. GPLv2.

#pragma once

#ifndef emine_h_
#define emine_h_

#include "build.h"

// A hitscan shot (Duke weapons, enemy fire) landed; if it hit a block, damage it. shotSprite carries the damage
// in its `extra` (as Duke's shotsparks do).
void Emine_HitscanBlock(int shotSprite, const hitdata_t *hit);
// After a shot's hitscan: smash the plants it went through, within reach (xy distance; 0 = no limit).
void Emine_ResetWeapons(int playerNum);  // every weapon from the start; ammo comes from ores
void Emine_ShotPlants(int shooterSprite, const vec3_t *start, int32_t reach);

// An explosion (A_RadiusDamage): blast blocks within the radius.
void Emine_ExplosionBlocks(int spriteNum, int blastRadius, int maxDamage);

// Floor height for falling debris etc.: the nearest block top below, else the sector floor.
int32_t Emine_FloorZ(int spriteNum, int32_t sectorFloorZ);

// A projectile about to move by (dx, dy, dz): if a block is in the way, stop it there and return a wall-hit code
// (Build moves projectiles across then up/down, which can cut the corner of a block). 0 = the way is clear.
int32_t Emine_ProjectileBlocks(int spriteNum, int32_t dx, int32_t dy, int32_t dz);

// Extra shade from the block world's lighting where this sprite stands (0 outside the block world / in daylight).
int Emine_LightShade(int spriteNum);

// Console commands and world event hooks (called once from registerosdcommands).
void Emine_RegisterOSD(void);

// Once per game tic (G_MoveWorld): falling blocks etc.
void Emine_Tick(void);

// Block water. Is this point in water?
bool Emine_InWaterAt(vec3_t const &pos);
// The sector lotag the player's movement should use: ST_2_UNDERWATER (swimming) while their body is in block
// water, else `lotag` unchanged. Also gives a hop out of the water when jump is held as they surface.
int  Emine_PlayerLotag(int playerNum, int lotag, uint32_t playerBits);
// After Duke's swimming: with no swim keys held, sink slowly (to standing on the bottom).
void Emine_SwimSink(int playerNum, uint32_t playerBits, int32_t floorZ);

#endif

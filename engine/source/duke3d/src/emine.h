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
// frame timing for play-testing (emine_perf.cpp): our systems, timed inside a tic
#define EMINE_PERF_SYSTEMS 5
enum { EMINE_PERF_MOBS, EMINE_PERF_WORLD, EMINE_PERF_FOOD, EMINE_PERF_VOICE, EMINE_PERF_OTHER };
void Emine_PerfSystem(int sys, double ms);
void Emine_PerfMouse(int mousex);
void Emine_PerfTics(int tics, double ms);
void Emine_PerfFrame(double drawms, double waitms, int q16ang);
void Emine_PerfPart(int part, double ms);  // 0 input, 1 world, 2 HUD and weapon, 3 present, 4 sound
void Emine_RegisterPerfOSD(void);

// Once per game tic (G_MoveWorld): falling blocks etc.
void Emine_Tick(void);

// Minecraft-style mobs (emine_mobs.cpp). Called by A_Execute before an actor's CON code: 0 = not ours (run the
// CON), 1 = handled, 2 = handled and the actor should be deleted.
int  Emine_ActorThink(int spriteNum, int playerNum);
void Emine_MobTick(void);
void Emine_MobsLevelStart(void);  // clears Duke's map-placed enemies and our mob state on the next tic
int Emine_MobTile(int spriteNum, int tile);  // the tile to draw for a mob (creepers draw from their own range)
void Emine_RegisterMobOSD(void);

// Food (emine_food.cpp): pickups that heal, melons that grow back, mushrooms in the dark.
enum { EMINE_FOOD_MELON, EMINE_FOOD_APPLE, EMINE_FOOD_MEAT, EMINE_FOOD_COOKED, EMINE_FOOD_MUSHROOM, EMINE_FOOD_COUNT };
void Emine_FoodDrop(int kind, vec3_t const &at, int owner);  // a pickup pops out at `at` and bounces to rest
void Emine_FoodBlockBroken(int bx, int by, int bz, int type, int owner);  // leaves may drop an apple, melons slices
void Emine_FoodTick(void);
void Emine_FoodLevelStart(void);
void Emine_RegisterFoodOSD(void);
// A dropped sprite at height z: pop it up a little and let it fall and bounce to rest on the blocks (emine.cpp).
void Emine_Bounce(int spriteNum, int32_t z);

// Voice (emine_voice.cpp): our lines in Duke's voice slots and for the moments Duke never had.
void Emine_VoiceInit(void);                       // once the game script has defined Duke's sounds
void Emine_VoiceTick(void);
void Emine_VoiceLevelStart(void);                 // play starts (or restarts after dying)
void Emine_VoiceReset(void);
// Say a line (an id from the voice sheet): always the first time (once Duke isn't already talking), then 1 in
// oneIn (0: never again). True if it played.
bool Emine_Say(const char *line, int oneIn = 1);
// 1 in oneIn: one of the lines whose ids start with prefix ("gib-").
bool Emine_SayChance(const char *prefix, int oneIn);
void Emine_VoiceFoley(const char *take);          // a non-speech take ("gulp")
void Emine_VoiceKill(int mobType, bool explosive, bool burnt, bool fused);  // the player killed a mob
void Emine_VoiceBlast(vec3_t const &at);          // a creeper went off
void Emine_VoiceAte(int kind, int before, int after, int maxHealth);
void Emine_VoiceOre(int blockType);               // an ore broke
void Emine_RegisterVoiceOSD(void);
bool Emine_VoiceGate(int soundNum);  // the sound engine asks before playing: false = wait (a gap after spoken lines)

// Block water. Is this point in water?
bool Emine_InWaterAt(vec3_t const &pos);
// The sector lotag the player's movement should use: ST_2_UNDERWATER (swimming) while their body is in block
// water, else `lotag` unchanged. Also gives a hop out of the water when jump is held as they surface.
int  Emine_PlayerLotag(int playerNum, int lotag, uint32_t playerBits);
// After Duke's swimming: with no swim keys held, sink slowly (to standing on the bottom).
void Emine_SwimSink(int playerNum, uint32_t playerBits, int32_t floorZ);

#endif

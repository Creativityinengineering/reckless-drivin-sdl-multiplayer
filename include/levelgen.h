#ifndef __LEVELGEN
#define __LEVELGEN

#include "compat.h"

/* Level generator (multiplayer menu): a level built from a seed and the
   track settings. The same code always gives the same level, on every
   computer. */

enum { kGenLandscapes = 10, kGenChoices = 4 };

typedef struct {
	UInt32 seed;
	UInt8 landscape;	/* one of the original levels' worlds, 0..9 */
	UInt8 length, curves, width, traffic, speed, cops, jumps, time;	/* 0..3 each */
	UInt8 crossings;	/* 0..3: loops where the road crosses itself on a bridge */
	UInt8 forks;	/* 0..3: side roads that leave the road and join it again */
	UInt8 highways;	/* 0..3: stretches on a highway with its own fast traffic */
} tLevelGen;

#define kLevelCodeSize 15	/* "XXXX-XXXX-XXXX" and the terminator */
#define kMaxSavedLevels 12

/* Settings of the multiplayer menu, kept in the preferences folder. */
extern tLevelGen gLevelGen;
extern int gLevelGenOn;	/* the next multiplayer game uses generated levels */

void LevelGen_LoadSettings(void);
void LevelGen_SaveSettings(void);
int LevelGen_SavedCount(void);
int LevelGen_GetSaved(int index, tLevelGen *gen);
void LevelGen_AddSaved(const tLevelGen *gen);
void LevelGen_DeleteSaved(int index);

void LevelGen_Encode(const tLevelGen *gen, char code[kLevelCodeSize]);
int LevelGen_Decode(const char *code, tLevelGen *gen);	/* 1 if valid */
UInt32 LevelGen_Pack(const tLevelGen *gen);	/* the settings without the seed */
void LevelGen_Unpack(UInt32 packed, UInt32 seed, tLevelGen *gen);

const char *LevelGen_LandscapeName(int landscape);
const char *LevelGen_ChoiceName(int setting, int value);	/* setting: 0 length .. 7 time, 8 crossings, 9 forks, 10 highways */

/* Builds level number levelIndex of this generator into the game's level
   data (road, tracks, marks, objects). Returns 0 if it cannot. */
int LevelGen_Build(const tLevelGen *gen, int levelIndex);
void LevelGen_Dispose(void);
int LevelGen_IsLoaded(void);
/* Lanes per direction of the loaded generated level (1 = ordinary road). */
extern int gGenLanes;
/* Sideways offset from the traffic track for a car's lane at height y. */
float LevelGen_LaneOffset(int lane, float x, float y);
/* The road of the first level for a map in the menu (no level loaded):
   the road edges at columns from start to finish, where it has a median,
   and the ramps as fractions of the length. */
int LevelGen_Preview(const tLevelGen *gen, int columns, float *left, float *right,
	UInt8 *divided, float *ramps, int maxRamps, int *rampCount, float *bridges, int *bridgeCount);
/* With crossings: the free-form road for the menu's map (every few points
   of the center line, and where it is a bridge); the number of points. */
int LevelGen_PreviewRoute(const tLevelGen *gen, float *xs, float *ys, UInt8 *bridge, int max);
/* Overpasses of the loaded generated level: their heights on the road. */
int LevelGen_GetBridges(const float **heights);

#endif

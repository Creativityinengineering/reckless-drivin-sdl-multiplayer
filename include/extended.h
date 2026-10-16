#ifndef __EXTENDED_H
#define __EXTENDED_H

#include "objects.h"

/* Extended mode crates (multiplayer): both hit the race leader. */
void Extended_SpawnCopSwarm(tObject *target);
void Extended_SpawnTrafficJam(tObject *target);
void Extended_AddTraffic(void);
void Extended_AdjustPackages(int amount);
void Extended_SpawnCops(tObject *target,int count);
int Extended_CountCopsNear(const tObject *target);
void Extended_Panic(tObject *car);
void Extended_Crash(tObject *a,tObject *b);
int Extended_PanicTurn(tObject *car,tInputData *input);

#endif

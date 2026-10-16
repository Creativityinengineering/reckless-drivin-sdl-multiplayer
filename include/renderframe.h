#ifndef __RENDERFRAME
#define __RENDERFRAME

#include "player.h"

#define kMaxZoomVelo	80.0
#define kInvLines		0

void RenderInit();
void RenderFrame();
int DrawPanel();
float GetComplSequenceLength(const tPlayer *player);

#endif
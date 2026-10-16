#ifndef __HIGH
#define __HIGH

#include "compat.h"

void CheckHighScore(UInt32 score);
void CheckHighScoreWithoutDisplay(UInt32 score);
int CheckHighScoreForPlayer(UInt32 score, const char *name);
int GetHighScoreRank(const char *name);
void ClearHighScores();
void ShowHighScores(int hilite);
	
#endif
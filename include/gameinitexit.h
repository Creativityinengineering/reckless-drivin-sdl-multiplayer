#ifndef __GAMEINITEXIT
#define __GAMEINITEXIT

extern int gLevelID;
extern int gGameOn;
extern int gPlayerCarID;
extern int gStartLevel;

void DisposeLevel();
void StartGame(int);
void ChoosePlayerVehicles(int count);
int Vehicle_Count(void);
int Vehicle_ID(int index);
int Vehicle_IndexOf(int id);
const char *Vehicle_Name(int id);
int LoadLevel();
void EndGame();
void SwapAllRoadInfo(void);

#endif
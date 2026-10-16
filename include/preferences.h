#ifndef __PREFERENCES
#define __PREFERENCES

#include "compat.h"
#include "input.h"

#define kNumHighScoreEntrys	10
#define kPrefsVersion 6
#define kLocalPlayerCount 4
#define kLocalBindableCount 8

typedef struct{
	Str15 name;
	UInt32 time;
	UInt32 score;
}tScoreRecord;

typedef struct{
	UInt16 version;
	UInt16 volume;
	UInt8  sound,engineSound,hqSound,skidSound;
	UInt8  lineSkip,motionBlur,hiColor;
	UInt8 hidElements[kNumElements];
	UInt8  unused[11]; /* bytes 0-1: window width (LE), 2-3: window height (LE), 4: fullscreen, 8: multiplayer game mode (co-op, duel, packages, police), 9: original game mode, 10: bit 0 LAN own screen */
	tScoreRecord	high[kNumHighScoreEntrys];
	float lapRecords[10];
	Str255 name,code;
	UInt8 keyCodes[kNumElements];
	Str255 lastName;
	UInt8 localPlayerCount;
	UInt8 localKeyCodes[kLocalPlayerCount][kLocalBindableCount];
	Str15 playerNames[kLocalPlayerCount];
}tPrefs;

extern tPrefs gPrefs;
void Preferences();	
void LoadPrefs();
void WritePrefs(int reset);
int Prefs_IsExtendedMode(void);
int Prefs_IsLanOwnScreen(void);
void Prefs_ToggleLanScreen(void);

#endif
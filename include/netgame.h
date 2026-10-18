#ifndef __NETGAME
#define __NETGAME

#include "objects.h"
#include "levelgen.h"

#define kNetworkMaxPlayers kMaxPlayers

int Network_InitHost(unsigned short port);

#define kMaxLanHosts 8
typedef struct {
	char address[16];
	char name[16];
	int players, limit, started;
	UInt32 token;
	UInt64 lastSeen;
} tLanHost;
int Network_StartDiscovery(unsigned short port);
int Network_PollDiscovery(tLanHost *hosts, int max);
void Network_StopDiscovery(void);
int Network_GetLocalAddress(char *address, size_t size);
void Network_SetLocalVehicle(int vehicle);
int Network_GetLocalVehicle(void);
int Network_IsSlotActive(int slot);
const char *Network_GetSlotName(int slot);
void Network_SetLocalName(const char *name);
int Network_TakeHostLevel(void);
int Network_GetLevelGen(tLevelGen *gen);	/* 1: build generated levels */

int Network_InitClient(const char *address, unsigned short port);
void Network_SetLobbyPlayerLimit(int count);
void Network_EnableLocalTwoPlayer(void);
void Network_EnableLocalPlayers(int count);
void Network_EndLocalGame(void);
int Network_IsLocalTwoPlayer(void);
int Network_IsLocalMultiplayer(void);
int Network_GetLocalDriverSlot(const tObject *object);
void Network_SetDriverButtons(const tObject *object, int fire, int missile);
void Network_SetPlayerVehicle(int slot, int vehicle);
int Network_GetPlayerVehicle(int slot);
int Network_GetLocalSlot(void);
int Network_IsLocalPlayer2(const tObject *object);
int Network_GetLocalInput(const tObject *object, tInputData *input);
void Network_LevelLoaded(void);
void Network_Shutdown(void);
void Network_Poll(void);
void Network_Publish(void);
void Network_SetGameStarted(void);
void Network_GameEnded(void);
void Network_StartCampaign(void);
void Network_BindDriver(tObject *object);
void Network_UnbindDriver(void);
int Network_GetBoundSlot(void);
void Network_ReplaceDriverObject(tObject *oldObject, tObject *newObject);
void Network_UpdateDriverButtons(const tObject *object);
int Network_AllDriversEliminated(void);
void Network_PrepareNextLevel(void);
tObject *Network_GetPoliceTarget(const tObject *cop);
UInt32 Network_GetPlayerAddOns(const tObject *object);
float Network_GetPlayerDeathDelay(const tObject *object);
int Network_GetLocalDriverCount(void);
tObject *Network_GetLocalDriverObject(int index);
int Network_IsPrimaryDriver(const tObject *object);
int Network_IsDriverObject(const tObject *object);
int Network_DriverIsFinished(const tObject *object);
int Network_DriverLights(int slot);
int Network_DriverIsEliminated(const tObject *object);
void Network_MarkDriverFinished(tObject *object);
void Network_MarkDriverEliminated(tObject *object);
int Network_UpdateRaceProgress(void);
#define kDuelKillScore 1000

int Network_UnfinishedDriverCount(void);
int Network_IsDuel(void);
int Network_GetDriverPlace(const tObject *object);
int Network_GetFinishedDriverCount(void);
float Network_NearestDriverDistance(float y);
float Network_NearestDriverDistance2D(t2DPoint pos);
void Network_GetDriverRange(float *minY, float *maxY);
int Network_GetPlacementBonus(int place);
int Network_IsTrailingDriver(const tObject *object);
tObject *Network_GetSpectateTarget(const tObject *owner);
void Network_UpdateSpectators(void);
void Network_KillDriver(tObject *object);
void Network_KillObjectFor(tObject *victim, tObject *killer);
void Network_RecordShot(const tObject *shooter, const tObject *projectile,
	int weapon);
const char *Network_GetGiftTargetName(const tObject *donor);
int Network_GiveLife(const tObject *donor);
void Network_HitDriver(tObject *victim, const tObject *shooter);
void Network_ProcessEvents(void);
enum { kCrateCopSwarm, kCrateTrafficJam };
int Network_TriggerCrate(const tObject *collector, int crate);
int Network_IsLocalDriver(const tObject *object);
const char *Network_GetLocalPlayerName(int index);
int Network_GetLocalScoreCount(void);
UInt32 Network_GetLocalScore(int index);
int Network_IsActive(void);
int Network_IsClient(void);
int Network_GameHasStarted(void);
int Network_GetPlayerCount(void);
const char *Network_GetPlayerName(const tObject *object);
tObject *Network_GetCameraForView(int viewIndex);
int Network_IsRemoteObject(const tObject *object);
void Network_SyncObjects(void);
void Network_ResetLevel(void);
void Network_PlaceDriverApart(tObject *driver);
void Network_PrepareRaceClock(void);
int Network_RaceHeld(void);
int Network_AdvanceRaceClock(void);
int Network_IsPackageMode(void);
/* Multiplayer game modes, chosen with M in the multiplayer menu. */
enum { kGameModeCoop, kGameModeDuel, kGameModePackages, kGameModePolice, kGameModeZombie,
	kGameModeWildlife,
	kGameModeCount };
int Network_GetGameMode(void);
void Network_SetSoloModes(int enabled);
void Network_UpdatePoliceMode(void);
int Network_HasAggressiveCops(void);
int Network_SwarmHunts(const tObject *driver);
int Network_DriverRamming(const tObject *object);
int Network_RainHits(const tObject *object);
int Network_IsZombieTarget(const tObject *driver);
tObject *Network_ClearRoadDriver(const tObject *car);
int Network_WarActive(void);
void Network_MarkHit(tObject *object, const tObject *driver);
void Network_CreditKill(tObject *victim, int score);

/* Crate settings of the multiplayer menu (K): which pickups exist and how
   many packages lie on the road, in every multiplayer mode. Bits follow the
   add-on choices of BonusObject; the host's settings apply on a LAN. */
enum {
	kCrateKindLock, kCrateKindMines, kCrateKindMissiles, kCrateKindSpikes,
	kCrateKindJammer, kCrateKindTurbo, kCrateKindPoints, kCrateKindLife,
	kCrateKindGrip, kCrateKindOffroad, kCrateKindMagnet, kCrateKindShield,
	kCrateKindRam, kCrateKindOil, kCrateKindNitro, kCrateKindRain,
	kCrateKindZombie, kCrateKindSwarm, kCrateKindJam, kCrateKindWar, kCrateKindDodge,
	kCrateKindCount
};
enum { kPackagesFew, kPackagesNormal, kPackagesMore, kPackagesMany, kPackageAmountCount };
enum { kCrateOff, kCrateRare, kCrateNormal, kCrateOften, kCrateWeightCount };
#define kCrateThemeCount 11
/* Theme mode: off, random per level, or one theme (kThemeFixed + index). */
enum { kThemeOff, kThemeRandom, kThemeFixed };
typedef struct {
	const char *name;
	UInt8 weights[kCrateKindCount];
} tCrateTheme;
extern const char *const kCrateKindNames[kCrateKindCount];
extern const tCrateTheme kCrateThemes[kCrateThemeCount];
int Network_UsesCrateSettings(void);
int Network_GetCrateWeight(int kind);
int Network_GetPackageAmount(void);
int Network_GetLocalCrateWeight(int kind);
int Network_GetLocalPackageAmount(void);
int Network_GetLocalThemeMode(void);
int Network_GetThemeMode(void);
int Network_GetLocalThemeWeight(int theme, int kind);
void Network_SetLocalCrates(const UInt8 *weights, int amount, int themeMode,
	const UInt8 themeWeights[kCrateThemeCount][kCrateKindCount]);
void Network_PrepareCrates(void);
/* LAN world sync: the host simulates traffic, cops and pickups; clients
   show copies (replicas) of the objects near their drivers. */
void Network_MoveReplica(tObject *object);
void Network_ReplicaKilled(tObject *object);
void Network_ForgetReplica(tObject *object);

#endif

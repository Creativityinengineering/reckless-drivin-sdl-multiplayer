#include "netgame.h"
#include "renderframe.h"
#include "extended.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "gameinitexit.h"
#include "input.h"
#include "platform.h"
#include "preferences.h"
#include "sprites.h"
#include "objects.h"
#include "vec2d.h"
#include "gamesounds.h"
#include "textfx.h"
#include "packs.h"
#include "particlefx.h"
#include "random.h"
#include <stddef.h>

#ifdef _WIN32
typedef SOCKET NetworkSocket;
typedef int NetworkSockLen;
#define kInvalidSocket INVALID_SOCKET
#define NetworkCloseSocket closesocket
#else
typedef int NetworkSocket;
typedef socklen_t NetworkSockLen;
#define kInvalidSocket (-1)
#define NetworkCloseSocket close
#endif

#define kNetworkMagic 0x52444d50u
#define kNetworkVersion 18
#define kNetworkIntervalMS 50
#define kNetworkTimeoutMS 10000

static int GetSlotForPlayer(const tObject *object);
static int GetLeaderSlot(void);
static int IsRacing(int slot);
static int IsLocalSlot(int slot);
static int WarActive(void);
static int sThemeIndex;	/* defined with the crate settings */
static int BindNested(tObject *driver);
static void RestoreBinding(int previous);

enum {
	kPacketHello = 1,
	kPacketWelcome,
	kPacketState,
	kPacketLeave,
	kPacketDiscover,	/* broadcast by a joining client: who hosts here? */
	kPacketAnnounce,	/* a host's answer: name, players, lobby size */
	kPacketWorld		/* host: objects near a client's driver */
};

/* World sync. The host simulates traffic, cops, pickups and scenery and sends
   the objects near each client's driver 20 times a second; the client shows
   copies (replicas) and tells the host what it destroyed or picked up. */
#define kWorldChunk 48		/* objects per packet: stays below the MTU */
#define kWorldRange 1800.0f	/* road distance around a driver that is sent */
#define kReplicaTimeoutMS 400	/* vanished from the host's states: gone */
#define kReplicaFarTimeoutMS 1500
#define kKillHistory 16
#define kInterpDelayMS 100	/* states are shown this late, between two received ones */
#define kTombstones 32

typedef struct {
	UInt16 id;
	SInt16 typeRes;
	UInt8 seq, unused;
} tWorldKill;

typedef struct {
	UInt16 id;
	SInt16 typeRes, frame;
	UInt8 control, layer;
	float x, y;
	SInt16 dir, veloX, veloY, jumpHeight;	/* dir in 1/65536 turns, rest *32 */
	UInt16 damageFlags, damage;
} tNetObject;

typedef struct {
	UInt32 magic;
	UInt16 version, packetSize;
	UInt8 type, slot, count, level;
	UInt32 sequence, raceId;
	UInt32 timeMS;	/* host clock */
	tNetObject objects[kWorldChunk];
} tWorldPacket;

typedef struct {
	float x, y, dir, velocityX, velocityY;
	UInt32 timeMS;	/* sender's clock when the car was there */
	int vehicle, frame;	/* frame: undamaged sprite, dents via damageFlags */
	int damageFlags;
	float damage;
	UInt8 dents[9];	/* dentAmount of the car */
	UInt8 brake, reverse;	/* brake lights */
	float jumpHeight;
	UInt8 readyLevel;	/* level + 1 once loaded, for the common start */
	UInt8 shield;	/* crates the others see or feel */
	float ramTime, nitroTime, rainTime, zombieTime, warTime, dodgeTime;
	UInt8 active;
	UInt8 finished, eliminated;
	int lives, extraLives, score, displayScore, bonus;
	int missiles, mines;
	UInt32 addOns;
	float deathDelay, finishDelay, slide[4], spikeFrame;
	float finishTime;
	UInt8 finishPlace;
	UInt8 giftTarget, giftSeq;
	UInt8 creditTarget, creditSeq;
	UInt8 shotSeq;
	tShotEvent shots[kShotHistory];
	UInt8 swarmTarget, swarmSeq, jamTarget, jamSeq;
	int fire, missile;
	UInt8 awardSeq;	/* host: points for drivers on other computers */
	struct { UInt8 seq, target; UInt16 amount; float x, y; } awards[8];
	UInt8 killSeq;	/* client: host objects it destroyed */
	tWorldKill kills[kKillHistory];
	char name[16];
} tNetworkPlayer;

typedef struct {
	UInt32 magic;
	UInt16 version, packetSize;
	UInt8 type, slot, started, playerCount;
	UInt32 sequence, raceId;
	UInt8 level;
	UInt8 duel;
	UInt8 raceHold;	/* host's race clock stands (waiting, or everyone finished) */
	UInt8 packageAmount;	/* host's crate settings */
	SInt8 theme;	/* this level's random theme, -1 = none */
	UInt8 themeMode;	/* host's theme setting, shown in the client lobby */
	UInt8 crateWeights[kCrateKindCount];
	float raceTime;	/* host's race clock, negative during the countdown */
	tNetworkPlayer players[kNetworkMaxPlayers];
} tNetworkPacket;

typedef struct {
	struct sockaddr_in address;
	UInt64 lastSeen;
	int used;
} tNetworkPeer;

static NetworkSocket sSocket = kInvalidSocket;
static struct sockaddr_in sServerAddress;
static tNetworkPeer sPeers[kNetworkMaxPlayers];
static tNetworkPlayer sPlayers[kNetworkMaxPlayers];
static tObject *sRemoteObjects[kNetworkMaxPlayers];
static int sRemoteVehicles[kNetworkMaxPlayers];
static tObject *sLocalPlayer2;
static int sIsHost;
static int sLocalTwoPlayer;
static int sLocalPlayerCount = 2;
static int sLobbyPlayerLimit = kNetworkMaxPlayers;
static int sLocalSlot = -1;
static int sStarted;
static int sStartedByHost;
static int sStartPending;
static int sStartingFromNetwork;
static int sPendingLevel = -1;
#ifdef _WIN32
static int sWinsockInitialized;
#endif
static UInt32 sSequence;
static UInt32 sRaceId;
static UInt32 sLaunchedRace;
static UInt64 sLastSendMS;
static UInt64 sLastHelloMS;
static int sPlayerFire[kNetworkMaxPlayers];
static int sPlayerMissile[kNetworkMaxPlayers];
static int sBoundSlot = -1;
static int sLocalVehicle;	/* LAN: vehicle picked in the lobby */
static UInt64 sSnapshotMS[kNetworkMaxPlayers];	/* arrival of a driver's last state */
static UInt32 sHostToken;	/* identifies this host in LAN announcements */
static int sHostMode;	/* LAN client: the host's game mode */
static int sSoloModes;	/* multiplayer menu, one player: the modes apply too */
static int sHostPackageAmount = 1;
static UInt8 sSeenGiftSeq[kNetworkMaxPlayers];
static UInt8 sSeenCreditSeq[kNetworkMaxPlayers];
static UInt8 sSeenShotSeq[kNetworkMaxPlayers];
static UInt8 sSeenSwarmSeq[kNetworkMaxPlayers];
static UInt8 sSeenJamSeq[kNetworkMaxPlayers];
static UInt8 sSeenKillSeq[kNetworkMaxPlayers];
static UInt8 sSeenAwardSeq[kNetworkMaxPlayers];
static tObject *sReplicas[65536];	/* client: by netId */
static struct { UInt16 id; SInt16 typeRes; UInt64 timeMS; } sTombstones[kTombstones];
static int sNextTombstone;
/* Remote drivers' recent states, and each sender's clock relative to ours. */
static tNetSample sPlayerSamples[kNetworkMaxPlayers][kNetSamples];
static UInt8 sPlayerSampleHead[kNetworkMaxPlayers], sPlayerSampleCount[kNetworkMaxPlayers];
static float sClockOffset[kNetworkMaxPlayers];
/* Simulation time: advances one step per game frame, like the cars. Wall
   time would add the frame pacing's jitter to every remote car. */
static double sSimClockMS;
/* Common start: after loading, everyone waits for the slowest computer;
   then the host counts down and runs the race clock for all. */
#define kCountdownSeconds 3.0f
#define kStartWaitMS 15000	/* a computer that never gets ready is not waited for */
static float sHostRaceTime;
static double sHostRaceTimeAt;
static UInt8 sHostRaceKnown, sHostRaceHold, sRaceHolding;
static UInt64 sHoldStartMS;
static UInt8 sClockKnown[kNetworkMaxPlayers];
static tPlayer *sSavedCurPlayer;
static tObject *sSavedPlayer;
static tObject *sSavedCamera;
static int sSavedCarID;
static tObject *sSavedVisFirst;
static tObject *sSavedVisLast;

extern int gEndGame;

/* Copy a driver's gameplay state into its network record before sending. */
static void PackPlayer(int slot)
{
	tNetworkPlayer *state = &sPlayers[slot];
	const tPlayer *player = &gPlayers[slot];
	int i;
	state->lives = player->lives;
	state->extraLives = player->extraLives;
	state->score = player->score;
	state->displayScore = player->displayScore;
	state->bonus = player->bonus;
	state->missiles = player->missiles;
	state->mines = player->mines;
	state->addOns = player->addOns;
	state->deathDelay = player->deathDelay;
	state->finishDelay = player->finishDelay;
	state->finishTime = player->finishTime;
	state->finishPlace = (UInt8)player->finishPlace;
	state->giftTarget = player->giftTarget;
	state->giftSeq = player->giftSeq;
	state->creditTarget = player->creditTarget;
	state->creditSeq = player->creditSeq;
	state->shotSeq = player->shotSeq;
	state->swarmTarget = player->swarmTarget;
	state->swarmSeq = player->swarmSeq;
	state->jamTarget = player->jamTarget;
	state->jamSeq = player->jamSeq;
	memcpy(state->shots, player->shots, sizeof(state->shots));
	state->spikeFrame = player->spikeFrame;
	for (i = 0; i < 4; ++i)
		state->slide[i] = player->slide[i];
	state->shield = (UInt8)(player->shield || player->shieldGrace > 0);
	state->ramTime = player->ramTime;
	state->nitroTime = player->nitroTime;
	state->rainTime = player->rainTime;
	state->zombieTime = player->zombieTime;
	state->warTime = player->warTime;
	state->dodgeTime = player->dodgeTime;
}

/* Take over a driver's gameplay state from a received network record. */
static void UnpackPlayer(int slot)
{
	const tNetworkPlayer *state = &sPlayers[slot];
	tPlayer *player = &gPlayers[slot];
	int i;
	player->lives = state->lives;
	player->extraLives = state->extraLives;
	player->score = state->score;
	player->displayScore = state->displayScore;
	player->bonus = state->bonus;
	player->missiles = state->missiles;
	player->mines = state->mines;
	player->addOns = state->addOns;
	player->deathDelay = state->deathDelay;
	player->finishDelay = state->finishDelay;
	player->finishTime = state->finishTime;
	player->finishPlace = state->finishPlace;
	player->giftTarget = state->giftTarget;
	player->giftSeq = state->giftSeq;
	player->creditTarget = state->creditTarget;
	player->creditSeq = state->creditSeq;
	player->shotSeq = state->shotSeq;
	player->swarmTarget = state->swarmTarget;
	player->swarmSeq = state->swarmSeq;
	player->jamTarget = state->jamTarget;
	player->jamSeq = state->jamSeq;
	memcpy(player->shots, state->shots, sizeof(player->shots));
	player->spikeFrame = state->spikeFrame;
	for (i = 0; i < 4; ++i)
		player->slide[i] = state->slide[i];
	player->shield = state->shield;
	player->ramTime = state->ramTime;
	player->nitroTime = state->nitroTime;
	player->rainTime = state->rainTime;
	player->zombieTime = state->zombieTime;
	player->warTime = state->warTime;
	player->dodgeTime = state->dodgeTime;
}

extern int gFire, gMissile;

void Network_UpdateDriverButtons(const tObject *object)
{
	int slot = Network_GetLocalDriverSlot(object);
	if (slot < 0)
		return;
	gFire = sPlayerFire[slot];
	gMissile = sPlayerMissile[slot];
}

void Network_SetDriverButtons(const tObject *object, int fire, int missile)
{
	int slot = Network_GetLocalDriverSlot(object);
	if (slot < 0)
		return;
	sPlayerFire[slot] = fire;
	sPlayerMissile[slot] = missile;
	sPlayers[slot].fire = fire;
	sPlayers[slot].missile = missile;
}

static void InitializePlayerState(int slot)
{
	if (slot < 0 || slot >= kNetworkMaxPlayers)
		return;
	sPlayers[slot].active = 1;
	if (sPlayers[slot].vehicle <= 0)
		sPlayers[slot].vehicle = kNormalPlayerCarID;
	sPlayers[slot].lives = 3;
	sPlayers[slot].extraLives = 0;
	sPlayers[slot].score = 0;
	sPlayers[slot].displayScore = 0;
	sPlayers[slot].bonus = 1;
	sPlayers[slot].missiles = 0;
	sPlayers[slot].mines = 0;
	sPlayers[slot].addOns = 0;
	sPlayers[slot].deathDelay = 0;
	sPlayers[slot].finishDelay = 0;
	sPlayers[slot].finished = 0;
	sPlayers[slot].eliminated = 0;
	memset(sPlayers[slot].slide, 0, sizeof(sPlayers[slot].slide));
	sPlayers[slot].spikeFrame = 0;
	Player_ResetForCampaign(&gPlayers[slot]);
}

void Network_SetPlayerVehicle(int slot, int vehicle)
{
	if (slot < 0 || slot >= kNetworkMaxPlayers || vehicle <= 0)
		return;
	sPlayers[slot].vehicle = vehicle;
}

int Network_GetPlayerVehicle(int slot)
{
	if (slot < 0 || slot >= kNetworkMaxPlayers ||
		sPlayers[slot].vehicle <= 0)
		return kNormalPlayerCarID;
	return sPlayers[slot].vehicle;
}

int Network_GetLocalSlot(void)
{
	return sLocalSlot;
}

static UInt64 NetworkTimeMS(void)
{
	return Platform_GetMicroseconds() / 1000;
}

static void InitializePacket(tNetworkPacket *packet, int type)
{
	memset(packet, 0, sizeof(*packet));
	packet->magic = kNetworkMagic;
	packet->version = kNetworkVersion;
	packet->packetSize = (UInt16)sizeof(*packet);
	packet->type = (UInt8)type;
	packet->sequence = ++sSequence;
}

static int SendPacket(const struct sockaddr_in *address, const tNetworkPacket *packet)
{
	int sent = (int)sendto(sSocket, (const char *)packet, (int)sizeof(*packet), 0,
		(const struct sockaddr *)address, (int)sizeof(*address));
	if (sent != (int)sizeof(*packet)) {
		fprintf(stderr, "[network] Failed to send LAN packet\n");
		return 0;
	}
	return 1;
}

static int SetNonBlocking(NetworkSocket socketHandle)
{
#ifdef _WIN32
	u_long enabled = 1;
	if (ioctlsocket(socketHandle, FIONBIO, &enabled) != 0) {
		fprintf(stderr, "[network] Could not make UDP socket non-blocking: %d\n",
			WSAGetLastError());
		return 0;
	}
#else
	int flags = fcntl(socketHandle, F_GETFL, 0);
	if (flags < 0 || fcntl(socketHandle, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("[network] Could not make UDP socket non-blocking");
		return 0;
	}
#endif
	return 1;
}

static int OpenSocket(void)
{
#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		fprintf(stderr, "[network] Winsock initialization failed\n");
		return 0;
	}
	sWinsockInitialized = 1;
#endif
	sSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sSocket == kInvalidSocket) {
		fprintf(stderr, "[network] Could not create UDP socket\n");
#ifdef _WIN32
		WSACleanup();
		sWinsockInitialized = 0;
#endif
		return 0;
	}
	if (!SetNonBlocking(sSocket)) {
		NetworkCloseSocket(sSocket);
		sSocket = kInvalidSocket;
#ifdef _WIN32
		WSACleanup();
		sWinsockInitialized = 0;
#endif
		return 0;
	}
	return 1;
}

static int SameAddress(const struct sockaddr_in *left, const struct sockaddr_in *right)
{
	return left->sin_addr.s_addr == right->sin_addr.s_addr &&
		left->sin_port == right->sin_port;
}

static int FindPeer(const struct sockaddr_in *address)
{
	int slot;
	for (slot = 1; slot < kNetworkMaxPlayers; ++slot)
		if (sPeers[slot].used && SameAddress(&sPeers[slot].address, address))
			return slot;
	return -1;
}

UInt32 Network_GetPlayerAddOns(const tObject *object)
{
	int slot = GetSlotForPlayer(object);
	if (slot < 0)
		return object == gPlayerObj ? gCurPlayer->addOns : 0;
	return gPlayers[slot].addOns;
}

float Network_GetPlayerDeathDelay(const tObject *object)
{
	int slot = GetSlotForPlayer(object);
	if (slot < 0)
		return object == gPlayerObj ? gCurPlayer->deathDelay : 0;
	return gPlayers[slot].deathDelay;
}

tObject *Network_GetPoliceTarget(const tObject *cop)
{
	tObject *target = NULL;
	float bestDistance = INFINITY;
	int slot, leader;
	if (!Network_IsActive())
		return gPlayerObj;
	/* A cop from a swarm crate keeps hunting the driver it was sent after. */
	if (cop->chaseTarget) {
		int hunted = GetSlotForPlayer((const tObject *)cop->chaseTarget);
		if (hunted >= 0 && IsRacing(hunted) && gPlayers[hunted].lives > 0)
			return gPlayers[hunted].obj;
	}
	leader = GetLeaderSlot();
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		tObject *candidate = gPlayers[slot].obj;
		float dx, dy, distance;
		if (!candidate || !sPlayers[slot].active || sPlayers[slot].finished ||
			sPlayers[slot].eliminated || gPlayers[slot].lives <= 0)
			continue;
		dx = cop->pos.x - candidate->pos.x;
		dy = cop->pos.y - candidate->pos.y;
		distance = dx * dx + dy * dy;
		/* Cops go for the leader unless another driver is much closer. */
		if (slot == leader)
			distance *= 0.25f;
		if (distance < bestDistance) {
			bestDistance = distance;
			target = candidate;
		}
	}
	return target ? target : gPlayerObj;
}

static int AddPeer(const struct sockaddr_in *address)
{
	int slot;
	for (slot = 1; slot < sLobbyPlayerLimit; ++slot) {
		if (!sPeers[slot].used) {
			sPeers[slot].address = *address;
			sPeers[slot].lastSeen = NetworkTimeMS();
			sPeers[slot].used = 1;
			sPlayers[slot].active = 1;
			fprintf(stderr, "[network] Player joined in slot %d\n", slot + 1);
			return slot;
		}
	}
	return -1;
}

/* The name of this computer's driver: on a LAN the first player name. */
static void CopyLocalName(char name[16], int slot)
{
	int index = sLocalTwoPlayer ? slot : 0;
	int nameLength = gPrefs.playerNames[index][0];
	if (nameLength > 15)
		nameLength = 15;
	memset(name, 0, 16);
	memcpy(name, gPrefs.playerNames[index] + 1, (size_t)nameLength);
}

/* The lobby name field: the LAN driver name (first player name), sent to
   the others with the next hello or state. */
void Network_SetLocalName(const char *name)
{
	size_t length = strlen(name);
	if (length > 15)
		length = 15;
	gPrefs.playerNames[0][0] = (UInt8)length;
	memcpy(gPrefs.playerNames[0] + 1, name, length);
	if (!sLocalTwoPlayer && sLocalSlot >= 0 && sLocalSlot < kNetworkMaxPlayers)
		CopyLocalName(sPlayers[sLocalSlot].name, sLocalSlot);
}

static void ReadLocalPlayer(void)
{
	tObject *player = gPlayerObj;
	tNetworkPlayer *state;
	if (!player || sLocalSlot < 0 || sLocalSlot >= kNetworkMaxPlayers)
		return;
	state = &sPlayers[sLocalSlot];
	state->x = player->pos.x;
	state->y = player->pos.y;
	state->dir = player->dir;
	state->velocityX = player->velo.x;
	state->velocityY = player->velo.y;
	state->timeMS = (UInt32)sSimClockMS;
	state->vehicle = gRoadInfo && gRoadInfo->water ?
		kNormalPlayerBoatID : gPlayerCarID;
	/* A damaged car's sprite is a local copy the others do not have: send
	   the undamaged one and where the dents are. */
	state->frame = SpriteBaseFrame(player->frame) ? SpriteBaseFrame(player->frame) :
		player->type->frame;
	state->damageFlags = player->damageFlags;
	state->damage = player->damage;
	memcpy(state->dents, player->dentAmount, sizeof(state->dents));
	state->brake = (UInt8)(player->input.brake != 0);
	state->reverse = (UInt8)(player->input.reverse != 0);
	state->jumpHeight = player->jumpHeight;
	state->active = 1;
	PackPlayer(sLocalSlot);
	CopyLocalName(state->name, sLocalSlot);
}

static void SendHostState(void)
{
	tNetworkPacket packet;
	int slot;
	InitializePacket(&packet, kPacketState);
	packet.slot = (UInt8)sLocalSlot;
	packet.started = (UInt8)sStartedByHost;
	packet.raceId = sRaceId;
	packet.level = (UInt8)gLevelID;
	packet.duel = (UInt8)Network_GetGameMode();	/* duel: the game mode */
	packet.raceTime = gGameTime;
	{
		int kind;
		for (kind = 0; kind < kCrateKindCount; ++kind)
			packet.crateWeights[kind] = (UInt8)Network_GetCrateWeight(kind);
		packet.theme = (SInt8)sThemeIndex;
		packet.themeMode = (UInt8)Network_GetLocalThemeMode();
	}
	packet.packageAmount = (UInt8)Network_GetLocalPackageAmount();
	packet.raceHold = sRaceHolding;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		packet.players[slot] = sPlayers[slot];
		if (packet.players[slot].active)
			packet.playerCount++;
	}
	for (slot = 1; slot < kNetworkMaxPlayers; ++slot)
		if (sPeers[slot].used)
			SendPacket(&sPeers[slot].address, &packet);
}

int Network_InitHost(unsigned short port)
{
	struct sockaddr_in address;
	if (sSocket != kInvalidSocket)
		Network_Shutdown();	/* a session left open would keep the port */
	if (!OpenSocket())
		return 0;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	if (bind(sSocket, (struct sockaddr *)&address, sizeof(address)) < 0) {
		fprintf(stderr, "[network] Could not listen on UDP port %u\n", port);
		Network_Shutdown();
		return 0;
	}
	sIsHost = 1;
	sHostToken = (UInt32)SDL_GetPerformanceCounter() | 1;
	sLocalSlot = 0;
	sPlayers[0].active = 1;
	sPlayers[0].vehicle = Network_GetLocalVehicle();
	CopyLocalName(sPlayers[0].name, 0);
	sPeers[0].used = 1;
	sLobbyPlayerLimit = kNetworkMaxPlayers;
	fprintf(stderr, "[network] Hosting LAN game on UDP port %u\n", port);
	return 1;
}

void Network_SetLobbyPlayerLimit(int count)
{
	if (count < 2 || count > kNetworkMaxPlayers) {
		fprintf(stderr, "[network] Lobby player limit must be between 2 and %d\n",
			kNetworkMaxPlayers);
		return;
	}
	sLobbyPlayerLimit = count;
}

int Network_InitClient(const char *address, unsigned short port)
{
	if (sSocket != kInvalidSocket)
		Network_Shutdown();
	if (!address || !OpenSocket())
		return 0;
	memset(&sServerAddress, 0, sizeof(sServerAddress));
	sServerAddress.sin_family = AF_INET;
	sServerAddress.sin_port = htons(port);
	if (inet_pton(AF_INET, address, &sServerAddress.sin_addr) != 1) {
		fprintf(stderr, "[network] Invalid IPv4 address: %s\n", address);
		Network_Shutdown();
		return 0;
	}
	sLocalSlot = -1;
	fprintf(stderr, "[network] Joining %s:%u\n", address, port);
	return 1;
}

/* ---- LAN discovery: find hosts without typing their address ---- */

static NetworkSocket sDiscoverySocket = kInvalidSocket;
static unsigned short sDiscoveryPort;
static tLanHost sLanHosts[kMaxLanHosts];
static int sLanHostCount;
static UInt64 sLastDiscoverMS;

int Network_StartDiscovery(unsigned short port)
{
	int enabled = 1;
#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return 0;
#endif
	sDiscoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sDiscoverySocket == kInvalidSocket) {
#ifdef _WIN32
		WSACleanup();
#endif
		return 0;
	}
	setsockopt(sDiscoverySocket, SOL_SOCKET, SO_BROADCAST,
		(const char *)&enabled, sizeof(enabled));
	if (!SetNonBlocking(sDiscoverySocket)) {
		Network_StopDiscovery();
		return 0;
	}
	sDiscoveryPort = port;
	sLanHostCount = 0;
	sLastDiscoverMS = 0;
	return 1;
}

static void SendDiscoverTo(UInt32 address)
{
	tNetworkPacket packet;
	struct sockaddr_in to;
	InitializePacket(&packet, kPacketDiscover);
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(sDiscoveryPort);
	to.sin_addr.s_addr = address;
	sendto(sDiscoverySocket, (const char *)&packet, (int)sizeof(packet), 0,
		(const struct sockaddr *)&to, (int)sizeof(to));
}

/* Many WLAN routers, mesh systems and guest networks drop broadcasts
   between devices while letting direct traffic through. Every other
   search round therefore also asks each address of the local networks
   directly (up to a /22, about 1000 addresses). */
static void SendDiscoverToSubnets(void)
{
#ifndef _WIN32
	struct ifaddrs *interfaces, *entry;
	if (getifaddrs(&interfaces) != 0)
		return;
	for (entry = interfaces; entry; entry = entry->ifa_next) {
		UInt32 address, mask, network, host, count;
		if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
			!entry->ifa_netmask || !(entry->ifa_flags & IFF_UP) ||
			(entry->ifa_flags & IFF_LOOPBACK))
			continue;
		address = ntohl(((struct sockaddr_in *)entry->ifa_addr)->sin_addr.s_addr);
		mask = ntohl(((struct sockaddr_in *)entry->ifa_netmask)->sin_addr.s_addr);
		if (mask < 0xFFFFFC00u)
			mask = 0xFFFFFF00u;	/* bigger networks: just the own /24 */
		network = address & mask;
		count = ~mask;
		for (host = 1; host < count; ++host)
			SendDiscoverTo(htonl(network | host));	/* own address too: a host here */
	}
	freeifaddrs(interfaces);
#endif
}

static void SendDiscover(void)
{
	static int round;
	if (round++ % 2 == 0)
		SendDiscoverToSubnets();
	SendDiscoverTo(htonl(INADDR_BROADCAST));
	SendDiscoverTo(htonl(INADDR_LOOPBACK));	/* a host on this computer */
#ifndef _WIN32
	{
		/* Each network's own broadcast address too: 255.255.255.255 only
		   leaves through the default interface. */
		struct ifaddrs *interfaces, *entry;
		if (getifaddrs(&interfaces) == 0) {
			for (entry = interfaces; entry; entry = entry->ifa_next)
				if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET &&
					(entry->ifa_flags & IFF_BROADCAST) && (entry->ifa_flags & IFF_UP) &&
					entry->ifa_broadaddr)
					SendDiscoverTo(((struct sockaddr_in *)entry->ifa_broadaddr)->sin_addr.s_addr);
			freeifaddrs(interfaces);
		}
	}
#endif
}

int Network_PollDiscovery(tLanHost *hosts, int max)
{
	UInt64 now = NetworkTimeMS();
	tNetworkPacket packet;
	struct sockaddr_in sender;
	int i, count;
	if (sDiscoverySocket == kInvalidSocket)
		return 0;
	if (!sLastDiscoverMS || now - sLastDiscoverMS >= 1000) {
		SendDiscover();
		sLastDiscoverMS = now;
	}
	for (;;) {
		NetworkSockLen senderLength = (NetworkSockLen)sizeof(sender);
		char address[16];
		tLanHost *host = NULL;
		int received = (int)recvfrom(sDiscoverySocket, (char *)&packet,
			(int)sizeof(packet), 0, (struct sockaddr *)&sender, &senderLength);
		if (received < 0)
			break;
		if (received != (int)sizeof(packet) || packet.magic != kNetworkMagic ||
			packet.version != kNetworkVersion || packet.type != kPacketAnnounce)
			continue;
		inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address));
		/* A host on several networks answers once per network: one entry. */
		for (i = 0; i < sLanHostCount; ++i)
			if (sLanHosts[i].token == packet.raceId) {
				host = &sLanHosts[i];
				if (strcmp(host->address, "127.0.0.1") != 0)
					snprintf(address, sizeof(address), "%s", host->address);
			}
		/* A host on this computer answers on loopback and on its LAN
		   address; keep one entry and prefer the LAN address. */
		if (!host && strcmp(address, "127.0.0.1") == 0 && sLanHostCount)
			continue;
		if (!host)
			for (i = 0; i < sLanHostCount; ++i)
				if (strcmp(sLanHosts[i].address, "127.0.0.1") == 0)
					host = &sLanHosts[i];
		if (!host && sLanHostCount < kMaxLanHosts)
			host = &sLanHosts[sLanHostCount++];
		if (!host)
			continue;
		snprintf(host->address, sizeof(host->address), "%s", address);
		host->token = packet.raceId;
		memcpy(host->name, packet.players[0].name, sizeof(host->name) - 1);
		host->name[sizeof(host->name) - 1] = '\0';
		host->players = packet.playerCount;
		host->limit = packet.slot;
		host->started = packet.started;
		host->lastSeen = now;
	}
	/* Hosts that stopped answering drop off the list. */
	for (i = 0; i < sLanHostCount; )
		if (now - sLanHosts[i].lastSeen > 3500)
			sLanHosts[i] = sLanHosts[--sLanHostCount];
		else
			++i;
	count = sLanHostCount < max ? sLanHostCount : max;
	memcpy(hosts, sLanHosts, (size_t)count * sizeof(*hosts));
	return count;
}

void Network_StopDiscovery(void)
{
	if (sDiscoverySocket == kInvalidSocket)
		return;
	NetworkCloseSocket(sDiscoverySocket);
	sDiscoverySocket = kInvalidSocket;
#ifdef _WIN32
	WSACleanup();
#endif
}

/* This computer's LAN address, shown in the host lobby. */
int Network_GetLocalAddress(char *address, size_t size)
{
#ifndef _WIN32
	struct ifaddrs *interfaces, *entry;
	int found = 0;
	if (getifaddrs(&interfaces) != 0)
		return 0;
	for (entry = interfaces; entry && !found; entry = entry->ifa_next)
		if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET &&
			(entry->ifa_flags & IFF_UP) && !(entry->ifa_flags & IFF_LOOPBACK)) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)entry->ifa_addr)->sin_addr,
				address, (socklen_t)size);
			found = 1;
		}
	freeifaddrs(interfaces);
	return found;
#else
	(void)address;
	(void)size;
	return 0;
#endif
}

/* LAN lobby vehicle choice for this computer's driver. */
void Network_SetLocalVehicle(int vehicle)
{
	sLocalVehicle = vehicle;
	if (sLocalSlot >= 0 && sLocalSlot < kNetworkMaxPlayers)
		sPlayers[sLocalSlot].vehicle = vehicle;
}

int Network_GetLocalVehicle(void)
{
	return sLocalVehicle > 0 ? sLocalVehicle : kNormalPlayerCarID;
}

int Network_IsSlotActive(int slot)
{
	return slot >= 0 && slot < kNetworkMaxPlayers && sPlayers[slot].active;
}

const char *Network_GetSlotName(int slot)
{
	if (!Network_IsSlotActive(slot))
		return NULL;
	return sPlayers[slot].name[0] ? sPlayers[slot].name : "Driver";
}

int Network_IsActive(void)
{
	return sSocket != kInvalidSocket || sLocalTwoPlayer;
}

int Network_IsClient(void)
{
	return sSocket != kInvalidSocket && !sIsHost;
}

int Network_IsLocalTwoPlayer(void)
{
	return sLocalTwoPlayer && sLocalPlayerCount == 2;
}

int Network_IsLocalMultiplayer(void)
{
	return sLocalTwoPlayer && sLocalPlayerCount > 1;
}

int Network_GetLocalDriverSlot(const tObject *object)
{
	int slot;
	if (!Network_IsLocalMultiplayer() || !object)
		return -1;
	for (slot = 0; slot < sLocalPlayerCount; ++slot)
		if (gPlayers[slot].obj == object)
			return slot;
	return -1;
}

void Network_EnableLocalTwoPlayer(void)
{
	Network_EnableLocalPlayers(2);
}

void Network_EnableLocalPlayers(int count)
{
	int slot;
	if (sSocket != kInvalidSocket) {
		fprintf(stderr, "[network] Local multiplayer cannot be combined with a LAN game\n");
		return;
	}
	if (count < 2 || count > kLocalPlayerCount) {
		fprintf(stderr, "[network] Local player count must be between 2 and %d\n",
			kLocalPlayerCount);
		return;
	}
	sLocalTwoPlayer = 1;
	sLocalPlayerCount = count;
	sLocalSlot = 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		sPlayers[slot].active = slot < count;
	fprintf(stderr, "[multiplayer] Local %d-player controls enabled\n", count);
}

void Network_EndLocalGame(void)
{
	int slot;
	Network_UnbindDriver();
	sSoloModes = 0;
	if (!sLocalTwoPlayer)
		return;
	sLocalTwoPlayer = 0;
	sLocalPlayerCount = 2;
	sLobbyPlayerLimit = kNetworkMaxPlayers;
	for (slot = 1; slot < kNetworkMaxPlayers; ++slot)
		sPlayers[slot].active = 0;
	sLocalPlayer2 = NULL;
	Platform_BeginMultiView(1);
}

int Network_IsLocalPlayer2(const tObject *object)
{
	return Network_GetLocalDriverSlot(object) == 1;
}

int Network_IsDriverObject(const tObject *object)
{
	int slot;
	if (!object)
		return 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (gPlayers[slot].obj == object && sPlayers[slot].active)
			return 1;
	return 0;
}

void Network_BindDriver(tObject *object)
{
	int slot;
	if (!Network_IsActive() || !Network_IsDriverObject(object))
		return;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (gPlayers[slot].obj == object)
			break;
	if (slot >= kNetworkMaxPlayers || slot == sBoundSlot)
		return;
	if (sBoundSlot < 0) {
		sSavedCurPlayer = gCurPlayer;
		sSavedPlayer = gPlayerObj;
		sSavedCamera = gCameraObj;
		sSavedCarID = gPlayerCarID;
		sSavedVisFirst = gFirstVisObj;
		sSavedVisLast = gLastVisObj;
	}
	sBoundSlot = slot;
	gCurPlayer = &gPlayers[slot];
	gPlayerObj = object;
	gCameraObj = object;
	if (sPlayers[slot].vehicle > 0)
		gPlayerCarID = sPlayers[slot].vehicle;
	GetVisObjs();
}

int Network_GetBoundSlot(void)
{
	return Network_IsActive() ? sBoundSlot : -1;
}

void Network_UnbindDriver(void)
{
	if (sBoundSlot < 0)
		return;
	gCurPlayer = sSavedCurPlayer;
	gPlayerObj = sSavedPlayer;
	gCameraObj = sSavedCamera;
	gPlayerCarID = sSavedCarID;
	gFirstVisObj = sSavedVisFirst;
	gLastVisObj = sSavedVisLast;
	sBoundSlot = -1;
}

void Network_ReplaceDriverObject(tObject *oldObject, tObject *newObject)
{
	int slot = GetSlotForPlayer(oldObject);
	if (slot < 0 || !newObject)
		return;
	gPlayers[slot].obj = newObject;
	if (sRemoteObjects[slot] == oldObject)
		sRemoteObjects[slot] = newObject;
	if (sLocalPlayer2 == oldObject)
		sLocalPlayer2 = newObject;
	if (sSavedPlayer == oldObject)
		sSavedPlayer = newObject;
	if (sSavedCamera == oldObject)
		sSavedCamera = newObject;
	if (sSavedVisFirst == oldObject)
		sSavedVisFirst = newObject;
	if (sSavedVisLast == oldObject)
		sSavedVisLast = newObject;
	if (sBoundSlot == slot) {
		gPlayerObj = newObject;
		gCameraObj = newObject;
	}
}

int Network_GetLocalDriverCount(void)
{
	if (!Network_IsActive())
		return 1;
	return sLocalTwoPlayer ? sLocalPlayerCount : (sLocalSlot >= 0 ? 1 : 0);
}

tObject *Network_GetLocalDriverObject(int index)
{
	if (!Network_IsActive())
		return index == 0 ? gPlayerObj : NULL;
	if (sLocalTwoPlayer) {
		return index >= 0 && index < sLocalPlayerCount ?
			gPlayers[index].obj : NULL;
	}

	return index == 0 && sLocalSlot >= 0 ? gPlayers[sLocalSlot].obj : NULL;
}

const char *Network_GetPlayerName(const tObject *object)
{
	int slot = GetSlotForPlayer(object);
	if (slot < 0 || !sPlayers[slot].active)
		return NULL;
	return sPlayers[slot].name[0] ? sPlayers[slot].name : "Driver";
}

int Network_IsPrimaryDriver(const tObject *object)
{
	return object && object == gPlayers[sLocalSlot >= 0 ? sLocalSlot : 0].obj;
}

static int GetSlotForPlayer(const tObject *object)
{
	int slot;
	/* Single player keeps its state in gPlayers[0] too, but must never pick
	   up finished/eliminated flags left over from a multiplayer session. */
	if (!Network_IsActive() || !object)
		return -1;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (gPlayers[slot].obj == object)
			return slot;
	return -1;
}

/* Brake (1) or reverse (2) lights of a driver's car, 0 for none: from the
   own input, or as the driver's computer reported it. */
int Network_DriverLights(int slot)
{
	const tObject *car;
	if (slot < 0 || slot >= kNetworkMaxPlayers || !(car = gPlayers[slot].obj))
		return 0;
	if (!Network_IsActive() || sLocalTwoPlayer || slot == sLocalSlot)
		return car->input.reverse ? 2 : car->input.brake ? 1 : 0;
	return sPlayers[slot].reverse ? 2 : sPlayers[slot].brake ? 1 : 0;
}

int Network_DriverIsFinished(const tObject *object)
{
	int slot = GetSlotForPlayer(object);
	return slot >= 0 && sPlayers[slot].finished;
}

int Network_DriverIsEliminated(const tObject *object)
{
	int slot = GetSlotForPlayer(object);
	return slot >= 0 && sPlayers[slot].eliminated;
}

void Network_MarkDriverFinished(tObject *object)
{
	int slot = GetSlotForPlayer(object);
	if (slot >= 0)
		sPlayers[slot].finished = 1;
}

void Network_MarkDriverEliminated(tObject *object)
{
	int slot = GetSlotForPlayer(object);
	if (slot >= 0)
		sPlayers[slot].eliminated = 1;
}

void Network_StartCampaign(void)
{
	int slot;
	if (!Network_IsActive())
		return;
	Network_UnbindDriver();
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		sPlayerFire[slot] = 0;
		sPlayerMissile[slot] = 0;
		if (sPlayers[slot].active)
			InitializePlayerState(slot);
		gPlayers[slot].obj = NULL;
		gPlayers[slot].spikeObj = NULL;
		gPlayers[slot].brakeObj = NULL;
	}
	InitializePlayerState(sLocalSlot);
	gCurPlayer = &gPlayers[sLocalSlot];
	memset(sSeenGiftSeq, 0, sizeof(sSeenGiftSeq));
	memset(sSeenCreditSeq, 0, sizeof(sSeenCreditSeq));
	memset(sSeenShotSeq, 0, sizeof(sSeenShotSeq));
	memset(sSeenSwarmSeq, 0, sizeof(sSeenSwarmSeq));
	memset(sSeenJamSeq, 0, sizeof(sSeenJamSeq));
	memset(sSeenKillSeq, 0, sizeof(sSeenKillSeq));
	memset(sSeenAwardSeq, 0, sizeof(sSeenAwardSeq));
}

int Network_UpdateRaceProgress(void)
{
	int slot, participants = 0;
	if (!Network_IsActive() || Network_IsClient())
		return 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		tObject *object = gPlayers[slot].obj;
		if (!sPlayers[slot].active)
			continue;
		participants++;
		if (gPlayers[slot].lives <= 0 && gPlayers[slot].deathDelay <= 0)
			sPlayers[slot].eliminated = 1;
		if (object && !sPlayers[slot].finished && !sPlayers[slot].eliminated &&
			object->pos.y > gLevelData->levelEnd)
			sPlayers[slot].finished = 1;
		if (!sPlayers[slot].finished && !sPlayers[slot].eliminated)
			return 0;
	}
	/* Let every finisher see their bonus count and result card first. */
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && sPlayers[slot].finished &&
			gPlayers[slot].finishDelay < GetComplSequenceLength(&gPlayers[slot]))
			return 0;
	return participants > 0;
}

void Network_PrepareNextLevel(void)
{
	int slot;
	if (!Network_IsActive() || Network_IsClient())
		return;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		if (!sPlayers[slot].active || !sPlayers[slot].eliminated)
			continue;
		/* Revive in both copies: remote drivers receive it via the record. */
		gPlayers[slot].lives = 1;
		gPlayers[slot].deathDelay = 0;
		gPlayers[slot].finishDelay = 0;
		sPlayers[slot].lives = 1;
		sPlayers[slot].deathDelay = 0;
		sPlayers[slot].finishDelay = 0;
		sPlayers[slot].finished = 0;
		sPlayers[slot].eliminated = 0;
	}
}

int Network_AllDriversEliminated(void)
{
	int slot, participants = 0;
	if (!Network_IsActive())
		return gCurPlayer->lives <= 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		if (!sPlayers[slot].active)
			continue;
		participants++;
		if (!sPlayers[slot].eliminated)
			return 0;
	}
	return participants > 0;
}

int Network_UnfinishedDriverCount(void)
{
	int slot, count = 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && !sPlayers[slot].finished &&
			!sPlayers[slot].eliminated)
			count++;
	return count;
}

int Network_IsLocalDriver(const tObject *object)
{
	int slot;
	if (!Network_IsActive())
		return 1;
	slot = GetSlotForPlayer(object);
	if (slot < 0)
		return object == gPlayerObj;
	if (sLocalTwoPlayer)
		return slot < sLocalPlayerCount;
	return slot == sLocalSlot;
}

const char *Network_GetLocalPlayerName(int index)
{
	int slot = sLocalTwoPlayer ? index : sLocalSlot;
	if (slot < 0 || slot >= kNetworkMaxPlayers || !sPlayers[slot].active)
		return NULL;
	return sPlayers[slot].name[0] ? sPlayers[slot].name : "Driver";
}

void Network_SetSoloModes(int enabled)
{
	sSoloModes = enabled;
}

/* ---- Crate settings ----
   Kept in their own file next to the preferences: the preferences file has
   no room left, and changing its size would reset everyone's settings.
   Every crate kind has a frequency (off, rare, normal, often); or each level
   picks a random theme with its own mix. */
#define kCrateFileMagic 0x52444334u	/* "RDC4": frequencies and edited themes */
#define kCrateFileMagicV3 0x52444333u	/* "RDC3": frequencies */
#define kCrateFileMagicV2 0x52444332u	/* "RDC2": on/off mask */
#define kCrateFileMagicV1 0x52444352u	/* "RDCR": on/off mask before CAR WAR */

const char *const kCrateKindNames[kCrateKindCount] = {
	"ADDON LOCK", "MINES", "MISSILES", "SPIKES", "POLICE JAMMER", "TURBO",
	"2000 POINTS", "EXTRA LIFE", "SUPER GRIP", "OFFROAD TYRES", "MAGNET",
	"SHIELD", "BATTERING RAM", "OIL SLICK", "NITRO", "RAIN MAKER",
	"ZOMBIE CARS", "COP SWARM", "TRAFFIC JAM", "CAR WAR", "CLEAR ROAD"
};

/* Kinds: lock mines missiles spikes jammer turbo points life grip offroad
   magnet shield ram oil nitro rain zombie swarm jam war clearroad */
const tCrateTheme kCrateThemes[kCrateThemeCount] = {
	{"WEAPONS",  {1,3,3,3,0,0,1,1,0,0,0,0,2,2,0,0,0,0,0,1}},
	{"SPEED",    {0,0,0,0,0,3,1,0,3,3,1,0,0,0,3,0,0,0,0,0,2}},
	{"CHAOS",    {0,1,1,0,0,0,0,0,0,0,0,0,1,2,0,2,3,3,3,3}},
	{"SURVIVAL", {2,0,0,0,3,0,1,2,1,2,0,3,0,0,0,0,1,1,0,0,3}},
	{"POLICE",   {0,0,1,0,2,1,0,0,0,1,0,1,2,0,2,0,0,3,1,0}},
	{"CLASSIC",  {2,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0}},
	{"MAYHEM",   {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3}},
	{"PRANKS",   {0,1,0,0,0,0,1,0,0,0,3,0,3,3,1,3,0,0,1,0,3}},
	{"HUNTERS",  {0,0,2,0,0,0,0,0,0,0,0,3,2,0,0,0,3,3,0,3,0}},
	{"TRAFFIC",  {0,0,0,0,0,0,1,0,0,3,0,0,1,0,0,0,1,0,3,3,3}},
	{"LUCKY",    {3,0,0,0,1,0,3,3,1,0,1,3,0,0,0,0,0,0,0,0,0}}
};

static UInt8 sCrateWeights[kCrateKindCount];
static UInt8 sThemeWeights[kCrateThemeCount][kCrateKindCount];	/* themes as edited */
static int sPackageAmount = kPackagesNormal;
static int sThemeMode;	/* kThemeOff, kThemeRandom or kThemeFixed + theme */
static int sCratesLoaded;
static int sThemeIndex = -1;	/* host: this level's theme */
static UInt8 sHostWeights[kCrateKindCount];	/* LAN client: the host's */
static int sHostTheme = -1, sShownTheme = -1;
static int sHostThemeMode;	/* LAN client: the host's theme setting */

/* The theme setting that applies: on a LAN client the host's. */
int Network_GetThemeMode(void)
{
	return Network_IsClient() ? sHostThemeMode : Network_GetLocalThemeMode();
}

static const char *CrateFilePath(void)
{
	static char path[600];
	char *prefDir = SDL_GetPrefPath("DarrCoh", "RecklessDrivin");
	if (!prefDir)
		return NULL;
	snprintf(path, sizeof(path), "%sCrates", prefDir);
	SDL_free(prefDir);
	return path;
}

/* Settings file: "RDC4", number of kinds, number of themes, the
   frequencies, package amount, theme mode, then each theme's frequencies.
   Older files ("RDC3": frequencies; "RDC2"/"RDCR": on/off) are read too;
   kinds or themes they lack start at their defaults. */
static void LoadCrates(void)
{
	const char *path = CrateFilePath();
	FILE *file = path ? fopen(path, "rb") : NULL;
	UInt8 data[1024];
	size_t got;
	int kind, theme;
	UInt32 magic;
	sCratesLoaded = 1;
	memset(sCrateWeights, kCrateNormal, sizeof(sCrateWeights));
	for (theme = 0; theme < kCrateThemeCount; ++theme)
		memcpy(sThemeWeights[theme], kCrateThemes[theme].weights, kCrateKindCount);
	if (!file)
		return;
	got = fread(data, 1, sizeof(data), file);
	fclose(file);
	if (got < 12)
		return;
	memcpy(&magic, data, 4);
	if (magic == kCrateFileMagic) {
		int kinds = data[4], themes = data[5];
		size_t need = 6 + (size_t)kinds + 2 + (size_t)kinds * themes;
		const UInt8 *themeData = data + 6 + kinds + 2;
		if (got < need)
			return;
		for (kind = 0; kind < kCrateKindCount && kind < kinds; ++kind)
			sCrateWeights[kind] = data[6 + kind] < kCrateWeightCount ? data[6 + kind] : kCrateNormal;
		sPackageAmount = data[6 + kinds] < kPackageAmountCount ? data[6 + kinds] : kPackagesNormal;
		sThemeMode = data[7 + kinds] < kThemeFixed + kCrateThemeCount ? data[7 + kinds] : kThemeOff;
		for (theme = 0; theme < kCrateThemeCount && theme < themes; ++theme)
			for (kind = 0; kind < kCrateKindCount && kind < kinds; ++kind) {
				UInt8 weight = themeData[theme * kinds + kind];
				sThemeWeights[theme][kind] = weight < kCrateWeightCount ? weight :
					kCrateThemes[theme].weights[kind];
			}
	} else if (magic == kCrateFileMagicV3 && got >= 6 + 20) {
		int stored = (int)got - 6;
		for (kind = 0; kind < kCrateKindCount && kind < stored; ++kind)
			sCrateWeights[kind] = data[4 + kind] < kCrateWeightCount ? data[4 + kind] : kCrateNormal;
		sPackageAmount = data[4 + stored] < kPackageAmountCount ? data[4 + stored] : kPackagesNormal;
		sThemeMode = data[5 + stored] < kThemeFixed + kCrateThemeCount ? data[5 + stored] : kThemeOff;
	} else if (magic == kCrateFileMagicV2 || magic == kCrateFileMagicV1) {
		/* Older on/off settings: on becomes normal. */
		UInt32 mask, amount;
		memcpy(&mask, data + 4, 4);
		memcpy(&amount, data + 8, 4);
		if (magic == kCrateFileMagicV1)
			mask |= 1u << kCrateKindWar;
		for (kind = 0; kind < kCrateKindCount; ++kind)
			sCrateWeights[kind] = (mask >> kind) & 1 ? kCrateNormal : kCrateOff;
		sPackageAmount = amount < kPackageAmountCount ? (int)amount : kPackagesNormal;
	}
}

int Network_GetLocalCrateWeight(int kind)
{
	if (!sCratesLoaded)
		LoadCrates();
	return kind >= 0 && kind < kCrateKindCount ? sCrateWeights[kind] : kCrateOff;
}

int Network_GetLocalThemeWeight(int theme, int kind)
{
	if (!sCratesLoaded)
		LoadCrates();
	if (theme < 0 || theme >= kCrateThemeCount || kind < 0 || kind >= kCrateKindCount)
		return kCrateOff;
	return sThemeWeights[theme][kind];
}

int Network_GetLocalPackageAmount(void)
{
	if (!sCratesLoaded)
		LoadCrates();
	return sPackageAmount;
}

int Network_GetLocalThemeMode(void)
{
	if (!sCratesLoaded)
		LoadCrates();
	return sThemeMode;
}

void Network_SetLocalCrates(const UInt8 *weights, int amount, int themeMode,
	const UInt8 themeWeights[kCrateThemeCount][kCrateKindCount])
{
	const char *path = CrateFilePath();
	FILE *file;
	int kind, theme;
	for (kind = 0; kind < kCrateKindCount; ++kind)
		sCrateWeights[kind] = weights[kind] < kCrateWeightCount ? weights[kind] : kCrateNormal;
	for (theme = 0; theme < kCrateThemeCount; ++theme)
		for (kind = 0; kind < kCrateKindCount; ++kind)
			sThemeWeights[theme][kind] = themeWeights[theme][kind] < kCrateWeightCount ?
				themeWeights[theme][kind] : kCrateNormal;
	sPackageAmount = amount >= 0 && amount < kPackageAmountCount ? amount : kPackagesNormal;
	sThemeMode = themeMode >= 0 && themeMode < kThemeFixed + kCrateThemeCount ?
		themeMode : kThemeOff;
	sCratesLoaded = 1;
	file = path ? fopen(path, "wb") : NULL;
	if (file) {
		UInt8 data[6 + kCrateKindCount + 2 + kCrateThemeCount * kCrateKindCount];
		UInt32 magic = kCrateFileMagic;
		memcpy(data, &magic, 4);
		data[4] = kCrateKindCount;
		data[5] = kCrateThemeCount;
		memcpy(data + 6, sCrateWeights, kCrateKindCount);
		data[6 + kCrateKindCount] = (UInt8)sPackageAmount;
		data[7 + kCrateKindCount] = (UInt8)sThemeMode;
		memcpy(data + 8 + kCrateKindCount, sThemeWeights, sizeof(sThemeWeights));
		fwrite(data, sizeof(data), 1, file);
		fclose(file);
	}
}

/* A level starts: with random themes on, the host (or a local game) picks
   this level's theme. Everyone is told which one it is. */
static void AnnounceTheme(int theme)
{
	tTextEffect fx = {320, 60, kEffectSmall + kEffectSinLines, 0, ""};
	char text[32];
	int length;
	snprintf(text, sizeof(text), "THEMEh%s", kCrateThemes[theme].name);
	length = (int)strlen(text);
	fx.text[0] = (UInt8)length;
	memcpy(fx.text + 1, text, (size_t)length);
	NewTextEffect(&fx);
}

void Network_PrepareCrates(void)
{
	sShownTheme = -1;
	sThemeIndex = -1;
	if (!Network_UsesCrateSettings() || Network_IsClient() || !Network_GetLocalThemeMode())
		return;
	sThemeIndex = Network_GetLocalThemeMode() == kThemeRandom ? RanInt(0, kCrateThemeCount) :
		Network_GetLocalThemeMode() - kThemeFixed;
	sShownTheme = sThemeIndex;
	AnnounceTheme(sThemeIndex);
}

/* Frequency of a crate kind in this game (0 = off ... 3 = often). */
int Network_GetCrateWeight(int kind)
{
	if (kind < 0 || kind >= kCrateKindCount)
		return kCrateOff;
	if (Network_IsClient())
		return sHostWeights[kind];
	if (sThemeIndex >= 0)
		return Network_GetLocalThemeWeight(sThemeIndex, kind);
	return Network_GetLocalCrateWeight(kind);
}

/* The settings apply to games started from the multiplayer menu. */
int Network_UsesCrateSettings(void)
{
	return Network_IsActive() || sSoloModes;
}

int Network_GetPackageAmount(void)
{
	int amount = Network_IsClient() ? sHostPackageAmount : Network_GetLocalPackageAmount();
	if (!Network_UsesCrateSettings())
		amount = kPackagesNormal;
	/* Package mode is about packages: at least many of them. */
	if (Network_IsPackageMode() && amount < kPackagesMany)
		amount = kPackagesMany;
	return amount;
}

/* Multiplayer game mode (gPrefs.unused[8]); on a LAN the host's applies. */
int Network_GetGameMode(void)
{
	if (!Network_IsActive() && !sSoloModes)
		return kGameModeCoop;
	if (Network_IsClient())
		return sHostMode;
	return gPrefs.unused[8] < kGameModeCount ? gPrefs.unused[8] : kGameModeCoop;
}

int Network_IsDuel(void)
{
	return Network_GetGameMode() == kGameModeDuel;
}

/* Many packages on the road, no cop swarm or traffic jam crates. */
int Network_IsPackageMode(void)
{
	return Network_GetGameMode() == kGameModePackages;
}

/* A cop swarm takes the police jammer away from its target, and for a
   while no new one can be picked up. */
#define kSwarmJammerFrames (30 * 60)
static unsigned long sSwarmUntil[kNetworkMaxPlayers];

static void MarkSwarm(int slot)
{
	if (slot < 0 || slot >= kNetworkMaxPlayers ||
		(Network_IsActive() && !IsLocalSlot(slot)))
		return;	/* a remote driver's computer does this */
	sSwarmUntil[slot] = gFrameCount + kSwarmJammerFrames;
	if (gPlayers[slot].addOns & kAddOnCop) {
		tTextEffect fx = {320, 82, kEffectSmall, 0, "\x0b" "JAMMERhLOST"};
		int previous;
		gPlayers[slot].addOns &= ~kAddOnCop;
		previous = BindNested(gPlayers[slot].obj);
		NewTextEffect(&fx);
		RestoreBinding(previous);
	}
}

int Network_SwarmHunts(const tObject *driver)
{
	int slot = Network_IsActive() ? GetSlotForPlayer(driver) : 0;
	return slot >= 0 && gFrameCount < sSwarmUntil[slot];
}

/* Battering ram crate: does this driver's car blow up what it rams? */
int Network_DriverRamming(const tObject *object)
{
	int slot;
	if (!object)
		return 0;
	if (!Network_IsActive())
		return object == gPlayerObj && gCurPlayer->ramTime > 0;
	slot = GetSlotForPlayer(object);
	return slot >= 0 && gPlayers[slot].ramTime > 0;
}

/* Rain crate: another driver made it rain, so this one has less grip. */
int Network_RainHits(const tObject *object)
{
	int slot = GetSlotForPlayer(object), other;
	if (slot < 0)
		return 0;
	for (other = 0; other < kNetworkMaxPlayers; ++other)
		if (other != slot && sPlayers[other].active && gPlayers[other].rainTime > 0)
			return 1;
	return 0;
}

/* Clear road crate: the driver near this car (ahead or behind, close to its
   line) whom it has to make way for, or NULL. */
tObject *Network_ClearRoadDriver(const tObject *car)
{
	int slot;
	tObject *best = NULL;
	float bestDistance = 360;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		tObject *driver;
		float dy;
		if (Network_IsActive() ? !sPlayers[slot].active : slot != 0)
			continue;
		driver = Network_IsActive() ? gPlayers[slot].obj : gPlayerObj;
		if (!driver || gPlayers[slot].dodgeTime <= 0)
			continue;
		dy = fabsf(driver->pos.y - car->pos.y);
		if (dy < bestDistance && fabsf(driver->pos.x - car->pos.x) < 90) {
			bestDistance = dy;
			best = driver;
		}
	}
	return best;
}

/* Package and police modes: cops hunt the drivers down. */
int Network_HasAggressiveCops(void)
{
	int mode = Network_GetGameMode();
	return mode == kGameModePackages || mode == kGameModePolice || mode == kGameModeZombie;
}

/* Zombie mode, or a zombie crate (15 s): ordinary traffic near a driver
   turns and hunts him, using the hunting cops' control. The crate sends it
   after the other drivers; alone, after the one who picked it up. */
#define kZombieDist 1000.0f

int Network_IsZombieTarget(const tObject *driver)
{
	int slot, other;
	if (!driver)
		return 0;
	if (Network_GetGameMode() == kGameModeZombie)
		return 1;
	if (!Network_IsActive())
		return driver == gPlayerObj && gCurPlayer->zombieTime > 0;
	slot = GetSlotForPlayer(driver);
	if (slot < 0)
		return 0;
	for (other = 0; other < kNetworkMaxPlayers; ++other)
		if (other != slot && sPlayers[other].active && gPlayers[other].zombieTime > 0)
			return 1;
	return 0;
}

static int ZombiesActive(void)
{
	int slot;
	if (Network_GetGameMode() == kGameModeZombie)
		return 1;
	if (!Network_IsActive())
		return gCurPlayer->zombieTime > 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && gPlayers[slot].zombieTime > 0)
			return 1;
	return 0;
}

/* Car war crate (15 s): traffic near the drivers picks the closest other
   car and tries to ram it off the road. */
#define kWarRange 700.0f
#define kWarRefreshFrames 120	/* a war car gives up this long after the crate */

int Network_WarActive(void)
{
	return WarActive();
}

static int WarActive(void)
{
	int slot;
	if (!Network_IsActive())
		return gCurPlayer->warTime > 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && gPlayers[slot].warTime > 0)
			return 1;
	return 0;
}

static int IsTrafficCar(const tObject *object)
{
	return object->type->flags & kObjectWheelFlag && !(object->type->flags & kObjectCop) &&
		!object->replica && !Network_IsDriverObject(object) && object != gPlayerObj &&
		object->control != kObjectNoInput &&
		!(object->type->flags2 & kObjectDamageble && object->damage > object->type->maxDamage);
}

static void UpdateWar(void)
{
	tObject *object;
	UInt32 until = (UInt32)gFrameCount + kWarRefreshFrames;
	if (!WarActive() || Network_IsClient() || Network_RaceHeld() ||
		gFrameCount % 20 || !gFirstObj)
		return;
	for (object = (tObject *)gFirstObj->next; object != gFirstObj;
		object = (tObject *)object->next) {
		tObject *other, *enemy = NULL;
		float best = kWarRange;
		if (!IsTrafficCar(object) ||
			Network_NearestDriverDistance(object->pos.y) > kVisDist)
			continue;
		if (object->warUntil > gFrameCount) {
			object->warUntil = until;	/* keeps fighting its enemy */
			continue;
		}
		/* Ordinary traffic, and zombies or furious cars too: the war wins. */
		if (object->control != kObjectDriveUp && object->control != kObjectDriveDown &&
			object->control != kObjectCopControl)
			continue;
		for (other = (tObject *)gFirstObj->next; other != gFirstObj;
			other = (tObject *)other->next) {
			float distance;
			if (other == object || !IsTrafficCar(other))
				continue;
			distance = VEC2D_Value(VEC2D_Difference(other->pos, object->pos));
			if (distance < best) {
				best = distance;
				enemy = other;
			}
		}
		if (!enemy)
			continue;
		object->warTargetId = enemy->netId;
		object->warUntil = until;
		object->control = kObjectCopControl;
		for (object->target = 0;
			object->target < (int)gTrackUp->num - 1 && gTrackUp->track[object->target].y < object->pos.y;
			object->target++);
	}
}

/* Wildlife mode: new animals keep coming out ahead of every driver. */
#define kWildlifeInterval 45	/* frames */
#define kWildlifeMaxNear 14

static void UpdateWildlife(void)
{
	int slot;
	if (Network_GetGameMode() != kGameModeWildlife || Network_IsClient() ||
		Network_RaceHeld() || gFrameCount % kWildlifeInterval || !gFirstObj)
		return;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		tObject *driver = Network_IsActive() ? (IsRacing(slot) ? gPlayers[slot].obj : NULL) :
			(slot == 0 ? gPlayerObj : NULL);
		if (!driver || Extended_CountAnimalsNear(driver) >= kWildlifeMaxNear)
			continue;
		Extended_SpawnAnimal(driver->pos.y + 500 + RanFl(0, 450));
		Extended_SpawnAnimal(driver->pos.y + 500 + RanFl(0, 450));
	}
}

static void UpdateZombies(void)
{
	tObject *object;
	/* During a car war the cars fight each other, not the drivers. */
	if (!ZombiesActive() || WarActive() || Network_IsClient() ||
		Network_RaceHeld() || gFrameCount % 30 || !gFirstObj)
		return;
	for (object = (tObject *)gFirstObj->next; object != gFirstObj;
		object = (tObject *)object->next) {
		tObject *victim = NULL;
		float best = kZombieDist;
		int slot;
		if (!(object->type->flags & kObjectWheelFlag) || object->type->flags & kObjectCop ||
			object->replica || Network_IsDriverObject(object) || object == gPlayerObj ||
			(object->control != kObjectDriveUp && object->control != kObjectDriveDown) ||
			(object->type->flags2 & kObjectDamageble && object->damage > object->type->maxDamage))
			continue;
		for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
			tObject *driver = Network_IsActive() ? (IsRacing(slot) ? gPlayers[slot].obj : NULL) :
				(slot == 0 ? gPlayerObj : NULL);
			float distance;
			if (!driver || !Network_IsZombieTarget(driver))
				continue;
			distance = VEC2D_Value(VEC2D_Difference(driver->pos, object->pos));
			if (distance < best) {
				best = distance;
				victim = driver;
			}
		}
		if (!victim)
			continue;
		object->chaseTarget = victim;
		object->control = kObjectCopControl;
		for (object->target = 0;
			object->target < (int)gTrackUp->num - 1 && gTrackUp->track[object->target].y < object->pos.y;
			object->target++);
	}
}

/* Police mode: new cops keep coming for every driver. Spawned where the
   world is simulated (not on LAN clients). */
#define kPoliceInterval 150	/* frames between spawns */
#define kPoliceMaxNear 10	/* cops around one driver */

void Network_UpdatePoliceMode(void)
{
	int slot;
	static UInt8 rainSeen[kNetworkMaxPlayers];
	UpdateZombies();
	UpdateWildlife();
	UpdateWar();
	/* Jammers stay off while a swarm hunts (also against Lock add-ons). */
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (gFrameCount < sSwarmUntil[slot])
			gPlayers[slot].addOns &= ~kAddOnCop;
	/* Tell drivers on this computer when someone else's rain starts. */
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		int hits = Network_IsActive() && IsLocalSlot(slot) && gPlayers[slot].obj &&
			Network_RainHits(gPlayers[slot].obj);
		if (hits && !rainSeen[slot]) {
			tTextEffect fx = {320, 60, kEffectSmall + kEffectSinLines, 0, "\x04" "RAIN"};
			int previous = BindNested(gPlayers[slot].obj);
			NewTextEffect(&fx);
			RestoreBinding(previous);
		}
		rainSeen[slot] = (UInt8)hits;
	}
	if (Network_GetGameMode() != kGameModePolice || Network_IsClient() ||
		Network_RaceHeld() || gFrameCount % kPoliceInterval)
		return;
	if (!Network_IsActive()) {
		if (gPlayerObj && gCurPlayer->lives > 0 && !gCurPlayer->deathDelay &&
			!gCurPlayer->finishDelay && Extended_CountCopsNear(gPlayerObj) < kPoliceMaxNear)
			Extended_SpawnCops(gPlayerObj, 2);
		return;
	}
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (IsRacing(slot) && gPlayers[slot].lives > 0 && !gPlayers[slot].deathDelay &&
			Extended_CountCopsNear(gPlayers[slot].obj) < kPoliceMaxNear)
			Extended_SpawnCops(gPlayers[slot].obj, 2);
}

static int IsLocalSlot(int slot)
{
	if (slot < 0 || slot >= kNetworkMaxPlayers)
		return 0;
	if (sLocalTwoPlayer)
		return slot < sLocalPlayerCount;
	return slot == sLocalSlot;
}

static int IsRacing(int slot)
{
	return sPlayers[slot].active && !sPlayers[slot].finished &&
		!sPlayers[slot].eliminated && gPlayers[slot].obj;
}

/* 1-based race position: finishers in crossing order, then drivers still
   racing by distance, then drivers who are out. */
int Network_GetDriverPlace(const tObject *object)
{
	int slot = GetSlotForPlayer(object), other, place = 1;
	if (slot < 0)
		return 0;
	for (other = 0; other < kNetworkMaxPlayers; ++other) {
		int ahead;
		if (other == slot || !sPlayers[other].active)
			continue;
		if (sPlayers[slot].finished)
			ahead = sPlayers[other].finished &&
				gPlayers[other].finishPlace < gPlayers[slot].finishPlace;
		else if (sPlayers[slot].eliminated)
			ahead = !sPlayers[other].eliminated ||
				gPlayers[other].score > gPlayers[slot].score;
		else
			ahead = sPlayers[other].finished ||
				(IsRacing(other) && gPlayers[other].obj->pos.y > object->pos.y);
		if (ahead)
			place++;
	}
	return place;
}

/* Distance (along the road) to the closest driver's car; traffic is only
   simulated in detail near a driver. Single player: the camera's car. */
float Network_NearestDriverDistance(float y)
{
	float best = INFINITY;
	int slot;
	if (!Network_IsActive())
		return fabsf(y - gCameraObj->pos.y);
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && gPlayers[slot].obj) {
			float distance = fabsf(y - gPlayers[slot].obj->pos.y);
			if (distance < best)
				best = distance;
		}
	return best < INFINITY ? best : fabsf(y - gCameraObj->pos.y);
}

/* Road range covered by all drivers, for the objects simulated each frame. */
void Network_GetDriverRange(float *minY, float *maxY)
{
	int slot;
	*minY = *maxY = gCameraObj->pos.y;
	if (!Network_IsActive())
		return;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && gPlayers[slot].obj) {
			float y = gPlayers[slot].obj->pos.y;
			if (y < *minY) *minY = y;
			if (y > *maxY) *maxY = y;
		}
}

int Network_GetFinishedDriverCount(void)
{
	int slot, count = 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && sPlayers[slot].finished)
			count++;
	return count;
}

int Network_GetPlacementBonus(int place)
{
	static const int kPlacementBonus[] = {0, 1000, 500, 250};
	if (Network_GetPlayerCount() < 2 || place < 1 || place > 3)
		return 0;
	return kPlacementBonus[place];
}

static int GetLeaderSlot(void)
{
	int slot, leader = -1;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (IsRacing(slot) && (leader < 0 ||
			gPlayers[slot].obj->pos.y > gPlayers[leader].obj->pos.y))
			leader = slot;
	return leader;
}

/* Drivers well behind the leader get help from pickups. */
int Network_IsTrailingDriver(const tObject *object)
{
	int leader = GetLeaderSlot();
	if (!Network_IsActive() || leader < 0 || !object)
		return 0;
	return gPlayers[leader].obj->pos.y - object->pos.y > 400;
}

tObject *Network_GetSpectateTarget(const tObject *owner)
{
	int slot = GetSlotForPlayer(owner), leader;
	if (slot < 0 || !sPlayers[slot].eliminated)
		return (tObject *)owner;
	leader = GetLeaderSlot();
	return leader >= 0 ? gPlayers[leader].obj : (tObject *)owner;
}

/* Eliminated drivers watch the leader; ease their camera like a driver's. */
void Network_UpdateSpectators(void)
{
	int slot;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && sPlayers[slot].eliminated)
			Player_UpdateZoom(&gPlayers[slot],
				Network_GetSpectateTarget(gPlayers[slot].obj));
}

/* Temporarily act as another driver, e.g. while a weapon's damage is booked;
   RestoreBinding returns to whoever was bound before (or to nobody). */
static int BindNested(tObject *driver)
{
	int previous = sBoundSlot;
	Network_BindDriver(driver);
	return previous;
}

static void RestoreBinding(int previous)
{
	if (previous >= 0)
		Network_BindDriver(gPlayers[previous].obj);
	else
		Network_UnbindDriver();
}

/* Runs KillObject for a driver that is not the bound one, as that driver.
   A remote driver's car is simulated on its own computer. */
void Network_KillDriver(tObject *object)
{
	int previous;
	if (Network_IsRemoteObject(object))
		return;
	previous = BindNested(object);
	if (gPlayerObj == object)
		KillObject(object);
	RestoreBinding(previous);
}

/* A driver's weapon destroyed something: the points go to that driver, or to
   nobody when the weapon is a replay of a remote driver's shot. */
void Network_KillObjectFor(tObject *victim, tObject *killer)
{
	int slot = GetSlotForPlayer(killer);
	if (slot < 0 || !sPlayers[slot].active) {
		victim->lastHitSlot = 0;	/* a cop's weapon: points for nobody */
		KillObject(victim);
		return;
	}
	victim->lastHitSlot = (UInt8)(slot + 1);
	victim->lastHitFrame = (UInt32)gFrameCount;
	if (!IsLocalSlot(slot)) {
		/* A replay of another computer's shot: that computer scores it. */
		gNoKillScore = 1;
		KillObject(victim);
		gNoKillScore = 0;
		return;
	}
	KillObject(victim);	/* Network_CreditKill gives the shooter the points */
}

/* ---- Fair points ----
   An object remembers the driver who last touched it. If it is destroyed
   within kHitCreditFrames (rammed into the ditch, burnt out after a crash),
   that driver gets its points; with no driver involved, nobody does. */
#define kHitCreditFrames (6 * 60)

void Network_MarkHit(tObject *object, const tObject *driver)
{
	int slot = GetSlotForPlayer(driver);
	if (slot < 0 || !object || Network_IsDriverObject(object))
		return;
	object->lastHitSlot = (UInt8)(slot + 1);
	object->lastHitFrame = (UInt32)gFrameCount;
}

void Network_CreditKill(tObject *victim, int score)
{
	int slot = victim->lastHitSlot - 1;
	if (slot < 0 || slot >= kNetworkMaxPlayers || !sPlayers[slot].active ||
		(UInt32)gFrameCount - victim->lastHitFrame > kHitCreditFrames)
		return;
	if (IsLocalSlot(slot)) {
		int previous = BindNested(gPlayers[slot].obj);
		gPlayers[slot].score += score;
		KillScoreText(victim, score);	/* in that driver's view */
		RestoreBinding(previous);
	} else if (sIsHost && sLocalSlot >= 0) {
		/* Destroyed in the host's world by a driver on another computer:
		   tell that computer. */
		tNetworkPlayer *state = &sPlayers[sLocalSlot];
		state->awardSeq++;
		state->awards[state->awardSeq % 8].seq = state->awardSeq;
		state->awards[state->awardSeq % 8].target = (UInt8)slot;
		state->awards[state->awardSeq % 8].amount = (UInt16)score;
		state->awards[state->awardSeq % 8].x = victim->pos.x;
		state->awards[state->awardSeq % 8].y = victim->pos.y;
	}
}

/* Points the host's world gave to drivers on this computer. */
static void ApplyAwards(int slot)
{
	const tNetworkPlayer *state = &sPlayers[slot];
	if ((UInt8)(state->awardSeq - sSeenAwardSeq[slot]) > 8)
		sSeenAwardSeq[slot] = (UInt8)(state->awardSeq - 8);
	while (sSeenAwardSeq[slot] != state->awardSeq) {
		int target;
		sSeenAwardSeq[slot]++;
		if (state->awards[sSeenAwardSeq[slot] % 8].seq != sSeenAwardSeq[slot])
			continue;
		target = state->awards[sSeenAwardSeq[slot] % 8].target;
		if (IsLocalSlot(target) && gPlayers[target].obj) {
			tObject spot;
			int previous = BindNested(gPlayers[target].obj);
			memset(&spot, 0, sizeof(spot));
			spot.pos = P2D(state->awards[sSeenAwardSeq[slot] % 8].x,
				state->awards[sSeenAwardSeq[slot] % 8].y);
			gPlayers[target].score += state->awards[sSeenAwardSeq[slot] % 8].amount;
			KillScoreText(&spot, state->awards[sSeenAwardSeq[slot] % 8].amount);
			RestoreBinding(previous);
		}
	}
}

/* ---- Smooth remote motion ----
   States arrive every 50 ms, but over WLAN often in bursts. Each carries the
   sender's clock, so objects are shown kInterpDelayMS in the past, between
   two known states; only when states are late is the motion extrapolated. */

/* The sender's clock minus ours: the smallest difference seen is the one
   with the least network delay. It may creep up slowly (clock drift). */
static void UpdateClock(int slot, UInt32 senderTime)
{
	float offset = (float)(SInt32)((UInt32)sSimClockMS - senderTime);
	/* Far later than usual: the sender's game was paused, start over. */
	if (!sClockKnown[slot] || offset < sClockOffset[slot] ||
		offset > sClockOffset[slot] + 300) {
		sClockOffset[slot] = offset;
		sClockKnown[slot] = 1;
	} else
		sClockOffset[slot] += (offset - sClockOffset[slot]) * 0.002f;
}

/* The sender-clock time that is shown now. */
static UInt32 ShownTime(int slot)
{
	return (UInt32)sSimClockMS - (UInt32)(SInt32)sClockOffset[slot] - kInterpDelayMS;
}

static void PushSample(tNetSample *ring, UInt8 *head, UInt8 *count, const tNetSample *sample)
{
	if (*count && (SInt32)(sample->time - ring[*head].time) <= 0)
		return;	/* repeated or late */
	*head = (UInt8)((*head + 1) % kNetSamples);
	ring[*head] = *sample;
	if (*count < kNetSamples)
		(*count)++;
}

static float TurnBetween(float from, float to)
{
	return fmodf(to - from + 3 * PI, 2 * PI) - PI;
}

static void SampleAt(const tNetSample *ring, UInt8 head, UInt8 count, UInt32 time,
	tNetSample *out)
{
	const tNetSample *newest = &ring[head];
	int i;
	if ((SInt32)(time - newest->time) >= 0) {
		float ahead = (SInt32)(time - newest->time) / 1000.0f;
		if (ahead > 0.25f)
			ahead = 0.25f;	/* lost states: do not drive off */
		*out = *newest;
		out->pos = VEC2D_Sum(newest->pos, VEC2D_Scale(newest->velo, kScale * ahead));
		return;
	}
	for (i = 1; i < count; ++i) {
		const tNetSample *older = &ring[(head - i + kNetSamples) % kNetSamples];
		const tNetSample *newer = &ring[(head - i + 1 + kNetSamples) % kNetSamples];
		if ((SInt32)(time - older->time) >= 0) {
			float span = (float)(SInt32)(newer->time - older->time);
			float f = span > 0 ? (SInt32)(time - older->time) / span : 1;
			out->pos = VEC2D_Sum(older->pos,
				VEC2D_Scale(VEC2D_Difference(newer->pos, older->pos), f));
			out->velo = VEC2D_Sum(older->velo,
				VEC2D_Scale(VEC2D_Difference(newer->velo, older->velo), f));
			out->dir = older->dir + TurnBetween(older->dir, newer->dir) * f;
			out->time = time;
			return;
		}
	}
	*out = ring[(head - count + 1 + kNetSamples) % kNetSamples];
}

/* Move a remote object to its shown state; only small corrections glide. */
static void FollowSample(tObject *object, const tNetSample *sample)
{
	t2DPoint error = VEC2D_Difference(sample->pos, object->pos);
	if (VEC2D_Value(error) > 250) {
		object->pos = sample->pos;
		object->dir = sample->dir;
	} else {
		object->pos = VEC2D_Sum(object->pos, VEC2D_Scale(error, 0.5f));
		object->dir += TurnBetween(object->dir, sample->dir) * 0.5f;
	}
	object->velo = sample->velo;
}

/* Show the dents another computer reports: the same places, made from the
   same undamaged sprite. */
static void ApplyLook(tObject *object, int baseFrame, int damageFlags, float damage,
	const UInt8 *dents)
{
	float maxDamage = object->type->maxDamage > 0 ? object->type->maxDamage : 1;
	int newFlags, bits = 0, bit;
	if (!SpriteIsValid(baseFrame) || SpriteIsDynamic(baseFrame))
		baseFrame = object->type->frame;
	/* Animated objects lose their dents on the sender too. */
	if (object->type->frameDuration)
		damageFlags = 0;
	if (object->netBaseFrame != baseFrame || (object->damageFlags & ~damageFlags) ||
		(!SpriteIsDynamic(object->frame) && object->frame != baseFrame)) {
		if (SpriteIsDynamic(object->frame))
			SpriteUnused(object->frame);
		object->frame = baseFrame;
		object->netBaseFrame = baseFrame;
		object->damageFlags = 0;
		object->netDamage = 0;
	}
	newFlags = damageFlags & ~object->damageFlags;
	if (newFlags) {
		float amount;
		for (bit = 0; bit < 9; ++bit)
			if (newFlags & (1 << bit))
				bits++;
		/* Usually one new dent per state: it got the new damage. */
		amount = (damage - object->netDamage) / maxDamage * 20 / bits;
		if (amount < 1)
			amount = 1;
		if (amount > 20)
			amount = 20;
		for (bit = 0; bit < 9; ++bit)
			if (newFlags & (1 << bit))
				object->frame = DamageLookFrame(object, object->frame, 1 << bit,
					dents && dents[bit] ? dents[bit] / 12.0f : amount);
		object->damageFlags |= newFlags;
		object->netDamage = damage;
	}
	object->damage = damage;
}

/* ---- World sync (LAN) ---- */

void Explosion(t2DPoint pos, t2DPoint velo, int offs, float mass, int sound);
/* ---- Remote drivers' effects ----
   Tire tracks, dust, brake lights, spikes and the death explosion come from
   a car's own physics and handling, which only run on the computer that
   drives it. The others make them here from the reported state. */

#define kTrackIntensity 0.075f	/* as in objectPhysics.c */

void AddTrack(t2DPoint p1, t2DPoint p2, float intensity);
void MakeSmoke(tObject *theObj);

static t2DPoint sWheelPos[kNetworkMaxPlayers][4];
static UInt8 sWheelPosValid[kNetworkMaxPlayers];
static float sSeenDeathDelay[kNetworkMaxPlayers];

static void RemoveDriverExtras(int slot)
{
	if (gPlayers[slot].spikeObj) {
		RemoveObject(gPlayers[slot].spikeObj);
		gPlayers[slot].spikeObj = NULL;
	}
	if (gPlayers[slot].brakeObj) {
		RemoveObject(gPlayers[slot].brakeObj);
		gPlayers[slot].brakeObj = NULL;
	}
	sWheelPosValid[slot] = 0;
}

static tObject *NewDriverExtra(tObject *car, int typeRes)
{
	tObject *extra = NewObject(car, (SInt16)typeRes);
	if (extra) {
		extra->netLocal = 1;
		extra->pos = car->pos;
		extra->dir = car->dir;
		extra->jumpHeight = car->jumpHeight;
	}
	return extra;
}

static void RemoteDriverEffects(int slot, tObject *car)
{
	tPlayer *player = &gPlayers[slot];
	const tNetworkPlayer *state = &sPlayers[slot];
	tObjectTypePtr type = car->type;
	int alive = !player->deathDelay && player->lives > 0 && !state->finished &&
		!state->eliminated;
	/* Wheels as in ControlWheelObjectPlayer: front left, front right,
	   rear left, rear right. */
	float s = sinf(car->dir), c = cosf(car->dir);
	float wl = type->wheelLength, ww = type->wheelWidth;
	t2DPoint wheels[4];
	int i;
	wheels[0] = P2D(s * wl - c * ww, c * wl + s * ww);
	wheels[1] = P2D(s * wl + c * ww, c * wl - s * ww);
	wheels[2] = P2D(-s * wl - c * ww, -c * wl + s * ww);
	wheels[3] = P2D(-s * wl + c * ww, -c * wl - s * ww);
	for (i = 0; i < 4; ++i)
		wheels[i] = VEC2D_Sum(VEC2D_Scale(wheels[i], kScale), car->pos);

	/* Death: the explosion KillObject made on the driver's computer. */
	if (player->deathDelay > 0 && sSeenDeathDelay[slot] <= 0 &&
		type->flags & kObjectDefaultDeath)
		Explosion(car->pos, car->velo, 0, type->mass, true);
	sSeenDeathDelay[slot] = player->deathDelay;

	if (alive && !car->jumpHeight && gRoadInfo) {
		for (i = 0; i < 4; ++i) {
			float slide = player->slide[i];
			if (slide > gRoadInfo->trackSlide && sWheelPosValid[slot] &&
				VEC2D_Value(VEC2D_Difference(wheels[i], sWheelPos[slot][i])) < 30 &&
				!CalcBackCollision(wheels[i]))
				AddTrack(sWheelPos[slot][i], wheels[i],
					(slide - gRoadInfo->trackSlide) * kTrackIntensity);
			if (slide > gRoadInfo->dustSlide)
				NewParticleFX(wheels[i], car->velo, 12, gRoadInfo->dustColor, 0, 20);
		}
	}
	memcpy(sWheelPos[slot], wheels, sizeof(wheels));
	sWheelPosValid[slot] = 1;

	/* Nitro flames. */
	if (alive && player->nitroTime > 0 && !(gFrameCount % 3))
		NitroFlame(car);
	/* Spikes, as in PlayerHandling. */
	if (alive && player->addOns & kAddOnSpikes && !gRoadInfo->water) {
		if (!player->spikeObj)
			player->spikeObj = NewDriverExtra(car, 207);
		if (player->spikeObj) {
			player->spikeObj->pos = car->pos;
			player->spikeObj->dir = car->dir;
			player->spikeObj->jumpHeight = car->jumpHeight;
			player->spikeObj->frame = player->spikeObj->type->frame +
				(int)player->spikeFrame % player->spikeObj->type->numFrames;
		}
	} else if (player->spikeObj) {
		RemoveObject(player->spikeObj);
		player->spikeObj = NULL;
	}
	/* Brake and reverse lights. */
	if (player->brakeObj) {
		RemoveObject(player->brakeObj);
		player->brakeObj = NULL;
	}
	/* (A motorbike's light is drawn on its own: the picture is a car's.) */
	if ((state->brake || state->reverse) && alive && !gRoadInfo->water &&
		car->type->width >= 1.0f) {
		player->brakeObj = NewDriverExtra(car, 228);
		if (player->brakeObj) {
			if (state->reverse)
				player->brakeObj->frame = 394;
			if (!state->brake)
				player->brakeObj->frame = 395;
		}
	}
}


void Explosion(t2DPoint pos, t2DPoint velo, int offs, float mass, int sound);

/* Objects of the shared world: everything but the drivers, their own
   weapons (replayed through shot events) and short-lived effects. */
static int IsWorldObject(const tObject *object)
{
	int slot;
	/* Short-lived effects are left out, unless an object the clients
	   already have turned into one: an opened package, a car's burning
	   wreck. Then they must see it too. */
	if (object->netLocal || object->replica || !object->type ||
		(object->type->flags & kObjectDieWhenAnimEndsFlag && !object->netSent) ||
		object->typeRes == 207 || object->typeRes == 228 ||	/* spikes, brake lights */
		Network_IsDriverObject(object) || Network_IsRemoteObject(object))
		return 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (object == gPlayers[slot].spikeObj || object == gPlayers[slot].brakeObj)
			return 0;
	return 1;
}

static SInt16 PackScaled(float value)
{
	value *= 32.0f;
	if (value > 32767.0f) value = 32767.0f;
	if (value < -32768.0f) value = -32768.0f;
	return (SInt16)value;
}

static void SendWorldPacket(int peer, tWorldPacket *packet)
{
	int size = (int)offsetof(tWorldPacket, objects) + packet->count * (int)sizeof(tNetObject);
	packet->packetSize = (UInt16)size;
	packet->sequence = ++sSequence;
	sendto(sSocket, (const char *)packet, size, 0,
		(const struct sockaddr *)&sPeers[peer].address, (int)sizeof(sPeers[peer].address));
	packet->count = 0;
}

static void SendWorldTo(int peer)
{
	tWorldPacket packet;
	tObject *object;
	float nearY = sPlayers[peer].y, watchY = nearY;
	/* A driver who is out watches the leader. */
	if (sPlayers[peer].eliminated) {
		int leader = GetLeaderSlot();
		if (leader >= 0)
			watchY = gPlayers[leader].obj->pos.y;
	}
	memset(&packet, 0, offsetof(tWorldPacket, objects));
	packet.magic = kNetworkMagic;
	packet.version = kNetworkVersion;
	packet.type = kPacketWorld;
	packet.slot = (UInt8)peer;
	packet.level = (UInt8)gLevelID;
	packet.raceId = sRaceId;
	packet.timeMS = (UInt32)sSimClockMS;
	for (object = (tObject *)gFirstObj->next; object != gFirstObj;
		object = (tObject *)object->next) {
		tNetObject *entry;
		float dir;
		if ((fabsf(object->pos.y - nearY) > kWorldRange &&
			fabsf(object->pos.y - watchY) > kWorldRange) || !IsWorldObject(object))
			continue;
		entry = &packet.objects[packet.count];
		object->netSent = 1;
		dir = fmodf(object->dir, 2 * PI);
		if (dir < 0)
			dir += 2 * PI;
		entry->id = object->netId;
		entry->typeRes = object->typeRes;
		/* A damaged object's sprite is a local copy: send the undamaged
		   one (e.g. the car's colour) and where the dents are. */
		entry->frame = (SInt16)(SpriteBaseFrame(object->frame) ?
			SpriteBaseFrame(object->frame) : object->type->frame);
		entry->damageFlags = (UInt16)object->damageFlags;
		entry->damage = (UInt16)(object->damage < 0 ? 0 :
			object->damage > 65535 ? 65535 : object->damage);
		entry->control = (UInt8)object->control;
		entry->layer = (UInt8)object->layer;
		entry->x = object->pos.x;
		entry->y = object->pos.y;
		entry->dir = (SInt16)(UInt16)(dir * (65536.0f / (2 * PI)));
		entry->veloX = PackScaled(object->velo.x);
		entry->veloY = PackScaled(object->velo.y);
		entry->jumpHeight = PackScaled(object->jumpHeight);
		if (++packet.count == kWorldChunk)
			SendWorldPacket(peer, &packet);
	}
	if (packet.count)
		SendWorldPacket(peer, &packet);
}

static void SendWorld(void)
{
	int peer;
	for (peer = 1; peer < kNetworkMaxPlayers; ++peer)
		if (sPeers[peer].used && sPlayers[peer].active)
			SendWorldTo(peer);
}

static int IsTombstoned(UInt16 id, SInt16 typeRes, UInt64 now)
{
	int i;
	for (i = 0; i < kTombstones; ++i)
		if (sTombstones[i].id == id && sTombstones[i].typeRes == typeRes &&
			now - sTombstones[i].timeMS < 1500)
			return 1;
	return 0;
}

static void RemoveReplica(tObject *object)
{
	SpriteUnused(object->frame);
	RemoveObject(object);
}

/* The host destroyed it (or turned it into a wreck): blow it up here too. */
static int ChangeReplicaType(tObject *object, SInt16 typeRes)
{
	tObjectTypePtr type = (tObjectTypePtr)GetUnsortedPackEntry(kPackObTy, typeRes, 0);
	if (!type)
		return 0;
	if (object->type->flags & kObjectDefaultDeath)
		Explosion(object->pos, object->velo, 0, object->type->mass, true);
	object->type = type;
	object->typeRes = typeRes;
	if (type->creationSound)
		PlaySound(object->pos, object->velo, 1, 1, type->creationSound);
	return 1;
}

static void ProcessWorldPacket(const tWorldPacket *packet)
{
	UInt64 now = NetworkTimeMS();
	int i;
	if (!gGameOn || !gFirstObj || !gPlayerObj || packet->raceId != sRaceId ||
		packet->level != gLevelID || sPendingLevel >= 0)
		return;
	UpdateClock(0, packet->timeMS);
	for (i = 0; i < packet->count; ++i) {
		const tNetObject *entry = &packet->objects[i];
		tObject *object;
		tNetSample sample;
		int created = 0;
		if (!entry->id || IsTombstoned(entry->id, entry->typeRes, now))
			continue;
		object = sReplicas[entry->id];
		if (!object) {
			object = NewObject(gFirstObj, entry->typeRes);
			if (!object)
				continue;
			object->replica = 1;
			object->netId = entry->id;
			sReplicas[entry->id] = object;
			created = 1;
		} else if (object->typeRes != entry->typeRes &&
			!ChangeReplicaType(object, entry->typeRes)) {
			RemoveReplica(object);
			continue;
		}
		sample.pos = P2D(entry->x, entry->y);
		sample.velo = P2D(entry->veloX / 32.0f, entry->veloY / 32.0f);
		sample.dir = (UInt16)entry->dir * (2 * PI / 65536.0f);
		sample.time = packet->timeMS;
		PushSample(object->netSamples, &object->netSampleHead, &object->netSampleCount,
			&sample);
		object->netSeenMS = (UInt32)now;
		if (created) {
			object->pos = sample.pos;
			object->dir = sample.dir;
			object->velo = sample.velo;
		}
		object->control = entry->control;
		object->layer = entry->layer;
		object->jumpHeight = entry->jumpHeight / 32.0f;
		object->jumpVelo = 0;
		ApplyLook(object, entry->frame, entry->damageFlags, entry->damage, NULL);
	}
}

/* Each frame: follow the host's states (world states use the host clock). */
void Network_MoveReplica(tObject *object)
{
	tNetSample sample;
	if (!object->netSampleCount)
		return;
	SampleAt(object->netSamples, object->netSampleHead, object->netSampleCount,
		ShownTime(0), &sample);
	FollowSample(object, &sample);
	/* Smoke and fire of a wrecked car, as the host shows them. */
	if (object->type->flags2 & kObjectDamageble && !(gFrameCount % kLowCalcRatio) &&
		object->type->maxDamage > 0) {
		if (object->damage * 2 > object->type->maxDamage && RanProb(0.25))
			MakeSmoke(object);
		if (object->damage > object->type->maxDamage &&
			RanProb((object->damage - object->type->maxDamage) /
				object->type->maxDamage * 0.25f))
			Explosion(object->pos, object->velo, 0, 250, 0);
	}
}

/* Objects the host no longer sends were removed there. Far from this
   driver they may just have left the sent range: wait longer. */
static void ExpireReplicas(void)
{
	UInt64 now = NetworkTimeMS();
	tObject *driver = sLocalSlot >= 0 && gPlayers[sLocalSlot].obj ?
		gPlayers[sLocalSlot].obj : gPlayerObj;
	tObject *object = (tObject *)gFirstObj->next;
	while (object != gFirstObj) {
		tObject *next = (tObject *)object->next;
		if (object->replica) {
			int near = fabsf(object->pos.y - driver->pos.y) < kWorldRange - 300;
			if ((UInt32)now - object->netSeenMS > (near ? kReplicaTimeoutMS : kReplicaFarTimeoutMS))
				RemoveReplica(object);
		}
		object = next;
	}
}

/* This computer destroyed or collected a host object: the host does the
   same, and its older states must not bring the object back meanwhile. */
void Network_ReplicaKilled(tObject *object)
{
	tNetworkPlayer *state;
	tWorldKill *kill;
	sTombstones[sNextTombstone].id = object->netId;
	sTombstones[sNextTombstone].typeRes = object->typeRes;
	sTombstones[sNextTombstone].timeMS = NetworkTimeMS();
	sNextTombstone = (sNextTombstone + 1) % kTombstones;
	if (!Network_IsClient() || sLocalSlot <= 0)
		return;
	state = &sPlayers[sLocalSlot];
	state->killSeq++;
	kill = &state->kills[state->killSeq % kKillHistory];
	kill->seq = state->killSeq;
	kill->id = object->netId;
	kill->typeRes = object->typeRes;
}

void Network_ForgetReplica(tObject *object)
{
	if (sReplicas[object->netId] == object)
		sReplicas[object->netId] = NULL;
}

/* Host: a client destroyed one of our objects. Points and pickups were
   booked on the client already. */
static void KillWorldObject(UInt16 id, SInt16 typeRes)
{
	tObject *object;
	if (!gFirstObj || !gPlayerObj)
		return;
	for (object = (tObject *)gFirstObj->next; object != gFirstObj;
		object = (tObject *)object->next)
		if (object->netId == id) {
			/* Already destroyed here (e.g. by the replayed missile): skip. */
			if (object->typeRes == typeRes && IsWorldObject(object)) {
				gNoKillScore = 1;
				KillObject(object);
				gNoKillScore = 0;
			}
			return;
		}
}

static void ApplyRemoteKills(int slot)
{
	const tNetworkPlayer *state = &sPlayers[slot];
	if ((UInt8)(state->killSeq - sSeenKillSeq[slot]) > kKillHistory)
		sSeenKillSeq[slot] = (UInt8)(state->killSeq - kKillHistory);
	while (sSeenKillSeq[slot] != state->killSeq) {
		const tWorldKill *kill;
		sSeenKillSeq[slot]++;
		kill = &state->kills[sSeenKillSeq[slot] % kKillHistory];
		if (kill->seq == sSeenKillSeq[slot])
			KillWorldObject(kill->id, kill->typeRes);
	}
}

/* LAN: remember a missile or mine so the other computers can replay it. */
void Network_RecordShot(const tObject *shooter, const tObject *projectile,
	int weapon)
{
	int slot = GetSlotForPlayer(shooter);
	tPlayer *player;
	tShotEvent *shot;
	if (sSocket == kInvalidSocket || !IsLocalSlot(slot) || !projectile)
		return;
	player = &gPlayers[slot];
	player->shotSeq++;
	shot = &player->shots[player->shotSeq % kShotHistory];
	shot->seq = player->shotSeq;
	shot->weapon = (SInt16)weapon;
	shot->x = projectile->pos.x;
	shot->y = projectile->pos.y;
	shot->dir = projectile->dir;
	shot->veloX = projectile->velo.x;
	shot->veloY = projectile->velo.y;
	shot->jumpVelo = projectile->jumpVelo;
	/* Every computer replays it, so the world sync leaves it out. */
	((tObject *)projectile)->netLocal = 1;
}

static void ReplayShot(int slot, const tShotEvent *shot)
{
	tObject *shooter = sRemoteObjects[slot];
	tObject *projectile;
	if (!shooter || !gFirstObj)
		return;
	projectile = NewObject(shooter, shot->weapon);
	if (!projectile)
		return;
	projectile->pos = P2D(shot->x, shot->y);
	projectile->dir = shot->dir;
	projectile->velo = P2D(shot->veloX, shot->veloY);
	projectile->jumpVelo = shot->jumpVelo;
	projectile->shooter = shooter;
	projectile->netLocal = 1;
}

static void ReviveDriver(int slot)
{
	gPlayers[slot].lives = 1;
	gPlayers[slot].deathDelay = 0.01f; /* respawn on the next frame */
	sPlayers[slot].eliminated = 0;
}

static int FindGiftTarget(int donor)
{
	int slot;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (slot != donor && sPlayers[slot].active && sPlayers[slot].eliminated)
			return slot;
	return -1;
}

const char *Network_GetGiftTargetName(const tObject *donor)
{
	int slot = GetSlotForPlayer(donor), target;
	if (slot < 0 || Network_IsDuel() || gPlayers[slot].lives < 2)
		return NULL;
	target = FindGiftTarget(slot);
	if (target < 0)
		return NULL;
	return sPlayers[target].name[0] ? sPlayers[target].name : "Driver";
}

/* A finished co-op driver hands one of their lives to a driver who is out. */
int Network_GiveLife(const tObject *donor)
{
	int slot = GetSlotForPlayer(donor), target;
	if (!Network_GetGiftTargetName(donor))
		return 0;
	target = FindGiftTarget(slot);
	gPlayers[slot].lives--;
	if (IsLocalSlot(target))
		ReviveDriver(target);
	else {
		gPlayers[slot].giftTarget = (UInt8)target;
		gPlayers[slot].giftSeq++;
	}
	SimplePlaySound(154);
	return 1;
}

static void CreditKill(int shooter, const tObject *victim)
{
	tTextEffect fx;
	Str31 text = "\x04" "1000";
	int previous;
	gPlayers[shooter].score += kDuelKillScore;
	gPlayers[shooter].kills++;
	fx.x = victim->pos.x;
	fx.y = victim->pos.y;
	fx.effectFlags = kEffectExplode + kEffectTiny + kEffectAbsPos;
	MakeFXStringFromNumStr(text, fx.text);
	/* Bound to the shooter, so the points pop up in the shooter's view. */
	previous = BindNested(gPlayers[shooter].obj);
	NewTextEffect(&fx);
	RestoreBinding(previous);
}

/* Another driver's missile or mine took a driver out. In co-op it only costs
   the victim a life; in duel mode the shooter also scores. */
void Network_HitDriver(tObject *victim, const tObject *shooter)
{
	int victimSlot = GetSlotForPlayer(victim);
	int shooterSlot = GetSlotForPlayer(shooter);
	const tPlayer *target;
	if (victimSlot < 0 || shooterSlot < 0 || victimSlot == shooterSlot)
		return;
	target = &gPlayers[victimSlot];
	if (target->finishDelay || target->deathDelay || target->lives <= 0)
		return;
	/* The computer that drives the victim's car decides about hits; it
	   sees the car's true position. */
	if (!IsLocalSlot(victimSlot))
		return;
	/* Bound to the victim, so OUCH only shows in the victim's view. */
	Network_KillDriver(victim);
	if (!Network_IsDuel())
		return;
	if (IsLocalSlot(shooterSlot))
		CreditKill(shooterSlot, victim);
	else {
		gPlayers[victimSlot].creditTarget = (UInt8)shooterSlot;
		gPlayers[victimSlot].creditSeq++;
	}
}

/* Life gifts and duel hits from drivers on other computers. */
void Network_ProcessEvents(void)
{
	int slot;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		const tPlayer *player = &gPlayers[slot];
		if (!sPlayers[slot].active || IsLocalSlot(slot))
			continue;
		if (player->giftSeq != sSeenGiftSeq[slot]) {
			sSeenGiftSeq[slot] = player->giftSeq;
			if (IsLocalSlot(player->giftTarget) &&
				sPlayers[player->giftTarget].eliminated)
				ReviveDriver(player->giftTarget);
		}
		if (player->creditSeq != sSeenCreditSeq[slot]) {
			sSeenCreditSeq[slot] = player->creditSeq;
			if (IsLocalSlot(player->creditTarget) && sRemoteObjects[slot])
				CreditKill(player->creditTarget, sRemoteObjects[slot]);
		}
		/* Crates raised on another computer appear here too. */
		if (player->swarmSeq != sSeenSwarmSeq[slot]) {
			sSeenSwarmSeq[slot] = player->swarmSeq;
			if (player->swarmTarget < kNetworkMaxPlayers && !Network_IsClient())
				Extended_SpawnCopSwarm(gPlayers[player->swarmTarget].obj);
			if (player->swarmTarget < kNetworkMaxPlayers)
				MarkSwarm(player->swarmTarget);
		}
		if (player->jamSeq != sSeenJamSeq[slot]) {
			sSeenJamSeq[slot] = player->jamSeq;
			if (player->jamTarget < kNetworkMaxPlayers && !Network_IsClient())
				Extended_SpawnTrafficJam(gPlayers[player->jamTarget].obj);
		}
		if (sIsHost)
			ApplyRemoteKills(slot);
		else
			ApplyAwards(slot);
		/* Replay new shots; older ones than the history holds are lost. */
		if ((UInt8)(player->shotSeq - sSeenShotSeq[slot]) > kShotHistory)
			sSeenShotSeq[slot] = (UInt8)(player->shotSeq - kShotHistory);
		while (sSeenShotSeq[slot] != player->shotSeq) {
			const tShotEvent *shot;
			sSeenShotSeq[slot]++;
			shot = &player->shots[sSeenShotSeq[slot] % kShotHistory];
			if (shot->seq == sSeenShotSeq[slot])
				ReplayShot(slot, shot);
		}
	}
}

/* Crate: a cop swarm or traffic jam aimed at the race leader; in package
   mode the cop swarm hunts the driver who picked it up. Returns 0 when it
   cannot be used, e.g. the collector is the leader. */
int Network_TriggerCrate(const tObject *collector, int crate)
{
	int slot = GetSlotForPlayer(collector), leader = GetLeaderSlot();
	if (!Network_IsActive()) {
		/* One player, package mode: only the cop swarm makes sense. */
		if (crate != kCrateCopSwarm || !Network_IsPackageMode())
			return 0;
		Extended_SpawnCopSwarm((tObject *)collector);
		MarkSwarm(0);
		return 1;
	}
	if (crate == kCrateCopSwarm && Network_IsPackageMode())
		leader = slot;
	else if (leader == slot)
		return 0;
	if (slot < 0 || leader < 0)
		return 0;
	/* On a LAN only the host spawns them; the others see its copies. */
	if (crate == kCrateCopSwarm) {
		if (!Network_IsClient())
			Extended_SpawnCopSwarm(gPlayers[leader].obj);
		MarkSwarm(leader);
		gPlayers[slot].swarmTarget = (UInt8)leader;
		gPlayers[slot].swarmSeq++;
	} else {
		if (!Network_IsClient())
			Extended_SpawnTrafficJam(gPlayers[leader].obj);
		gPlayers[slot].jamTarget = (UInt8)leader;
		gPlayers[slot].jamSeq++;
	}
	return 1;
}

int Network_GetLocalScoreCount(void)
{
	if (!Network_IsActive())
		return 1;
	if (!Network_IsClient())
		return sLocalTwoPlayer ? sLocalPlayerCount : 1;
	return sLocalSlot >= 0 ? 1 : 0;
}

UInt32 Network_GetLocalScore(int index)
{
	int slot = sLocalTwoPlayer ? index : sLocalSlot;
	if (sLocalTwoPlayer && (index < 0 || index >= sLocalPlayerCount))
		return 0;
	if (!sLocalTwoPlayer && index != 0)
		return 0;
	if (slot < 0 || slot >= kNetworkMaxPlayers)
		return 0;
	return (UInt32)gPlayers[slot].score;
}

int Network_GetLocalInput(const tObject *object, tInputData *input)
{
	int slot = Network_GetLocalDriverSlot(object);
	int forward, backward, left, right;
	if (slot < 0 || !input)
		return 0;
	forward = Platform_IsKeyDown((SDL_Scancode)gPrefs.localKeyCodes[slot][kForward]);
	backward = Platform_IsKeyDown((SDL_Scancode)gPrefs.localKeyCodes[slot][kBackward]);
	left = Platform_IsKeyDown((SDL_Scancode)gPrefs.localKeyCodes[slot][kLeft]);
	right = Platform_IsKeyDown((SDL_Scancode)gPrefs.localKeyCodes[slot][kRight]);
	input->throttle = forward == backward ? 0 : (forward ? 1 : -1);
	input->steering = left == right ? 0 : (left ? -1 : 1);
	input->brake = 0;
	input->handbrake = 0;
	input->reverse = backward;
	input->kickdown = Platform_IsKeyDown(
		(SDL_Scancode)gPrefs.localKeyCodes[slot][kKickdown]);
	return 1;
}

/* ---- Placing drivers apart ---- */
#define kDriverGap 80.0f	/* cars are about 40 px long */
#define kGridRowGap 64.0f	/* five rows of two for ten drivers */

/* The carriageway around x at height y: start positions stay on it. */
static void GetCarriageway(float x, float y, float *left, float *right)
{
	int index = (int)(y / 2);
	tRoad road;
	if (index < 0)
		index = 0;
	if (index >= (int)*gRoadLenght)
		index = (int)*gRoadLenght - 1;
	road = gRoadData + index;
	if (x >= (*road)[2]) {
		*left = (*road)[2];
		*right = (*road)[3];
	} else if (x <= (*road)[1]) {
		*left = (*road)[0];
		*right = (*road)[1];
	} else {
		*left = (*road)[0];
		*right = (*road)[3];
	}
	*left += 16;
	*right -= 16;
}

static void SetDriverTarget(tObject *driver)
{
	driver->control = kObjectDriveUp;
	for (driver->target = 0;
		driver->target < (int)gTrackUp->num - 1 &&
		gTrackUp->track[driver->target].y < driver->pos.y;
		driver->target++);
}

/* Start grid: two cars side by side, rows behind each other; each slot has
   its own place, so computers that place only their own car agree. */
static void PlaceOnGrid(tObject *driver, int slot)
{
	float left, right, x = gLevelData->xStartPos, y = 500 - (slot / 2) * kGridRowGap;
	GetCarriageway(x, y, &left, &right);
	x += slot & 1 ? 32 : -32;
	if (x < left) x = left;
	if (x > right) x = right;
	driver->pos = P2D(x, y);
	driver->dir = 0;
	SetDriverTarget(driver);
}

static int DriverSpotIsFree(const tObject *driver, t2DPoint pos)
{
	int slot;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		const tObject *other = gPlayers[slot].obj;
		if (other && other != driver && sPlayers[slot].active &&
			VEC2D_Value(VEC2D_Difference(other->pos, pos)) < kDriverGap)
			return 0;
	}
	return 1;
}

/* A respawned driver must not appear on top of another one: try beside,
   then further back. */
void Network_PlaceDriverApart(tObject *driver)
{
	static const float kSideOffsets[] = {0, 30, -30, 60, -60};
	int row, side;
	if (!Network_IsActive() || !driver)
		return;
	for (row = 0; row < 6; ++row)
		for (side = 0; side < 5; ++side) {
			t2DPoint pos = P2D(driver->pos.x + kSideOffsets[side],
				driver->pos.y - row * kDriverGap);
			float left, right;
			if (pos.y < 100)
				continue;
			GetCarriageway(driver->pos.x, pos.y, &left, &right);
			if (pos.x < left || pos.x > right)
				continue;
			if (DriverSpotIsFree(driver, pos)) {
				if (row || side) {
					driver->pos = pos;
					SetDriverTarget(driver);
				}
				return;
			}
		}
}

void Network_LevelLoaded(void)
{
	int slot;
	if (!Network_IsActive() || !gPlayerObj || !gFirstObj ||
		sLocalSlot < 0 || sLocalSlot >= kNetworkMaxPlayers)
		return;
	gPlayers[sLocalSlot].obj = gPlayerObj;
	if (Network_IsClient()) {
		/* The world comes from the host; this computer's random traffic,
		   cops and pickups would only differ from it. */
		tObject *object = (tObject *)gFirstObj->next;
		while (object != gFirstObj) {
			tObject *next = (tObject *)object->next;
			if (object != gPlayerObj) {
				SpriteUnused(object->frame);
				RemoveObject(object);
			}
			object = next;
		}
	}
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		if (!sPlayers[slot].active)
			continue;
		sPlayers[slot].finished = 0;
		sPlayers[slot].eliminated = gPlayers[slot].lives <= 0;
		sPlayers[slot].finishDelay = 0;
		/* On a LAN the other drivers' names arrive from their computers. */
		if (sLocalTwoPlayer || slot == sLocalSlot)
			CopyLocalName(sPlayers[slot].name, slot);
	}
	if (!sLocalTwoPlayer) {
		PlaceOnGrid(gPlayerObj, sLocalSlot);
		return;
	}
	PlaceOnGrid(gPlayerObj, 0);
	for (slot = 1; slot < sLocalPlayerCount; ++slot) {
		int vehicle = gRoadInfo && gRoadInfo->water ?
			kNormalPlayerBoatID : Network_GetPlayerVehicle(slot);
		tObject *driver = NewObject(gFirstObj, (SInt16)vehicle);
		if (!driver) {
			fprintf(stderr, "[multiplayer] Could not create local player %d vehicle\n",
				slot + 1);
			sPlayers[slot].active = 0;
			continue;
		}
		PlaceOnGrid(driver, slot);
		gPlayers[slot].obj = driver;
		sPlayers[slot].active = 1;
	}
	sLocalPlayer2 = gPlayers[1].obj;
}

/* Called when a level is loaded: on a LAN the race clock starts at the
   countdown and stands until every computer has loaded the level. */
void Network_PrepareRaceClock(void)
{
	if (sSocket == kInvalidSocket || sLocalSlot < 0)
		return;
	gGameTime = -kCountdownSeconds;
	sPlayers[sLocalSlot].readyLevel = (UInt8)(gLevelID + 1);
	sHostRaceKnown = 0;
	sRaceHolding = 1;
	sHoldStartMS = NetworkTimeMS();
}

/* Drivers stand still until GO. */
int Network_RaceHeld(void)
{
	return sSocket != kInvalidSocket && gGameTime < 0;
}

static int AllDriversReady(void)
{
	int slot;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active && sPlayers[slot].readyLevel != gLevelID + 1)
			return 0;
	return 1;
}

/* LAN race clock, once per frame; returns 0 when not on a LAN. */
int Network_AdvanceRaceClock(void)
{
	if (sSocket == kInvalidSocket)
		return 0;
	if (sIsHost) {
		sRaceHolding = !Network_UnfinishedDriverCount() ||
			(gGameTime <= -kCountdownSeconds && !AllDriversReady() &&
				NetworkTimeMS() - sHoldStartMS < kStartWaitMS);
		if (!sRaceHolding)
			gGameTime += kFrameDuration;
		return 1;
	}
	/* Client: the host's clock, carried on between its states and eased in
	   so it never jumps back over a countdown step. */
	if (!sHostRaceKnown)
		return 1;
	{
		float target = sHostRaceTime;
		if (!sHostRaceHold)
			target += (float)((sSimClockMS - sHostRaceTimeAt) / 1000.0);
		if (sHostRaceHold) {
			gGameTime = target;
		} else {
			float error;
			gGameTime += kFrameDuration;
			error = target - gGameTime;
			if (fabsf(error) > 0.5f)
				gGameTime = target;
			else
				gGameTime += error * 0.05f;
		}
	}
	return 1;
}

int Network_GameHasStarted(void)
{
	return sStarted && (!Network_IsClient() || sStartingFromNetwork);
}

void Network_SetGameStarted(void)
{
	if (sIsHost) {
		if (!sStartedByHost)
			sRaceId++;
		sStartedByHost = 1;
		sStarted = 1;
	}
}

void Network_GameEnded(void)
{
	sStarted = 0;
	if (sIsHost) {
		sStartedByHost = 0;
		SendHostState();
	}
}

/* A remote driver's state arrived (directly or relayed by the host). */
static void RecordPlayerSample(int slot)
{
	const tNetworkPlayer *state = &sPlayers[slot];
	tNetSample sample;
	if (!state->active || !state->timeMS)
		return;
	if (sPlayerSampleCount[slot] &&
		(SInt32)(state->timeMS - sPlayerSamples[slot][sPlayerSampleHead[slot]].time) <= 0)
		return;
	UpdateClock(slot, state->timeMS);
	sample.pos = P2D(state->x, state->y);
	sample.velo = P2D(state->velocityX, state->velocityY);
	sample.dir = state->dir;
	sample.time = state->timeMS;
	PushSample(sPlayerSamples[slot], &sPlayerSampleHead[slot], &sPlayerSampleCount[slot],
		&sample);
}

static void ProcessHostPacket(const tNetworkPacket *packet, const struct sockaddr_in *sender)
{
	int slot = FindPeer(sender);
	if (packet->type == kPacketDiscover) {
		tNetworkPacket response;
		int nameLength = gPrefs.playerNames[0][0];
		InitializePacket(&response, kPacketAnnounce);
		response.raceId = sHostToken;	/* announce: raceId carries the host token */
		response.slot = (UInt8)sLobbyPlayerLimit;
		response.started = (UInt8)sStartedByHost;
		response.playerCount = (UInt8)Network_GetPlayerCount();
		if (nameLength > 15)
			nameLength = 15;
		memcpy(response.players[0].name, gPrefs.playerNames[0] + 1, (size_t)nameLength);
		SendPacket(sender, &response);
		return;
	}
	if (packet->type == kPacketHello) {
		tNetworkPacket response;
		if (slot < 0) {
			/* A computer that joins again with a new socket (after leaving
			   without notice or a restart) takes over its old slot instead
			   of finding the lobby full. */
			int old;
			for (old = 1; old < kNetworkMaxPlayers && slot < 0; ++old)
				if (sPeers[old].used &&
					sPeers[old].address.sin_addr.s_addr == sender->sin_addr.s_addr &&
					NetworkTimeMS() - sPeers[old].lastSeen > 2500) {
					slot = old;
					sPeers[old].address = *sender;
					fprintf(stderr, "[network] Player rejoined slot %d from a new socket\n",
						slot + 1);
				}
		}
		if (slot < 0)
			slot = AddPeer(sender);
		if (slot < 0) {
			fprintf(stderr, "[network] Lobby is full; rejected incoming player\n");
			return;
		}
		sPeers[slot].lastSeen = NetworkTimeMS();
		if (packet->players[0].vehicle > 0)
			sPlayers[slot].vehicle = packet->players[0].vehicle;
		if (packet->players[0].name[0])
			memcpy(sPlayers[slot].name, packet->players[0].name,
				sizeof(sPlayers[slot].name));
		InitializePacket(&response, kPacketWelcome);
		response.slot = (UInt8)slot;
		response.started = (UInt8)sStartedByHost;
		response.raceId = sRaceId;
		response.level = (UInt8)gLevelID;
		SendPacket(sender, &response);
		return;
	}
	/* A driver whose computer stalled (and timed out) comes back into its
	   old slot instead of being ignored for the rest of the game. */
	if (slot < 0 && packet->type == kPacketState && packet->slot >= 1 &&
		packet->slot < sLobbyPlayerLimit && !sPeers[packet->slot].used) {
		slot = packet->slot;
		sPeers[slot].address = *sender;
		sPeers[slot].used = 1;
		fprintf(stderr, "[network] Player rejoined slot %d\n", slot + 1);
	}
	if (slot < 1 || packet->slot != slot)
		return;
	sPeers[slot].lastSeen = NetworkTimeMS();
	if (packet->type == kPacketLeave) {
		sPeers[slot].used = 0;
		memset(&sPlayers[slot], 0, sizeof(sPlayers[slot]));
		fprintf(stderr, "[network] Player left slot %d\n", slot + 1);
	} else if (packet->type == kPacketState && packet->players[slot].active &&
		packet->level == (UInt8)gLevelID) {
		/* A late state from the level before (still "finished") would
		   otherwise come back to the client and keep it finished in the
		   new level for good. */
		sPlayers[slot] = packet->players[slot];
		sPlayers[slot].active = 1;
		sSnapshotMS[slot] = NetworkTimeMS();
		UnpackPlayer(slot);
		RecordPlayerSample(slot);
	}
}

static void ProcessClientPacket(const tNetworkPacket *packet, const struct sockaddr_in *sender)
{
	int slot, wasStarted = sStarted;
	int levelChanged;
	if (!SameAddress(sender, &sServerAddress))
		return;
	if (packet->type == kPacketWelcome) {
		if (packet->slot > 0 && packet->slot < kNetworkMaxPlayers) {
			sLocalSlot = packet->slot;
			sPlayers[sLocalSlot].vehicle = Network_GetLocalVehicle();
			sRaceId = packet->raceId;
			sStarted = packet->started != 0;
			if (sStarted && packet->level < 10)
				sPendingLevel = packet->level;
			if (sStarted && sRaceId != sLaunchedRace && !gGameOn)
				sStartPending = 1;
		}
		return;
	}
	if (packet->type != kPacketState || packet->playerCount > kNetworkMaxPlayers)
		return;
	levelChanged = gGameOn && packet->level < 10 && packet->level != gLevelID;
	sHostMode = packet->duel < kGameModeCount ? packet->duel : kGameModeCoop;
	memcpy(sHostWeights, packet->crateWeights, sizeof(sHostWeights));
	sHostTheme = packet->theme;
	sHostThemeMode = packet->themeMode < kThemeFixed + kCrateThemeCount ?
		packet->themeMode : kThemeOff;
	/* Show the host's theme once per level. */
	if (gGameOn && !levelChanged && sHostTheme >= 0 && sHostTheme < kCrateThemeCount &&
		sHostTheme != sShownTheme) {
		sShownTheme = sHostTheme;
		AnnounceTheme(sHostTheme);
	}
	sHostPackageAmount = packet->packageAmount < kPackageAmountCount ?
		packet->packageAmount : kPackagesNormal;
	if (gGameOn && !levelChanged && packet->raceId == sRaceId && packet->started) {
		sHostRaceTime = packet->raceTime;
		sHostRaceTimeAt = sSimClockMS;
		sHostRaceHold = packet->raceHold;
		sHostRaceKnown = 1;
	}
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		if (slot != sLocalSlot || levelChanged) {
			UInt8 killSeq = sPlayers[slot].killSeq;
			tWorldKill kills[kKillHistory];
			memcpy(kills, sPlayers[slot].kills, sizeof(kills));
			sPlayers[slot] = packet->players[slot];
			if (slot == sLocalSlot) {
				sPlayers[slot].killSeq = killSeq;
				memcpy(sPlayers[slot].kills, kills, sizeof(kills));
			}
			sSnapshotMS[slot] = NetworkTimeMS();
			UnpackPlayer(slot);
			if (slot != sLocalSlot)
				RecordPlayerSample(slot);
		} else {
			sPlayers[slot].finished = packet->players[slot].finished;
			sPlayers[slot].eliminated = packet->players[slot].eliminated;
		}
	}
	sPlayers[0].active = 1;
	sStarted = packet->started != 0;
	sRaceId = packet->raceId;
	if (wasStarted && !sStarted && gGameOn)
		gEndGame = 1;
	if (levelChanged)
		sPendingLevel = packet->level;
	if (sStarted && sLocalSlot > 0 && sRaceId != sLaunchedRace && !gGameOn)
		sStartPending = 1;
}

/* A client joining a running campaign starts in the host's level (and
   does not load it a second time). */
int Network_TakeHostLevel(void)
{
	int level = sPendingLevel;
	if (!Network_IsClient() || level < 0 || level >= 10)
		return -1;
	sPendingLevel = -1;
	return level;
}

void Network_Poll(void)
{
	union {
		tNetworkPacket state;
		tWorldPacket world;
	} buffer;
	tNetworkPacket packet;
	struct sockaddr_in sender;
	NetworkSockLen senderLength = (NetworkSockLen)sizeof(sender);
	UInt64 now = NetworkTimeMS();
	if (sSocket == kInvalidSocket)
		return;
	for (;;) {
		int received = (int)recvfrom(sSocket, (char *)&buffer, (int)sizeof(buffer), 0,
			(struct sockaddr *)&sender, &senderLength);
		if (received < 0) {
#ifdef _WIN32
			if (WSAGetLastError() != WSAEWOULDBLOCK)
				fprintf(stderr, "[network] UDP receive failed: %d\n", WSAGetLastError());
#else
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				perror("[network] UDP receive failed");
#endif
			break;
		}
		if (received >= (int)offsetof(tWorldPacket, objects) &&
			buffer.world.magic == kNetworkMagic && buffer.world.version == kNetworkVersion &&
			buffer.world.type == kPacketWorld) {
			if (!sIsHost && SameAddress(&sender, &sServerAddress) &&
				buffer.world.count <= kWorldChunk &&
				received == (int)offsetof(tWorldPacket, objects) +
					buffer.world.count * (int)sizeof(tNetObject))
				ProcessWorldPacket(&buffer.world);
			senderLength = (NetworkSockLen)sizeof(sender);
			continue;
		}
		if (received != (int)sizeof(packet)) {
			senderLength = (NetworkSockLen)sizeof(sender);
			continue;
		}
		memcpy(&packet, &buffer.state, sizeof(packet));
		if (packet.magic != kNetworkMagic || packet.version != kNetworkVersion ||
			packet.packetSize != (UInt16)sizeof(packet)) {
			senderLength = (NetworkSockLen)sizeof(sender);
			continue;
		}
		if (sIsHost)
			ProcessHostPacket(&packet, &sender);
		else
			ProcessClientPacket(&packet, &sender);
		senderLength = (int)sizeof(sender);
	}
	if (!sIsHost && (!gGameOn || sLocalSlot < 0) &&
		now - sLastHelloMS >= (sLocalSlot < 0 ? 500 : 1000)) {
		InitializePacket(&packet, kPacketHello);
		/* hello: players[0] carries this driver's name and lobby vehicle */
		packet.players[0].vehicle = Network_GetLocalVehicle();
		CopyLocalName(packet.players[0].name, sLocalSlot);
		SendPacket(&sServerAddress, &packet);
		sLastHelloMS = now;
	}
	if (sIsHost) {
		int slot;
		/* Packets received above stamp lastSeen later than now. */
		now = NetworkTimeMS();
		for (slot = 1; slot < kNetworkMaxPlayers; ++slot) {
			if (sPeers[slot].used && now > sPeers[slot].lastSeen + kNetworkTimeoutMS) {
				sPeers[slot].used = 0;
				memset(&sPlayers[slot], 0, sizeof(sPlayers[slot]));
				fprintf(stderr, "[network] Player in slot %d timed out\n", slot + 1);
			}
		}
	}
	if (sStartPending) {
		sStartPending = 0;
		if (sStarted && sRaceId != sLaunchedRace && sLocalSlot > 0) {
			sLaunchedRace = sRaceId;
			sStartingFromNetwork = 1;
			StartGame(0);
			sStartingFromNetwork = 0;
		}
	}
	if (sPendingLevel >= 0 && gGameOn) {
		int level = sPendingLevel;
		sPendingLevel = -1;
		DisposeLevel();
		gLevelID = level;
		if (!LoadLevel())
			gEndGame = 1;
	}
}

void Network_Publish(void)
{
	UInt64 now = NetworkTimeMS();
	tNetworkPacket packet;
	if (sSocket == kInvalidSocket || now - sLastSendMS < kNetworkIntervalMS)
		return;
	if (sIsHost) {
		if (gGameOn)
			ReadLocalPlayer();
		SendHostState();
		if (gGameOn && gFirstObj && gPlayerObj)
			SendWorld();
	} else if (gGameOn && sLocalSlot > 0) {
		ReadLocalPlayer();
		InitializePacket(&packet, kPacketState);
		packet.slot = (UInt8)sLocalSlot;
		packet.started = (UInt8)sStarted;
		packet.level = (UInt8)gLevelID;	/* the host drops states from another level */
		packet.playerCount = 1;
		packet.players[sLocalSlot] = sPlayers[sLocalSlot];
		SendPacket(&sServerAddress, &packet);
	}
	sLastSendMS = now;
}

int Network_GetPlayerCount(void)
{
	int count = 0, slot;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (sPlayers[slot].active)
			count++;
	return count;
}

static int GetActiveSlot(int viewIndex)
{
	int slot, index = 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		if (!sPlayers[slot].active)
			continue;
		if (index++ == viewIndex)
			return slot;
	}
	return -1;
}

tObject *Network_GetCameraForView(int viewIndex)
{
	int slot = GetActiveSlot(viewIndex);
	if (slot < 0 || slot == sLocalSlot)
		return gPlayerObj;
	if (sLocalTwoPlayer && slot < sLocalPlayerCount)
		return gPlayers[slot].obj ? gPlayers[slot].obj : gPlayerObj;
	return sRemoteObjects[slot] ? sRemoteObjects[slot] : gPlayerObj;
}

int Network_IsRemoteObject(const tObject *object)
{
	int slot;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (object && object == sRemoteObjects[slot])
			return 1;
	return 0;
}

static void RemoveRemoteObject(int slot)
{
	tObject *object = sRemoteObjects[slot];
	if (!object)
		return;
	/* Nothing may keep pointing at the freed car (a driver who timed out
	   and later rejoins gets a new one). */
	if (gPlayers[slot].obj == object)
		gPlayers[slot].obj = NULL;
	RemoveDriverExtras(slot);
	SpriteUnused(object->frame);
	RemoveObject(object);
	sRemoteObjects[slot] = NULL;
}

void Network_SyncObjects(void)
{
	int slot;
	sSimClockMS += 1000.0 * kFrameDuration;
	if (!Network_IsActive() || !gFirstObj || !gPlayerObj)
		return;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		tObject *object;
		tNetworkPlayer *player = &sPlayers[slot];
		if (slot == sLocalSlot)
			continue;
		if (sLocalTwoPlayer && slot < sLocalPlayerCount)
			continue;
		if (!player->active) {
			RemoveRemoteObject(slot);
			continue;
		}
		object = sRemoteObjects[slot];
		if (object && sRemoteVehicles[slot] != player->vehicle) {
			RemoveRemoteObject(slot);
			object = NULL;
		}
		if (!object) {
			int vehicle = player->vehicle > 0 ? player->vehicle :
				(gRoadInfo && gRoadInfo->water ? kNormalPlayerBoatID : kNormalPlayerCarID);
			object = NewObject(gFirstObj, (SInt16)vehicle);
			if (!object) {
				fprintf(stderr, "[network] Could not create remote player's vehicle\n");
				continue;
			}
			sRemoteObjects[slot] = object;
			sRemoteVehicles[slot] = vehicle;
			gPlayers[slot].obj = object;
			object->control = kObjectNoInput;
			object->pos = P2D(player->x, player->y);
			object->dir = player->dir;
		}
		if (sPlayerSampleCount[slot]) {
			tNetSample sample;
			SampleAt(sPlayerSamples[slot], sPlayerSampleHead[slot], sPlayerSampleCount[slot],
				ShownTime(slot), &sample);
			FollowSample(object, &sample);
		} else {
			object->pos = P2D(player->x, player->y);
			object->dir = player->dir;
			object->velo = P2D(player->velocityX, player->velocityY);
		}
		/* Dents made here by collisions are replaced by the reported ones. */
		ApplyLook(object, player->frame, player->damageFlags, player->damage, player->dents);
		object->jumpHeight += (player->jumpHeight - object->jumpHeight) * 0.5f;
		if (player->jumpHeight == 0)
			object->jumpHeight = 0;
		RemoteDriverEffects(slot, object);
		Player_UpdateZoom(&gPlayers[slot], object);
	}
	if (Network_IsClient())
		ExpireReplicas();
}

void Network_ResetLevel(void)
{
	int slot;
	memset(sRemoteObjects, 0, sizeof(sRemoteObjects));
	memset(sRemoteVehicles, 0, sizeof(sRemoteVehicles));
	memset(sReplicas, 0, sizeof(sReplicas));
	memset(sTombstones, 0, sizeof(sTombstones));
	memset(sPlayerSampleCount, 0, sizeof(sPlayerSampleCount));
	memset(sWheelPosValid, 0, sizeof(sWheelPosValid));
	memset(sSwarmUntil, 0, sizeof(sSwarmUntil));
	/* Not ready for the next level until it is loaded. */
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		sPlayers[slot].readyLevel = 0;
	memset(sSeenDeathDelay, 0, sizeof(sSeenDeathDelay));
	sLocalPlayer2 = NULL;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		gPlayers[slot].obj = NULL;
		gPlayers[slot].spikeObj = NULL;
		gPlayers[slot].brakeObj = NULL;
	}
}

void Network_Shutdown(void)
{
	if (Network_IsActive() && !sIsHost && sLocalSlot > 0) {
		/* Tell the host the slot is free (more than once: UDP may lose it). */
		int i;
		for (i = 0; i < 3; ++i) {
			tNetworkPacket packet;
			InitializePacket(&packet, kPacketLeave);
			packet.slot = (UInt8)sLocalSlot;
			SendPacket(&sServerAddress, &packet);
		}
	}
	if (sSocket != kInvalidSocket) {
		NetworkCloseSocket(sSocket);
		sSocket = kInvalidSocket;
	}
#ifdef _WIN32
	if (sWinsockInitialized) {
		WSACleanup();
		sWinsockInitialized = 0;
	}
#endif
	sIsHost = 0;
	sLocalSlot = -1;
	sLocalTwoPlayer = 0;
	sLocalPlayerCount = 2;
	sStarted = 0;
	sStartedByHost = 0;
	sStartPending = 0;
	sStartingFromNetwork = 0;
	sRaceId = 0;
	sLaunchedRace = 0;
	memset(sPeers, 0, sizeof(sPeers));
	memset(sPlayers, 0, sizeof(sPlayers));
	memset(sRemoteObjects, 0, sizeof(sRemoteObjects));
	memset(sRemoteVehicles, 0, sizeof(sRemoteVehicles));
	memset(sReplicas, 0, sizeof(sReplicas));
	/* Nothing of this session may leak into the next one. */
	memset(sSeenGiftSeq, 0, sizeof(sSeenGiftSeq));
	memset(sSeenCreditSeq, 0, sizeof(sSeenCreditSeq));
	memset(sSeenShotSeq, 0, sizeof(sSeenShotSeq));
	memset(sSeenSwarmSeq, 0, sizeof(sSeenSwarmSeq));
	memset(sSeenJamSeq, 0, sizeof(sSeenJamSeq));
	memset(sSeenKillSeq, 0, sizeof(sSeenKillSeq));
	memset(sSeenAwardSeq, 0, sizeof(sSeenAwardSeq));
	memset(sPlayerSampleCount, 0, sizeof(sPlayerSampleCount));
	memset(sClockKnown, 0, sizeof(sClockKnown));
	sLobbyPlayerLimit = kNetworkMaxPlayers;
	sPendingLevel = -1;
	sHostRaceKnown = 0;
	sHostMode = kGameModeCoop;
	sBoundSlot = -1;
	sLastHelloMS = 0;
	sLocalPlayer2 = NULL;
}

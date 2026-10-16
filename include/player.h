#ifndef __PLAYER_H
#define __PLAYER_H

#include "objects.h"

#define kMaxPlayers 4
#define kShotHistory 4

/* A missile or mine fired by a driver, replayed on the other computers. */
typedef struct {
	UInt8 seq;
	SInt16 weapon;
	float x,y,dir,veloX,veloY,jumpVelo;
} tShotEvent;

/* Everything that belongs to one driver. Gameplay code works on gCurPlayer;
   multiplayer switches drivers by pointing gCurPlayer at another slot. */
typedef struct {
	tObject *obj;               /* driver's vehicle, NULL while not in a level */
	tObject *spikeObj,*brakeObj;
	int lives,extraLives;
	int score,displayScore;
	int bonus;
	int missiles,mines;
	UInt32 addOns;
	float deathDelay,finishDelay;
	float slide[4];
	float spikeFrame;
	float zoomVelo;             /* smoothed speed that drives camera distance */
	float gripTime;             /* super grip crate: seconds left */
	float offroadTime;          /* offroad tyres crate: seconds left */
	float magnetTime;           /* magnet crate: pulls in packages */
	float ramTime;              /* battering ram crate: rammed cars explode */
	float oilTime;              /* oil slick crate: drops oil behind the car */
	float nitroTime;            /* nitro crate: strong push */
	float rainTime;             /* rain crate: less grip for everyone else */
	float zombieTime;           /* zombie crate: traffic hunts the others */
	float warTime;              /* car war crate: traffic rams each other */
	float dodgeTime;            /* clear road crate: traffic gives way */
	float shieldGrace;          /* shield just used up: no harm for a moment */
	int shield;                 /* shield crate: the next crash costs nothing */
	unsigned long pickupTextFrame; /* frame the last pickup name was shown */
	int pickupTextRow;          /* its row at the top of the view */
	/* level completion sequence */
	float finishTime;           /* race clock when this driver crossed the line */
	int scoreBeforeBonus;
	int bonusCount;             /* ticks played by the bonus counter sound */
	int resultRecorded;         /* time bonus booked and high score checked */
	int highScoreRank;
	int finishPlace;            /* 1 = first across the line, 0 = not finished */
	/* multiplayer */
	int kills;                  /* duel mode: rivals taken out this campaign */
	int fireHeld;               /* fire was down last frame (life gift edge) */
	unsigned long switchDelayStart; /* gear change timing (input.c) per driver */
	UInt8 giftTarget,giftSeq;   /* last life given away, for remote drivers */
	UInt8 creditTarget,creditSeq; /* duel: rival whose weapon took this driver out */
	UInt8 shotSeq;              /* weapons fired so far; newest in shots[] */
	UInt8 swarmTarget,swarmSeq; /* last cop swarm crate, for remote drivers */
	UInt8 jamTarget,jamSeq;     /* last traffic jam crate, for remote drivers */
	tShotEvent shots[kShotHistory];
} tPlayer;

extern tPlayer gPlayers[kMaxPlayers];
extern tPlayer *gCurPlayer;

void Player_ResetForCampaign(tPlayer *player);
void Player_ResetForLevel(tPlayer *player);
void Player_UpdateZoom(tPlayer *player,const tObject *vehicle);

#endif

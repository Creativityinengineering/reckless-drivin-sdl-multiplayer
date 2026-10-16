#include <string.h>
#include "player.h"
#include "renderframe.h"
#include "vec2d.h"

#define kZoomAcceleration	80.0

tPlayer gPlayers[kMaxPlayers];
tPlayer *gCurPlayer=&gPlayers[0];

void Player_ResetForCampaign(tPlayer *player)
{
	tObject *obj=player->obj;
	memset(player,0,sizeof(*player));
	player->obj=obj;
	player->lives=3;
	player->bonus=1;
	player->zoomVelo=kMaxZoomVelo;
}

void Player_ResetForLevel(tPlayer *player)
{
	player->obj=nil;
	player->spikeObj=nil;
	player->brakeObj=nil;
	player->deathDelay=0;
	player->finishDelay=0;
	player->finishTime=0;
	player->scoreBeforeBonus=player->score;
	player->bonusCount=0;
	player->resultRecorded=0;
	player->highScoreRank=0;
	player->finishPlace=0;
	player->bonus=1;
	player->displayScore=player->score;
	memset(player->slide,0,sizeof(player->slide));
	player->zoomVelo=kMaxZoomVelo;
	player->gripTime=0;
	player->offroadTime=0;
	player->magnetTime=0;
	player->ramTime=0;
	player->oilTime=0;
	player->nitroTime=0;
	player->rainTime=0;
	player->zombieTime=0;
	player->warTime=0;
	player->dodgeTime=0;
	player->shieldGrace=0;
	player->shield=0;
	player->pickupTextFrame=0;
	player->pickupTextRow=0;
}

/* Ease the camera distance towards the vehicle's speed so that bumps and
   collisions do not make the view (and the road texture) jump. */
void Player_UpdateZoom(tPlayer *player,const tObject *vehicle)
{
	float velo;
	if(!vehicle)
		return;
	velo=VEC2D_Value(vehicle->velo);
	if(player->zoomVelo>velo)
	{
		player->zoomVelo-=kZoomAcceleration*kFrameDuration;
		if(player->zoomVelo<velo)
			player->zoomVelo=velo;
	}
	else
	{
		player->zoomVelo+=kZoomAcceleration*kFrameDuration;
		if(player->zoomVelo>velo)
			player->zoomVelo=velo;
	}
	if(player->zoomVelo>kMaxZoomVelo)
		player->zoomVelo=kMaxZoomVelo;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include "objects.h"
#include "renderframe.h"
#include "screen.h"
#include "input.h"
#include "gamesounds.h"
#include "textfx.h"
#include "route.h"
#include "roads.h"
#include "gameinitexit.h"
#include "packs.h"
#include "random.h"
#include "netgame.h"
#include "platform.h"
#include "high.h"
#include "preferences.h"

#define kCalcFPMS			(kCalcFPS/(float)1000000)
#define kGraphFrameCount 	12
#define kDisplayScoreIncr	360

#define kExtraLiveScore		5000
#define kPenaltyTime		0 //penalty seconds for loosing a life.
#define kMinCopDist			1200	//minumum distance resurrecting player has from cops in pixels

extern UInt64 GetMSTime(void);

UInt64 gStartMS,gPauseMS,gLastGraphFrameMS[kGraphFrameCount];
unsigned long gFrameCount,gGraphFrameCount;
int gEndGame;

void InitFrameCount()
{
	gFrameCount=0;
	gStartMS=GetMSTime();
}

void PauseFrameCount()
{
	gPauseMS=GetMSTime();
}

void ResumeFrameCount()
{
	gStartMS+=GetMSTime()-gPauseMS;
}

static inline Boolean CheckFrameTime()
{
	unsigned long optFrameCount;
	UInt64 curMS;

	curMS=GetMSTime()-gStartMS;
	optFrameCount=curMS*kCalcFPMS;
	if(gFrameCount>optFrameCount)
	{
		BlockMoveData(gLastGraphFrameMS+1,gLastGraphFrameMS,sizeof(UInt64)*(kGraphFrameCount-1));
		gLastGraphFrameMS[kGraphFrameCount-1]=curMS;
		return true;
	}
	return false;
}

/* Set RD_FRAMESTATS=1 to log render cost and skipped frames every 2 seconds. */
static struct {
	int enabled;
	UInt64 windowStart,renderTotal,renderMax;
	unsigned long simFrames,renderedFrames;
} sFrameStats={-1};

static void FrameStatsRecord(int rendered,UInt64 renderMicros)
{
	UInt64 now;
	if(sFrameStats.enabled<0)
	{
		const char *env=getenv("RD_FRAMESTATS");
		sFrameStats.enabled=env&&env[0]&&env[0]!='0';
	}
	if(!sFrameStats.enabled)
		return;
	now=Platform_GetMicroseconds();
	if(!sFrameStats.windowStart)
		sFrameStats.windowStart=now;
	sFrameStats.simFrames++;
	if(rendered)
	{
		sFrameStats.renderedFrames++;
		sFrameStats.renderTotal+=renderMicros;
		if(renderMicros>sFrameStats.renderMax)
			sFrameStats.renderMax=renderMicros;
	}
	if(now-sFrameStats.windowStart>=2000000)
	{
		float seconds=(now-sFrameStats.windowStart)/1000000.0f;
		fprintf(stderr,"[framestats] sim %.1f fps, drawn %.1f fps, skipped %lu, "
			"render avg %.2f ms max %.2f ms, views %d, scale %d\n",
			sFrameStats.simFrames/seconds,sFrameStats.renderedFrames/seconds,
			sFrameStats.simFrames-sFrameStats.renderedFrames,
			sFrameStats.renderedFrames?sFrameStats.renderTotal/1000.0f/sFrameStats.renderedFrames:0.0f,
			sFrameStats.renderMax/1000.0f,
			Network_IsActive()?Network_GetPlayerCount():1,
			Platform_GetRenderScale());
		memset(&sFrameStats,0,sizeof(sFrameStats));
		sFrameStats.enabled=1;
		sFrameStats.windowStart=now;
	}
}

static inline void CheckTimeSkip()
{
	unsigned long optFrameCount;
	UInt64 curMS;
	do{
		curMS=GetMSTime()-gStartMS;
		optFrameCount=curMS*kCalcFPMS;
	}while(gFrameCount>optFrameCount);
}

t2DPoint GetUniquePos(SInt16 minOffs,SInt16 maxOffs,float *objDir,int *dir);
t2DPoint GetUniquePosLimited(SInt16 minOffs,SInt16 maxOffs,float *objDir,int *dir,
	int maxTries,int *found);

#define kRespawnGrace 2.5f

void CopClear()
{
	tObject	*theObj=(tObject*)(gFirstObj->next);
	while(theObj!=gFirstObj)
	{
		if(theObj!=gPlayerObj&&theObj->type->flags&kObjectCop)
		{
			int tries=0,found=1;
			/* Bounded: near the level end there may be no spot far enough
			   away, and with many cars on the road no free spot at all (the
			   search used to run forever then); the cop then stays. */
			while(fabs(theObj->pos.y-gPlayerObj->pos.y)<kMinCopDist&&tries++<50&&found)
			{
				t2DPoint oldPos=theObj->pos;
				float oldDir=theObj->dir;
				int oldControl=theObj->control;
				theObj->dir=-1;
				theObj->pos=GetUniquePosLimited(20,200,&theObj->dir,&theObj->control,200,&found);
				if(!found)
				{
					theObj->pos=oldPos;
					theObj->dir=oldDir;
					theObj->control=oldControl;
				}
			}
		}
		theObj=(tObject*)theObj->next;
	}
}

void ResurrectPlayer()
{
	tObject *oldPlayer = gPlayerObj;
	tObject *newPlayer;
	int i, trackCount = gTrackUp ? gTrackUp->num : 0;
	if (trackCount < 2) {
		fprintf(stderr, "[gameframe] ResurrectPlayer: track data is unavailable\n");
		gCurPlayer->deathDelay = 0.5f;
		return;
	}
	for(i=0;(i<gTrackUp->num)&&(gTrackUp->track[i].y<gPlayerObj->pos.y);i++);
	if(gRouteActive)	/* free-form road: the track point ahead of the wreck */
		i=Route_TrackTarget(gTrackUp,gPlayerObj->pos);
	if(i < 1)
		i = 1;
	else if(i >= trackCount)
		i = trackCount - 1;
	newPlayer=NewObject(gFirstObj,gRoadInfo->water?kNormalPlayerBoatID:gPlayerCarID);
	if(!newPlayer){
		fprintf(stderr, "[gameframe] ResurrectPlayer: failed to create player object\n");
		gCurPlayer->deathDelay = 0.5f;
		return;
	}
	gPlayerObj=newPlayer;
	gPlayerObj->pos.x=gTrackUp->track[i-1].x;
	gPlayerObj->pos.y=gTrackUp->track[i-1].y;
	gPlayerObj->control=kObjectDriveUp;
	if(gPlayerObj->pos.y>=500)
	{
		gPlayerObj->target=i;
		{
			t2DPoint targetPos=P2D(gTrackUp->track[i].x,gTrackUp->track[i].y);
			if(gPlayerObj->pos.y-targetPos.y)
				gPlayerObj->dir=atan((gPlayerObj->pos.x-targetPos.x)/(gPlayerObj->pos.y-targetPos.y));
			else
				gPlayerObj->dir=0;
		}
	}
	else{
		gPlayerObj->pos.x=gLevelData->xStartPos;
		gPlayerObj->pos.y=500;
		gPlayerObj->target=trackCount > 1 ? 1 : 0;
	}
	if(gRouteActive)
	{
		/* free-form road: back on the road where the wreck was, on the own
		   lane, facing the way the road goes (south or west as well) */
		float s=Route_Nearest(oldPlayer->pos,oldPlayer->routeS>0?oldPlayer->routeS:-1),dir;
		if(s<500)
			s=500;
		if(s>Route_FinishS()-100)
			s=Route_FinishS()-100;
		Route_Point(s,Route_HalfWidth()*0.45f,&gPlayerObj->pos,&dir);
		gPlayerObj->dir=dir;
		gPlayerObj->routeS=s;
		gPlayerObj->onBridge=0;
		gPlayerObj->target=Route_TrackTarget(gTrackUp,gPlayerObj->pos);
	}
	gPlayerObj->velo=P2D(0,0);
	gPlayerObj->rotVelo=0;
	gPlayerObj->damage=0;
	gPlayerObj->damageFlags=0;
	Network_ReplaceDriverObject(oldPlayer,gPlayerObj);
	Network_PlaceDriverApart(gPlayerObj);
	gCurPlayer->obj=gPlayerObj;
	gCameraObj=gPlayerObj;
	gScreenBlitSpecial=true;
	gCurPlayer->zoomVelo=kMaxZoomVelo;
	gGameTime+=kPenaltyTime;
	/* Alone, cops near the wreck are moved away. With others watching
	   they drive on (else they vanish before everyone's eyes); the driver
	   is safe for a moment instead (the blinking shield ring). */
	if(Network_IsActive()&&Network_GetPlayerCount()>1)
	{
		if(gCurPlayer->shieldGrace<kRespawnGrace)
			gCurPlayer->shieldGrace=kRespawnGrace;
	}
	else
		CopClear();
}



static void TickTimer(float *timer)
{
	if(*timer>0)
		*timer-=kFrameDuration;
}

/* Timed crates of the bound driver: magnet, oil slick, nitro and the rest
   count down here. */
static void CrateEffects()
{
	int active=!gCurPlayer->deathDelay&&gCurPlayer->lives>0&&!gCurPlayer->finishDelay;
	TickTimer(&gCurPlayer->magnetTime);
	TickTimer(&gCurPlayer->ramTime);
	TickTimer(&gCurPlayer->oilTime);
	TickTimer(&gCurPlayer->nitroTime);
	TickTimer(&gCurPlayer->rainTime);
	TickTimer(&gCurPlayer->zombieTime);
	TickTimer(&gCurPlayer->warTime);
	TickTimer(&gCurPlayer->dodgeTime);
	TickTimer(&gCurPlayer->shieldGrace);
	if(!active)
		return;
	if(gCurPlayer->magnetTime>0)
	{
		/* Packages close by fly to the car and are collected. On a LAN
		   client they are the host's, so they are collected from further. */
		tObject *obj=(tObject*)gFirstObj->next;
		while(obj!=gFirstObj)
		{
			tObject *next=(tObject*)obj->next;
			if(obj->type->flags&kObjectBonusFlag)
			{
				t2DPoint diff=VEC2D_Difference(gPlayerObj->pos,obj->pos);
				float dist=VEC2D_Value(diff);
				if(dist<(obj->replica?160:60))
					BonusObject(obj);
				else if(dist<240&&!obj->replica)
					obj->pos=VEC2D_Sum(obj->pos,VEC2D_Scale(diff,0.15));
			}
			obj=next;
		}
	}
	if(gCurPlayer->oilTime>0&&!(gFrameCount%20)&&!gPlayerObj->jumpHeight)
	{
		t2DPoint back=P2D(-sin(gPlayerObj->dir),-cos(gPlayerObj->dir));
		tObject *oil=NewObject(gPlayerObj,161);
		if(oil)
		{
			oil->pos=VEC2D_Sum(gPlayerObj->pos,
				VEC2D_Scale(back,(*gPlayerObj->type).length*kScale+14));
			oil->dir=RanFl(0,2*PI);
			oil->shooter=gPlayerObj;	/* its own oil does not catch it */
			Network_RecordShot(gPlayerObj,oil,161);
		}
	}
	if(gCurPlayer->nitroTime>0&&!(gFrameCount%3))
		NitroFlame(gPlayerObj);
}

void PlayerHandling()
{
	//weapons handling and engine sound.
	if(Network_DriverIsFinished(gPlayerObj))
	{
		/* The host may flag a finish before this driver's own detection. */
		if(!gCurPlayer->finishDelay)
		{
			gCurPlayer->finishDelay=0.001;
			gCurPlayer->finishTime=gGameTime;
			gCurPlayer->finishPlace=Network_GetFinishedDriverCount();
		}
		gCurPlayer->finishDelay+=kFrameDuration;
		/* Co-op: fire hands one of this driver's lives to a driver who is out. */
		Network_UpdateDriverButtons(gPlayerObj);
		if(gFire&&!gCurPlayer->fireHeld)
			Network_GiveLife(gPlayerObj);
		gCurPlayer->fireHeld=gFire;
		SetCarSound(-1,0,0,0);
		FFBDirect(0,0);
		return;
	}
	if(Network_DriverIsEliminated(gPlayerObj))
	{
		SetCarSound(-1,0,0,0);
		FFBDirect(0,0);
		return;
	}
	if(gCurPlayer->deathDelay)
	{
		gCurPlayer->deathDelay-=kFrameDuration;
		if(gCurPlayer->deathDelay<=0)
		{
			gCurPlayer->deathDelay=0;
			if(gCurPlayer->lives)
				ResurrectPlayer();
			else if (Network_IsActive())
			{
				const char *name=Network_GetPlayerName(gPlayerObj);
				Network_MarkDriverEliminated(gPlayerObj);
				if(name)
					CheckHighScoreForPlayer((UInt32)gCurPlayer->score,name);
			}
			else
				gEndGame=true;
		}
		SetCarSound(-1,0,0,0);
		FFBDirect(0,0);
	}
	else
	{
		Network_UpdateDriverButtons(gPlayerObj);
		if(!gCameraObj->jumpHeight)
		{
			float lMag=(gCurPlayer->slide[0]+gCurPlayer->slide[2])*0.1;
			float rMag=(gCurPlayer->slide[1]+gCurPlayer->slide[3])*0.1;
			SetCarSound(fabs(gCameraObj->throttle)+(gRoadInfo->water?gCameraObj->input.brake:0),(gCurPlayer->slide[0]+gCurPlayer->slide[2])*0.5,(gCurPlayer->slide[1]+gCurPlayer->slide[3])*0.5,VEC2D_DotProduct(gCameraObj->velo,P2D(sin(gCameraObj->dir),cos(gCameraObj->dir))));
			if(gRoadInfo->skidSound!=145)
				FFBDirect(lMag>0.3?0.3:lMag,rMag>0.3?0.3:rMag);
			else
				FFBDirect(0,0);
		}
		else
		{
			SetCarSound(fabs(gCameraObj->throttle)+(gRoadInfo->water?gCameraObj->input.brake:0),0,0,fabs(gCameraObj->input.throttle)*71);
			FFBDirect(0,0);
		}
		if(!gPlayerObj->jumpHeight&&!Network_RaceHeld())
		{
			if(gFire&&gCurPlayer->mines)
			{
				FireWeapon(gPlayerObj,(*gRoadInfo).water?254:206);
				gCurPlayer->mines--;
				FFBJolt(1.0,1.0,0.1);
			}
			if(gMissile&&gCurPlayer->missiles)
			{
				FireWeapon(gPlayerObj,(*gRoadInfo).water?221:162);
				gCurPlayer->missiles--;
				FFBJolt(0.8,0.8,0.8);
			}
		}
	}
	//time handling.
	if (Network_IsActive()) {
		/* The race clock runs in UpdateRaceClock(); finished drivers never
		   get here, so the overtime penalty only hits drivers still racing. */
		if(!(gFrameCount%(int)(kCalcFPS/10))&&gGameTime>gLevelData->time&&gCurPlayer->score)
			gCurPlayer->score-=1;
	} else {
		if(gCurPlayer->finishDelay)
			gCurPlayer->finishDelay+=kFrameDuration;
		else
			gGameTime+=kFrameDuration;
		if(!(gFrameCount%(int)(kCalcFPS/10))) {
			if(gGameTime>gLevelData->time&&gCurPlayer->score)
				gCurPlayer->score-=1;
			if(!(gFrameCount%(int)kCalcFPS)&&!gCurPlayer->finishDelay&&
				gGameTime>=gLevelData->time-10) {
				if(gGameTime<gLevelData->time)
					SimplePlaySound(144);
				if((int)gGameTime==(int)gLevelData->time) {
					tTextEffect fx={320,240,kEffectSinLines+kEffectMoveDown,0,"\x09TIMEhUPee"};
					NewTextEffect(&fx);
					SimplePlaySound(149);
				}
			}
		}
	}
	//score handling
	if(gCurPlayer->displayScore<gCurPlayer->score)
	{
		gCurPlayer->displayScore+=kDisplayScoreIncr*kFrameDuration;
		if(gCurPlayer->displayScore>gCurPlayer->score)
			gCurPlayer->displayScore=gCurPlayer->score;
	}
	if(gCurPlayer->displayScore>gCurPlayer->score)
		gCurPlayer->displayScore=gCurPlayer->score;
	if(gCurPlayer->score>(gCurPlayer->extraLives+1)*kExtraLiveScore)
	{
		tTextEffect fx={320,240,kEffectSinLines+kEffectMoveUp,0,"\x0c" "EXTRAhLIFEee"};
		NewTextEffect(&fx);
		gCurPlayer->extraLives++;
		gCurPlayer->lives++;
		SimplePlaySound(154);
	}
	//finish detection
	if(gCameraObj->pos.y<320)gCameraObj->pos.y=320;
	if(gRouteActive)	/* free-form road: the finish is a distance along it */
		Route_TrackObject(gPlayerObj);
	if(gRouteActive?gPlayerObj->routeS>Route_FinishS():gPlayerObj->pos.y>gLevelData->levelEnd)
		if(!gCurPlayer->deathDelay&&!gCurPlayer->finishDelay&&
			!Network_DriverIsFinished(gPlayerObj))
		{
			/* The time bonus is booked by the level-complete sequence. */
			tTextEffect fx={320,240,kEffectExplode,0,"\x0f" "LEVELhCOMPLETED"};
			NewTextEffect(&fx);
			gCurPlayer->finishDelay=0.001;
			gCurPlayer->finishTime=gGameTime;
			gCurPlayer->scoreBeforeBonus=gCurPlayer->score;
			gCurPlayer->finishPlace=Network_GetFinishedDriverCount()+1;
			gCurPlayer->fireHeld=gFire;
			Network_MarkDriverFinished(gPlayerObj);
		}
	//background drift
	if (!Network_IsActive() || Network_IsPrimaryDriver(gPlayerObj)) {
		gXDriftPos+=(*gRoadInfo).xDrift*kFrameDuration;
		gYDriftPos+=(*gRoadInfo).yDrift*kFrameDuration;
		gXFrontDriftPos+=(*gRoadInfo).xFrontDrift*kFrameDuration;
		gYFrontDriftPos+=(*gRoadInfo).yFrontDrift*kFrameDuration;
	}
	//camera distance
	Player_UpdateZoom(gCurPlayer,gPlayerObj);
	//super grip and offroad tyres crates
	if(gCurPlayer->gripTime>0)
		gCurPlayer->gripTime-=kFrameDuration;
	if(gCurPlayer->offroadTime>0)
	{
		float before=gCurPlayer->offroadTime;
		gCurPlayer->offroadTime-=kFrameDuration;
		if(before>3&&gCurPlayer->offroadTime<=3)
		{
			tTextEffect fx={320,60,kEffectSmall+kEffectSinLines,0,"\x0c" "OFFROADhENDS"};
			NewTextEffect(&fx);
		}
	}
	CrateEffects();
	/* Off the road (offroad tyres crate) or on the rough edge: grass and
	   gravel slow the car down, it cannot race there. */
	if(!(*gRoadInfo).water&&!gPlayerObj->jumpHeight&&!gCurPlayer->deathDelay)
	{
		int off=CalcBackCollision(gPlayerObj->pos);
		float cap=off==2?13:off==1?22:0,velo=VEC2D_Value(gPlayerObj->velo);
		if(cap&&velo>cap)
			gPlayerObj->velo=VEC2D_Scale(gPlayerObj->velo,(velo-(velo-cap)*0.06f)/velo);
	}
	//spike handling
	gCurPlayer->spikeFrame+=VEC2D_Value(gPlayerObj->velo)/20;
	if(gCurPlayer->addOns&kAddOnSpikes&&!(gCurPlayer->deathDelay!=0)&&!(*gRoadInfo).water)
	{
		if(!gCurPlayer->spikeObj)
			gCurPlayer->spikeObj=NewObject(gPlayerObj,207);
		if(gCurPlayer->spikeObj){
			gCurPlayer->spikeObj->pos=gPlayerObj->pos;
			gCurPlayer->spikeObj->onBridge=gPlayerObj->onBridge;	/* the car's level */
			gCurPlayer->spikeObj->dir=gPlayerObj->dir;
			gCurPlayer->spikeObj->jumpHeight=gPlayerObj->jumpHeight;
			gCurPlayer->spikeObj->frame=gCurPlayer->spikeObj->type->frame+(int)gCurPlayer->spikeFrame%gCurPlayer->spikeObj->type->numFrames;
		}
	}else if(gCurPlayer->spikeObj)
	{
		KillObject(gCurPlayer->spikeObj);
		gCurPlayer->spikeObj=nil;
	}
	//brake light handling
	if(gCurPlayer->brakeObj)
	{
		KillObject(gCurPlayer->brakeObj);
		gCurPlayer->brakeObj=nil;
	}
	/* The brake light picture is a car's; a motorbike's is drawn on its own. */
	if((gPlayerObj->input.brake||gPlayerObj->input.reverse)&&!(*gRoadInfo).water&&!(gCurPlayer->deathDelay!=0)&&gCurPlayer->lives&&
		gPlayerObj->type->width>=1.0f)
	{
		gCurPlayer->brakeObj=NewObject(gPlayerObj,228);
		if(gCurPlayer->brakeObj){
			if(gPlayerObj->input.reverse)gCurPlayer->brakeObj->frame=394;
			if(!gPlayerObj->input.brake)gCurPlayer->brakeObj->frame=395;
			gCurPlayer->brakeObj->pos=gPlayerObj->pos;
			gCurPlayer->brakeObj->onBridge=gPlayerObj->onBridge;	/* not seen through a bridge */
			gCurPlayer->brakeObj->dir=gPlayerObj->dir;
			gCurPlayer->brakeObj->jumpHeight=gPlayerObj->jumpHeight;
		}
	}
	//invincibility
	gPlayerObj->damage=0;
}

/* Multiplayer race clock: keeps running until the last driver is done, so
   everyone's time bonus is measured against the same clock. */
static void UpdateRaceClock()
{
	static int countdownShown;
	float previous=gGameTime;
	if(!Network_AdvanceRaceClock()) {
		if(!Network_UnfinishedDriverCount())
			return;
		gGameTime+=kFrameDuration;
	}
	/* LAN start: 3, 2, 1, GO once every computer is ready. */
	if(previous<0) {
		if(gGameTime>previous) {
			int number=gGameTime>=0?0:(int)ceilf(-gGameTime-0.001f);
			if(number<countdownShown) {
				tTextEffect fx={320,240,kEffectExplode,0,"\x01" "3"};
				countdownShown=number;
				if(number)
					fx.text[1]='0'+number+0x2b;	/* digits in the effect font, see MakeFXStringFromNumStr */
				else
					memcpy(fx.text,"\x04" "GOee",5);
				NewTextEffect(&fx);
				SimplePlaySound(144);
			}
		} else
			countdownShown=4;
	}
	if(gGameTime<0)
		return;
	if(!(gFrameCount%(int)kCalcFPS)&&gGameTime>=gLevelData->time-10) {
		if(gGameTime<gLevelData->time)
			SimplePlaySound(144);
		if((int)gGameTime==(int)gLevelData->time) {
			tTextEffect fx={320,240,kEffectSinLines+kEffectMoveDown,0,"\x09TIMEhUPee"};
			NewTextEffect(&fx);
			SimplePlaySound(149);
		}
	}
}

void GameFrame()
{
	MoveObjects();
	Network_SyncObjects();
	Network_ProcessEvents();
	Network_UpdatePoliceMode();
	if (Network_IsActive()) {
		int driver, driverCount = Network_GetLocalDriverCount();
		for (driver = 0; driver < driverCount; ++driver) {
			tObject *player = Network_GetLocalDriverObject(driver);
			if (!player)
				continue;
			Network_BindDriver(player);
			PlayerHandling();
			Network_UnbindDriver();
		}
		UpdateRaceClock();
		Network_UpdateSpectators();
	} else {
		PlayerHandling();
	}
	if (Network_UpdateRaceProgress()) {
		if (Network_AllDriversEliminated() || gLevelID + 1 >= 10) {
			gEndGame = true;
		} else {
			Network_PrepareNextLevel();
			DisposeLevel();
			gLevelID++;
			if (!LoadLevel())
				gEndGame = true;
		}
	}
	gFrameCount++;
	if(CheckFrameTime())
	{
		UInt64 renderStart=Platform_GetMicroseconds();
		tObject *viewDrivers[kNetworkMaxPlayers];
		int viewCount = 0, view;
		if (Network_IsActive() && !Network_IsLocalMultiplayer() &&
			(Prefs_IsLanOwnScreen() || Network_GetPlayerCount() > 4)) {
			/* LAN, own screen: only this computer's driver, full size.
			   Split screen has room for four views at most. */
			viewDrivers[0] = Network_GetLocalDriverObject(0);
			viewCount = viewDrivers[0] ? 1 : 0;
		} else if (Network_IsActive()) {
			viewCount = Network_GetPlayerCount();
			for (view = 0; view < viewCount; ++view)
				viewDrivers[view] = Network_GetCameraForView(view);
		}
		Platform_BeginMultiView(viewCount > 1 ? viewCount : 1);
		if (viewCount > 0) {
			tObject *savedCamera = gCameraObj;
			for (view = 0; view < viewCount; ++view) {
				tObject *driver = viewDrivers[view];
				if (!driver)
					driver = savedCamera;
				Network_BindDriver(driver);
				/* A driver who is out watches the leader from their own view. */
				gCameraObj = Network_GetSpectateTarget(driver);
				if (!gCameraObj)
					gCameraObj = driver;
				GetVisObjs();
				gScreenBlitSpecial = false;
				RenderFrame();
				Network_UnbindDriver();
				gCameraObj = savedCamera;
			}
			gCameraObj = savedCamera;
			GetVisObjs();
		} else {
			RenderFrame();
		}
		gGraphFrameCount++;
		{
			/* Exclude vsync wait so the number reflects software rendering cost. */
			UInt64 elapsed=Platform_GetMicroseconds()-renderStart;
			UInt64 present=Platform_TakePresentMicroseconds();
			FrameStatsRecord(1,elapsed>present?elapsed-present:0);
		}
		CheckTimeSkip();
	}
	else
		FrameStatsRecord(0,0);
	Network_Publish();
	if(gEndGame)
		EndGame();
}

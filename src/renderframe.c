#include "compat.h"
#include <math.h>
#include <stdio.h>
#include "objects.h"
#include "screen.h"
#include "sprites.h"
#include "vec2d.h"
#include "trig.h"
#include "textfx.h"
#include "renderframe.h"
#include "gameinitexit.h"
#include "preferences.h"
#include "particlefx.h"
#include "packs.h"
#include "rle.h"
#include "gamesounds.h"
#include "interface.h"
#include "netgame.h"
#include "textrender.h"
#include "hqassets.h"
#include "high.h"

void DrawRoadZoomed(float,float,float);
void DrawRoadZoomed16(float,float,float);

#define kXCameraScreenPos 0.5
#define kYCameraScreenPos 0.55
//horizontal and vertical position of the object the camera is looking at on screen, => 0,5/0,5 is center.

#define kPanelHeight	121		//heigth of the cockpit graphic on screen in pixels.

#define kMaxDisplayVelo	89.408
#define kZoomVeloFactor 50

#define kTrackSize		3.4		//width of the Tracks left by cars in pixels
#define kTrackLifeTime		(14.0*kCalcFPS)	//time after which rubber tracks left by cars get removed in frames.
#define kTrackDeathDuration	(0.8*kCalcFPS)	//time it takes for rubber tracks to fade out in frames.

#define kDigitOffs		18
#define kTimeDecimalOffs	(kDigitOffs+3)
#define kTimeMinuteOffs		(-2*kDigitOffs-5)
#define kScoreX			610
#define kScoreY			51
#define kLiveX			613
#define kLiveY			76
#define kTimeX			586
#define kTimeY			26
#define kAddOnX			292
#define kAddOnY			25

#define kLifeBarWidth	158
#define kLifeBarY		27
#define kLifeBarX		227
#define kLifeBarFrameId	156

#define kComplYSize		370
#define kTimeTakeX		302
#define kTimeTakeY		204
#define kTimeBestX		302
#define kTimeBestY		233
#define kTimeLeftX		438
#define kTimeLeftY		282
#define kTimeBonusX		459
#define kTimeBonusY		311
#define kTimeBonus2X	459
#define kTimeBonus2Y	342

#define kScrollComplDelay	1.5
#define kShowComplDelay		4
#define kShowBestTimeDelay	6
#define kShowBonusTimeDelay	8
#define kStartBonusDelay	10
#define kCountBonusSpeed	0.1
#define kCountNoiseSpeed	0.1
#define kMultiplyDelay		1
#define kCloseDelay			3.5

#define kMaxMarkLength	128


static const int kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000};

void DrawNum(int h,int v,int num,int digits)
{
	int i;
	for(i=0;i<digits;i++)
	{
		if(num/kPow10[i]||!i)
			DrawRLE(h,v,128+(num/kPow10[i])%10);
		h-=kDigitOffs;
	}
}

void DrawNumZeroed(int h,int v,int num,int digits)
{
	int i;
	for(i=0;i<digits;i++)
	{
		DrawRLE(h,v,128+(num/kPow10[i])%10);
		h-=kDigitOffs;
	}
}

#define kResultHoldDelay	3

static const char *PlaceSuffix(int place)
{
	return place==1?"ST":place==2?"ND":place==3?"RD":"TH";
}

/* Live race position (and duel kills) in the top-left corner. */
static void DrawRacePosition(void)
{
	tPixel *framebuffer=(tPixel*)gBaseAddr;
	int place=Network_GetDriverPlace(gPlayerObj);
	char text[24];
	if(place<1)
		return;
	TR_FillRect(framebuffer,gRowBytes/4,6,6,Network_IsDuel()?112:96,
		Network_IsDuel()?48:34,COL_DKGRAY);
	snprintf(text,sizeof(text),"%d%s",place,PlaceSuffix(place));
	TR_DrawString(framebuffer,gRowBytes/4,12,11,text,
		place==1?COL_YELLOW:COL_WHITE,3);
	snprintf(text,sizeof(text),"/%d",Network_GetPlayerCount());
	TR_DrawString(framebuffer,gRowBytes/4,12+TR_StringWidth("0ST",3)+4,25,
		text,COL_GRAY,1);
	if(Network_IsDuel())
	{
		snprintf(text,sizeof(text),"KILLS %d",gCurPlayer->kills);
		TR_DrawString(framebuffer,gRowBytes/4,12,40,text,COL_RED,1);
	}
}

static float ComplTimeLeft(const tPlayer *player)
{
	float timeDiff=gLevelData->time-player->finishTime;
	return timeDiff>0?timeDiff:0;
}

/* How long a driver's level-complete sequence runs, including the result
   card in multiplayer; the next level waits until every finisher is done. */
float GetComplSequenceLength(const tPlayer *player)
{
	return kStartBonusDelay+ComplTimeLeft(player)*kCountBonusSpeed+kCloseDelay+
		(Network_IsActive()?kResultHoldDelay:0);
}

/* Name, total score and leaderboard place, shown once a driver's bonus is in. */
static void DrawComplResult(int isLocal)
{
	const char *playerName=Network_GetPlayerName(gPlayerObj);
	char text[32];
	tPixel *framebuffer=(tPixel*)gBaseAddr;
	int rank=gCurPlayer->highScoreRank;
	if(!playerName)
		playerName="Driver";
	if(!isLocal)
		rank=GetHighScoreRank(playerName);
	/* Below the completion panel, where the dashboard normally sits. */
	TR_FillRect(framebuffer,gRowBytes/4,SCREEN_WIDTH/2-150,SCREEN_HEIGHT-88,
		300,84,COL_DKGRAY);
	TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
		SCREEN_HEIGHT-82,playerName,COL_WHITE,2);
	snprintf(text,sizeof(text),"SCORE: %u",(unsigned)gCurPlayer->score);
	TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
		SCREEN_HEIGHT-62,text,COL_CYAN,2);
	if(rank>0&&(gFrameCount/20)%3)
	{
		snprintf(text,sizeof(text),"HIGH SCORE #%d",rank);
		TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
			SCREEN_HEIGHT-42,text,COL_YELLOW,2);
	}
	if(gCurPlayer->finishPlace>0)
	{
		int placeBonus=Network_GetPlacementBonus(gCurPlayer->finishPlace);
		if(placeBonus)
			snprintf(text,sizeof(text),"%d%s PLACE  +%d",gCurPlayer->finishPlace,
				PlaceSuffix(gCurPlayer->finishPlace),placeBonus);
		else
			snprintf(text,sizeof(text),"%d%s PLACE",gCurPlayer->finishPlace,
				PlaceSuffix(gCurPlayer->finishPlace));
		TR_FillRect(framebuffer,gRowBytes/4,SCREEN_WIDTH/2-150,SCREEN_HEIGHT-112,
			300,22,COL_DKGRAY);
		TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
			SCREEN_HEIGHT-106,text,gCurPlayer->finishPlace==1?COL_YELLOW:COL_WHITE,1);
	}
	if(Network_UnfinishedDriverCount()>0)
		TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
			SCREEN_HEIGHT-18,"WAITING FOR OTHER DRIVERS",COL_GRAY,1);
}

int DrawComplCount()
{
	float timeDiff=ComplTimeLeft(gCurPlayer);
	float countTime=timeDiff*kCountBonusSpeed;
	float finishDelay=gCurPlayer->finishDelay;
	float finishTime=gCurPlayer->finishTime;
	int bonus=timeDiff*10;
	/* Remote drivers count their own bonus; their score arrives over the
	   network, so only drivers simulated here change score or play sounds. */
	int isLocal=Network_IsLocalDriver(gPlayerObj);
	int done=finishDelay>=kStartBonusDelay+countTime+kCloseDelay;
	if(Network_IsActive()&&done&&isLocal&&!gCurPlayer->resultRecorded)
	{
		const char *playerName=Network_GetPlayerName(gPlayerObj);
		gCurPlayer->score=gCurPlayer->scoreBeforeBonus+bonus*gCurPlayer->bonus+
			Network_GetPlacementBonus(gCurPlayer->finishPlace);
		gCurPlayer->resultRecorded=true;
		if(playerName)
			gCurPlayer->highScoreRank=CheckHighScoreForPlayer(
				(UInt32)gCurPlayer->score,playerName);
	}
	/* Once booked, the final numbers stay on screen but the score is left alone. */
	if(gCurPlayer->resultRecorded)
		isLocal=false;
	if(finishDelay>=kShowComplDelay)
		DrawRLE(0,SCREEN_HEIGHT/2-kComplYSize/2,149);
	else if(finishDelay>=kScrollComplDelay)
	{
		int yPos=(SCREEN_HEIGHT/2+kComplYSize/2)*sqrt((finishDelay-kScrollComplDelay)/(kShowComplDelay-kScrollComplDelay))-kComplYSize;
		DrawRLEYClip(0,yPos,149);
		if(isLocal)
		{
			gCurPlayer->scoreBeforeBonus=gCurPlayer->score;
			gCurPlayer->bonusCount=0;
		}
	}
	if(finishDelay>=kShowBestTimeDelay&&!gLevelResFile)
	{
		if(isLocal&&gPrefs.lapRecords[gLevelID]>finishTime)
		{
			gPrefs.lapRecords[gLevelID]=finishTime;
			SimplePlaySound(155);
		}
		DrawNum(kTimeBestX+kTimeMinuteOffs,kTimeBestY,floor(gPrefs.lapRecords[gLevelID])/60,2);
		DrawNumZeroed(kTimeBestX,kTimeBestY,(int)floor(gPrefs.lapRecords[gLevelID])%60,2);
		DrawNum(kTimeBestX+kTimeDecimalOffs,kTimeBestY,(int)floor(gPrefs.lapRecords[gLevelID]*10),1);
		DrawNum(kTimeTakeX+kTimeMinuteOffs,kTimeTakeY,floor(finishTime)/60,2);
		DrawNumZeroed(kTimeTakeX,kTimeTakeY,(int)floor(finishTime)%60,2);
		DrawNum(kTimeTakeX+kTimeDecimalOffs,kTimeTakeY,(int)floor(finishTime*10)%10,1);
	}
	if(finishDelay>=kStartBonusDelay)
	{
		float bonusPerc=(finishDelay-kStartBonusDelay)/countTime;
		if(bonusPerc>1)bonusPerc=1;
		if(isLocal&&bonus*bonusPerc*kCountNoiseSpeed>gCurPlayer->bonusCount)
		{
			gCurPlayer->bonusCount++;
			SimplePlaySound(148);
		}
		DrawNum(kTimeLeftX+kTimeMinuteOffs,kTimeLeftY,floor(timeDiff-timeDiff*bonusPerc)/60,2);
		DrawNumZeroed(kTimeLeftX,kTimeLeftY,(int)floor(timeDiff-timeDiff*bonusPerc)%60,2);
		DrawNum(kTimeLeftX+kTimeDecimalOffs,kTimeLeftY,(int)floor((timeDiff-timeDiff*bonusPerc)*10),1);
		DrawNum(kTimeBonusX,kTimeBonusY,bonus*bonusPerc,5);
		if(isLocal)
			gCurPlayer->score=gCurPlayer->scoreBeforeBonus+bonus*bonusPerc;
	}
	else if(finishDelay>=kShowBonusTimeDelay)
	{
		DrawNum(kTimeLeftX+kTimeMinuteOffs,kTimeLeftY,floor(timeDiff)/60,2);
		DrawNumZeroed(kTimeLeftX,kTimeLeftY,(int)floor(timeDiff)%60,2);
		DrawNum(kTimeLeftX+kTimeDecimalOffs,kTimeLeftY,(int)floor(timeDiff*10),1);
		DrawNum(kTimeBonusX,kTimeBonusY,0,5);
	}
	if(finishDelay>=kStartBonusDelay+countTime+kMultiplyDelay)
		if(gCurPlayer->bonus!=1)
		{
			if(isLocal&&gCurPlayer->bonusCount!=32767)
			{
				SimplePlaySound(141);
				gCurPlayer->bonusCount=32767;
			}
			DrawRLE(kTimeBonus2X-5*kDigitOffs,kTimeBonus2Y,137+gCurPlayer->bonus);
			DrawNum(kTimeBonus2X,kTimeBonus2Y,bonus*gCurPlayer->bonus,5);
		}
	if(Network_IsActive())
	{
		const char *giftName=Network_IsLocalDriver(gPlayerObj)?
			Network_GetGiftTargetName(gPlayerObj):NULL;
		if(done)
			DrawComplResult(Network_IsLocalDriver(gPlayerObj));
		if(giftName)
		{
			tPixel *framebuffer=(tPixel*)gBaseAddr;
			char text[48];
			snprintf(text,sizeof(text),"FIRE: GIVE 1 LIFE TO %s",giftName);
			TR_FillRect(framebuffer,gRowBytes/4,SCREEN_WIDTH/2-150,24,300,22,COL_DKGRAY);
			TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,30,
				text,(gFrameCount/15)%2?COL_GREEN:COL_WHITE,1);
		}
		if(done)
			return true;
	}
	if(done)
	{
		gCurPlayer->score=gCurPlayer->scoreBeforeBonus+bonus*gCurPlayer->bonus;
		DisposeLevel();
		gLevelID++;
		return LoadLevel();
	}
	return true;
}

/* Shown in the view of a driver who lost the last life before the finish. */
static void DrawEliminated(void)
{
	tPixel *framebuffer=(tPixel*)gBaseAddr;
	char text[32];
	const char *watched=gCameraObj!=gPlayerObj?Network_GetPlayerName(gCameraObj):NULL;
	/* Kept at the top so the leader's race stays visible below. */
	TR_FillRect(framebuffer,gRowBytes/4,SCREEN_WIDTH/2-190,8,380,
		watched?80:66,COL_DKGRAY);
	TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
		14,"OUT OF LIVES",COL_RED,2);
	TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
		36,Network_IsDuel()?"BACK NEXT LEVEL WITH 1 LIFE":
		"BACK NEXT LEVEL OR WITH A TEAMMATE'S LIFE",COL_WHITE,1);
	snprintf(text,sizeof(text),"SCORE: %u",(unsigned)gCurPlayer->score);
	TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
		50,text,COL_CYAN,1);
	if(watched)
	{
		snprintf(text,sizeof(text),"WATCHING %s",watched);
		TR_DrawStringCentered(framebuffer,gRowBytes/4,SCREEN_WIDTH/2,
			66,text,COL_YELLOW,1);
	}
}

/* Helper: convert int to Pascal string for display */
static void NumToPStr(int num, Str31 pstr)
{
	char tmp[16];
	int len = sprintf(tmp, "%d", num);
	pstr[0] = len;
	memcpy(pstr+1, tmp, len);
}

void DrawDisplays()
{
	int i;
	float timeDisp=gLevelData->time-gGameTime>0?gLevelData->time-gGameTime:0;
	if(timeDisp>gLevelData->time)
		timeDisp=gLevelData->time;	/* LAN countdown */
	float veloDisplay;
	static int dispDbg = 0;
	if(!gCameraObj->jumpHeight)
		veloDisplay=-PI/2+VEC2D_DotProduct(gCameraObj->velo,P2D(sin(gCameraObj->dir),cos(gCameraObj->dir)))*PI/kMaxDisplayVelo;
	else
		veloDisplay=-PI/2+gCameraObj->input.throttle*PI;
	DrawRLE(0,SCREEN_HEIGHT-kPanelHeight,148);
	DrawSprite(189,148*Platform_GetRenderScale(),
		(SCREEN_HEIGHT-14)*Platform_GetRenderScale(),veloDisplay,1);
	DrawNum(kScoreX,SCREEN_HEIGHT-kScoreY,gCurPlayer->displayScore,6);
	DrawNum(kLiveX,SCREEN_HEIGHT-kLiveY,gCurPlayer->lives,2);
	DrawNum(kTimeX+kTimeMinuteOffs,SCREEN_HEIGHT-kTimeY,floor(timeDisp)/60,2);
	DrawNumZeroed(kTimeX,SCREEN_HEIGHT-kTimeY,(int)floor(timeDisp)%60,2);
	DrawNum(kTimeX+kTimeDecimalOffs,SCREEN_HEIGHT-kTimeY,(int)floor(timeDisp*10),1);
	for(i=0;i<6;i++)
		if(gCurPlayer->addOns&1<<i)
			DrawRLE(kAddOnX+20*i,SCREEN_HEIGHT-kAddOnY,142+i);
	if(Network_IsActive() && Network_GetPlayerCount() > 1) {
		const char *playerName = Network_GetPlayerName(gPlayerObj);
		if(playerName && playerName[0]) {
			int rowBytes;
			tPixel *fb = Platform_GetFramebuffer(&rowBytes);
			int stride = rowBytes/4;
			int width = TR_StringWidth(playerName, 1);
			TR_FillRect(fb, stride, SCREEN_WIDTH - width - 20, 8,
				width + 12, 20, COL_DKGRAY);
			TR_DrawStringRight(fb, stride, SCREEN_WIDTH - 10, 14,
				playerName, COL_WHITE, 1);
		}
		DrawRacePosition();
	}
	if(gCurPlayer->bonus!=1)
		DrawRLE(kAddOnX+6*20,SCREEN_HEIGHT-kAddOnY,137+gCurPlayer->bonus);
	if(gCurPlayer->missiles)
	{
		Str31 numStr;
		DrawRLE(SCREEN_WIDTH-120,5,154);
		NumToPStr(gCurPlayer->missiles,numStr);
		MakeFXStringFromNumStr(numStr,numStr);
		SimpleDrawText(numStr,SCREEN_WIDTH-60,5);
	}
	if(gCurPlayer->mines)
	{
		Str31 numStr;
		DrawRLE(SCREEN_WIDTH-120,gCurPlayer->missiles?30:5,155);
		NumToPStr(gCurPlayer->mines,numStr);
		MakeFXStringFromNumStr(numStr,numStr);
		SimpleDrawText(numStr,SCREEN_WIDTH-60,gCurPlayer->missiles?30:5);
	}
	(void)dispDbg;
}

int DrawPanel()
{
	if(Network_IsActive()&&Network_DriverIsEliminated(gPlayerObj))
		DrawEliminated();
	else if(gCurPlayer->lives)
		if(gCurPlayer->finishDelay)
			return DrawComplCount();
		else if(gCurPlayer->deathDelay)
			DrawRLE(0,SCREEN_HEIGHT-kPanelHeight,150);
		else
			DrawDisplays();
	return true;
}

void DrawTextureBlock(int x,int y,int size,UInt32 zoom,UInt32 u,UInt32 v,UInt8 *texture)
{
	int line,pix;
	UInt8 *dst=gBaseAddr+y*gRowBytes+x;
	for(line=0;line<size;line++){
		UInt8 *data=texture+((v>>9)&0x3f80);
		for(pix=0;pix<size;pix++)
		{
			u+=zoom;
			*dst=data[(u>>16)&0x7f];
			dst++;
		}
		v+=zoom;
		dst+=gRowBytes-size;
	}
}

void DrawTextureBlockClipped(int x,int y,int size,UInt32 zoom,UInt32 u,UInt32 v,UInt8 *texture)
{
	int line,pix;
	int xSize=size,ySize=size;
	UInt8 *dst;
	if(x<0){xSize+=x;x=0;}
	if(y<0){ySize+=y;y=0;}
	if(x+size>gXSize)xSize+=gXSize-x-size;
	if(y+size>gYSize)ySize+=gYSize-y-size;
	dst=gBaseAddr+y*gRowBytes+x;
	for(line=0;line<ySize;line++){
		UInt8 *data=texture+((v>>9)&0x3f80);
		for(pix=0;pix<xSize;pix++)
		{
			u+=zoom;
			*dst=data[(u>>16)&0x7f];
			dst++;
		}
		v+=zoom;
		dst+=gRowBytes-xSize;
	}
}

void DrawTracksZoomed(float xDrawStart,float yDrawStart,float zoom)
{
	float invZoom=1/zoom;
	SInt32 fixZoom=zoom*65536.0;
	int size=kTrackSize*invZoom;
	UInt32 shiftClip=gXSize-size<<16;
	int i;
	int y1Clip=yDrawStart-(gYSize-(gCurPlayer->finishDelay?0:kInvLines))*zoom;
	UInt8 *textures=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).tracks,0);
	for(i=0;i<gTrackCount;i++)
	{
		if(gTracks[i].p2.y<=yDrawStart+size&&gTracks[i].p1.y>y1Clip&&gTracks[i].time+kTrackLifeTime+kTrackDeathDuration>gFrameCount)
		{
			float x2=(gTracks[i].p2.x-xDrawStart)*invZoom-size/2;
			float x1=(gTracks[i].p1.x-xDrawStart)*invZoom-size/2;
			float intensity=gTracks[i].intensity*3*((gTracks[i].time+kTrackLifeTime>gFrameCount)?1:1-(gFrameCount-gTracks[i].time-kTrackLifeTime)/kTrackDeathDuration);
			UInt8 *texture=textures+(int)intensity*128*128;
			if((x1>-size||x2>-size)&&(x1<gXSize||x2<gXSize)){
				float y1=(yDrawStart-gTracks[i].p1.y)*invZoom-size/2;
				float y2=(yDrawStart-gTracks[i].p2.y)*invZoom-size/2;
				UInt32 v1=(int)gTracks[i].p1.y<<16;
				SInt32 u1=(int)gTracks[i].p1.x<<16;
				UInt32 v2=(int)gTracks[i].p2.y<<16;
				SInt32 u2=(int)gTracks[i].p2.x<<16;
				int dy=y2-y1;
				if(dy){
					int y;
					SInt32 dxdy=((x2-x1)/(y2-y1)*65536.0);
					SInt32 dudy=(u2-u1)/(y2-y1);
					SInt32 x=x1*65536.0;
					SInt32 u=u1,v=v1;
					int numBlocks=ceil(fabs(((x2-x1)/(y2-y1)))-size);
					if(numBlocks<0)numBlocks=0;
					numBlocks++;
					for(y=y1;y<y2;y++)
					{
						SInt32 blockU=u;
						SInt32 blockX=x>>16;
						int i;
						for(i=0;i<numBlocks;i++)
						{
							if(blockX>=0&&blockX<gXSize-size&&y>=0&&y<gYSize-size)
								DrawTextureBlock(blockX,y,size,fixZoom,blockU,v,texture);
							else
								DrawTextureBlockClipped(blockX,y,size,fixZoom,blockU,v,texture);
							blockU+=fixZoom;
							blockX++;
						}
						x+=dxdy;
						u+=dudy;
						v+=fixZoom;
					}
				}
				else{
					int x;
					if(x2<0)x2=0;
					if(x1>gXSize-size)x1=gXSize-size;
					if(x1<x2)
					{
						SInt32 u=u1;
						if(x1<0)x1=0;
						if(x2>gXSize-size)x2=gXSize-size;
						for(x=x1;x<x2;x++)
							DrawTextureBlockClipped(x,y1,size,fixZoom,u+=fixZoom,v1,texture);
					}
					else{
						SInt32 u=u2;
						if(x2<0)x2=0;
						if(x1>gXSize-size)x1=gXSize-size;
						for(x=x2;x<x1;x++)
							DrawTextureBlockClipped(x,y1,size,fixZoom,u+=fixZoom,v1,texture);
					}
				}
			}
		}
	}
}

void DrawMarksZoomed(float xDrawStart,float yDrawStart,float zoom)
{
	float invZoom=1/zoom;
	SInt32 fixZoom=zoom*65536.0;
	int size=4.2*invZoom;
	UInt32 shiftClip=gXSize-size<<16;
	int i;
	int y1Clip=yDrawStart-(gYSize-(gCurPlayer->finishDelay?0:kInvLines))*zoom;
	UInt8 *texture=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).marks,0);
	int l=0,r=gMarkSize;
  	while(r-1>l)
		if(gMarks[(l+r)/2].p1.y+gMarks[(l+r)/2].p2.y>yDrawStart*2+kMaxMarkLength)
			l=(l+r)/2;
		else
			r=(l+r)/2;
	for(i=l;i<gMarkSize&&gMarks[i].p1.y+gMarks[i].p2.y>y1Clip*2-kMaxMarkLength;i++)
	{
		if(gMarks[i].p2.y<=yDrawStart+size&&gMarks[i].p1.y>y1Clip)
		{
			float x2=(gMarks[i].p2.x-xDrawStart)*invZoom-size/2;
			float x1=(gMarks[i].p1.x-xDrawStart)*invZoom-size/2;
			if((x1>-size||x2>-size)&&(x1<gXSize||x2<gXSize)){
				float y1=(yDrawStart-gMarks[i].p1.y)*invZoom-size/2;
				float y2=(yDrawStart-gMarks[i].p2.y)*invZoom-size/2;
				UInt32 v1=(int)gMarks[i].p1.y<<16;
				SInt32 u1=(int)gMarks[i].p1.x<<16;
				UInt32 v2=(int)gMarks[i].p2.y<<16;
				SInt32 u2=(int)gMarks[i].p2.x<<16;
				int dy=y2-y1;
				if(dy){
					int y;
					SInt32 dxdy=((x2-x1)/(y2-y1)*65536.0);
					SInt32 dudy=(u2-u1)/(y2-y1);
					SInt32 x=x1*65536.0;
					SInt32 u=u1,v=v1;
					int numBlocks=ceil(fabs(((x2-x1)/(y2-y1)))-size);
					if(numBlocks<0)numBlocks=0;
					numBlocks++;
					for(y=y1;y<y2;y++)
					{
						SInt32 blockU=u;
						SInt32 blockX=x>>16;
						int i;
						for(i=0;i<numBlocks;i++)
						{
							if(blockX>=0&&blockX<gXSize-size&&y>=0&&y<gYSize-size)
								DrawTextureBlock(blockX,y,size,fixZoom,blockU,v,texture);
							else
								DrawTextureBlockClipped(blockX,y,size,fixZoom,blockU,v,texture);
							blockU+=fixZoom;
							blockX++;
						}
						x+=dxdy;
						u+=dudy;
						v+=fixZoom;
					}
				}
				else{
					int x;
					if(x2<0)x2=0;
					if(x1>gXSize-size)x1=gXSize-size;
					if(x1<x2)
					{
						SInt32 u=u1;
						if(x1<0)x1=0;
						if(x2>gXSize-size)x2=gXSize-size;
						for(x=x1;x<x2;x++)
							DrawTextureBlockClipped(x,y1,size,fixZoom,u+=fixZoom,v1,texture);
					}
					else{
						SInt32 u=u2;
						if(x2<0)x2=0;
						if(x1>gXSize-size)x1=gXSize-size;
						for(x=x2;x<x1;x++)
							DrawTextureBlockClipped(x,y1,size,fixZoom,u+=fixZoom,v1,texture);
					}
				}
			}
		}
	}
}

static tPixel SampleTextureHQ(const tHQImage *hq,UInt32 u,UInt32 v)
{
	int x=(u&0x007fffff)>>14;
	int y=(v&0x007fffff)>>14;
	if(hq->width!=128||hq->height!=128||hq->scale!=4)
		return PixelRGB(0,0,0);
	const UInt8 *pixel=hq->rgba+((size_t)y*512+x)*4;
	return PixelRGB(pixel[0],pixel[1],pixel[2]);
}

void DrawTextureBlock16(UInt32 x,UInt32 y,int size,UInt32 zoom,UInt32 u,UInt32 v,UInt16 *texture,const tHQImage *hq)
{
	int line,pix;
	tPixel *dst=(tPixel*)(gBaseAddr+y*gRowBytes)+x;
	for(line=0;line<size;line++){
		UInt16 *data=texture+((v>>9)&0x3f80);
		for(pix=0;pix<size;pix++)
		{
			u+=zoom;
			*dst=hq&&hq->width==128&&hq->height==128&&hq->scale==4?
				SampleTextureHQ(hq,u,v):Pixel555(SWAP16(data[(u>>16)&0x7f]));
			dst++;
		}
		v+=zoom;
		dst+=gRowBytes/4-size;
	}
}

void DrawTextureBlockClipped16(int x,int y,int size,UInt32 zoom,UInt32 u,UInt32 v,UInt16 *texture,const tHQImage *hq)
{
	int line,pix;
	int xSize=size,ySize=size;
	tPixel *dst;
	if(x<0){xSize+=x;x=0;}
	if(y<0){ySize+=y;y=0;}
	if(x+size>gXSize)xSize+=gXSize-x-size;
	if(y+size>gYSize)ySize+=gYSize-y-size;
	dst=(tPixel*)(gBaseAddr+y*gRowBytes)+x;
	for(line=0;line<ySize;line++){
		UInt16 *data=texture+((v>>9)&0x3f80);
		for(pix=0;pix<xSize;pix++)
		{
			u+=zoom;
			*dst=hq&&hq->width==128&&hq->height==128&&hq->scale==4?
				SampleTextureHQ(hq,u,v):Pixel555(SWAP16(data[(u>>16)&0x7f]));
			dst++;
		}
		v+=zoom;
		dst+=gRowBytes/4-xSize;
	}
}

void DrawTracksZoomed16(float xDrawStart,float yDrawStart,float zoom)
{
	float invZoom=1/zoom;
	SInt32 fixZoom=zoom*65536.0;
	int size=kTrackSize*invZoom;
	UInt32 shiftClip=gXSize-size<<16;
	int i;
	int y1Clip=yDrawStart-(gYSize-(gCurPlayer->finishDelay?0:kInvLines))*zoom;
	int textureID=(*gRoadInfo).tracks;
	UInt16 *textures=(UInt16*)GetUnsortedPackEntry(kPackTx16,textureID,0);
	for(i=0;i<gTrackCount;i++)
	{
		if(gTracks[i].p2.y<=yDrawStart+size&&gTracks[i].p1.y>y1Clip&&gTracks[i].time+kTrackLifeTime+kTrackDeathDuration>gFrameCount)
		{
			float x2=(gTracks[i].p2.x-xDrawStart)*invZoom-size/2;
			float x1=(gTracks[i].p1.x-xDrawStart)*invZoom-size/2;
			float intensity=gTracks[i].intensity*3*((gTracks[i].time+kTrackLifeTime>gFrameCount)?1:1-(gFrameCount-gTracks[i].time-kTrackLifeTime)/kTrackDeathDuration);
			int frame=(int)intensity;
			UInt16 *texture=textures+frame*128*128;
			const tHQImage *hq=HQAssets_Load("textures",textureID,frame);
			if((x1>-size||x2>-size)&&(x1<gXSize||x2<gXSize)){
				float y1=(yDrawStart-gTracks[i].p1.y)*invZoom-size/2;
				float y2=(yDrawStart-gTracks[i].p2.y)*invZoom-size/2;
				UInt32 v1=(int)gTracks[i].p1.y<<16;
				SInt32 u1=(int)gTracks[i].p1.x<<16;
				UInt32 v2=(int)gTracks[i].p2.y<<16;
				SInt32 u2=(int)gTracks[i].p2.x<<16;
				int dy=y2-y1;
				if(dy){
					int y;
					SInt32 dxdy=((x2-x1)/(y2-y1)*65536.0);
					SInt32 dudy=(u2-u1)/(y2-y1);
					SInt32 x=x1*65536.0;
					SInt32 u=u1,v=v1;
					int numBlocks=ceil(fabs(((x2-x1)/(y2-y1)))-size);
					if(numBlocks<0)numBlocks=0;
					numBlocks++;
					for(y=y1;y<y2;y++)
					{
						SInt32 blockU=u;
						SInt32 blockX=x>>16;
						int i;
						for(i=0;i<numBlocks;i++)
						{
							if(blockX>=0&&blockX<gXSize-size&&y>=0&&y<gYSize-size)
								DrawTextureBlock16(blockX,y,size,fixZoom,blockU,v,texture,hq);
							else
								DrawTextureBlockClipped16(blockX,y,size,fixZoom,blockU,v,texture,hq);
							blockU+=fixZoom;
							blockX++;
						}
						x+=dxdy;
						u+=dudy;
						v+=fixZoom;
					}
				}
				else{
					int x;
					if(x2<0)x2=0;
					if(x1>gXSize-size)x1=gXSize-size;
					if(x1<x2)
					{
						SInt32 u=u1;
						if(x1<0)x1=0;
						if(x2>gXSize-size)x2=gXSize-size;
						for(x=x1;x<x2;x++)
							DrawTextureBlockClipped16(x,y1,size,fixZoom,u+=fixZoom,v1,texture,hq);
					}
					else{
						SInt32 u=u2;
						if(x2<0)x2=0;
						if(x1>gXSize-size)x1=gXSize-size;
						for(x=x2;x<x1;x++)
							DrawTextureBlockClipped16(x,y1,size,fixZoom,u+=fixZoom,v1,texture,hq);
					}
				}
			}
		}
	}
}

void DrawMarksZoomed16(float xDrawStart,float yDrawStart,float zoom)
{
	float invZoom=1/zoom;
	SInt32 fixZoom=zoom*65536.0;
	int size=4.2*invZoom;
	UInt32 shiftClip=gXSize-size<<16;
	int i;
	int y1Clip=yDrawStart-(gYSize-(gCurPlayer->finishDelay?0:kInvLines))*zoom;
	int textureID=(*gRoadInfo).marks;
	UInt16 *texture=(UInt16*)GetUnsortedPackEntry(kPackTx16,textureID,0);
	const tHQImage *hq=HQAssets_Load("textures",textureID,0);
	int l=0,r=gMarkSize;
  	while(r-1>l)
		if(gMarks[(l+r)/2].p1.y+gMarks[(l+r)/2].p2.y>yDrawStart*2+kMaxMarkLength)
			l=(l+r)/2;
		else
			r=(l+r)/2;
	for(i=l;i<gMarkSize&&gMarks[i].p1.y+gMarks[i].p2.y>y1Clip*2-kMaxMarkLength;i++)
	{
		if(gMarks[i].p2.y<=yDrawStart+size&&gMarks[i].p1.y>y1Clip)
		{
			float x2=(gMarks[i].p2.x-xDrawStart)*invZoom-size/2;
			float x1=(gMarks[i].p1.x-xDrawStart)*invZoom-size/2;
			if((x1>-size||x2>-size)&&(x1<gXSize||x2<gXSize)){
				float y1=(yDrawStart-gMarks[i].p1.y)*invZoom-size/2;
				float y2=(yDrawStart-gMarks[i].p2.y)*invZoom-size/2;
				UInt32 v1=(int)gMarks[i].p1.y<<16;
				SInt32 u1=(int)gMarks[i].p1.x<<16;
				UInt32 v2=(int)gMarks[i].p2.y<<16;
				SInt32 u2=(int)gMarks[i].p2.x<<16;
				int dy=y2-y1;
				if(dy){
					int y;
					SInt32 dxdy=((x2-x1)/(y2-y1)*65536.0);
					SInt32 dudy=(u2-u1)/(y2-y1);
					SInt32 x=x1*65536.0;
					SInt32 u=u1,v=v1;
					int numBlocks=ceil(fabs(((x2-x1)/(y2-y1)))-size);
					if(numBlocks<0)numBlocks=0;
					numBlocks++;
					for(y=y1;y<y2;y++)
					{
						SInt32 blockU=u;
						SInt32 blockX=x>>16;
						int i;
						for(i=0;i<numBlocks;i++)
						{
							if(blockX>=0&&blockX<gXSize-size&&y>=0&&y<gYSize-size)
								DrawTextureBlock16(blockX,y,size,fixZoom,blockU,v,texture,hq);
							else
								DrawTextureBlockClipped16(blockX,y,size,fixZoom,blockU,v,texture,hq);
							blockU+=fixZoom;
							blockX++;
						}
						x+=dxdy;
						u+=dudy;
						v+=fixZoom;
					}
				}
				else{
					int x;
					if(x2<0)x2=0;
					if(x1>gXSize-size)x1=gXSize-size;
					if(x1<x2)
					{
						SInt32 u=u1;
						if(x1<0)x1=0;
						if(x2>gXSize-size)x2=gXSize-size;
						for(x=x1;x<x2;x++)
							DrawTextureBlockClipped16(x,y1,size,fixZoom,u+=fixZoom,v1,texture,hq);
					}
					else{
						SInt32 u=u2;
						if(x2<0)x2=0;
						if(x1>gXSize-size)x1=gXSize-size;
						for(x=x2;x<x1;x++)
							DrawTextureBlockClipped16(x,y1,size,fixZoom,u+=fixZoom,v1,texture,hq);
					}
				}
			}
		}
	}
}

void DrawSpriteLayerZoomed(float xDrawStart,float yDrawStart,float zoom,float tide,int layer)
{
	tObject *theObj=gFirstVisObj;
	float invZoom=1/zoom;
	float maxDrawOffs=128*invZoom;
	while(theObj!=gLastVisObj)
	{
		if(theObj->layer==layer)
			if(theObj->frame&&!theObj->jumpHeight)
			{
				float x=(theObj->pos.x-xDrawStart)*invZoom;
				float y=(yDrawStart-theObj->pos.y)*invZoom;
				if((y>-maxDrawOffs)&&(y<gYSize+maxDrawOffs))
				{
					float objTide=((*gRoadInfo).water&&(*theObj->type).flags2&kObjectFloating)?1+tide*0.5+tide*VEC2D_Value(theObj->velo)*0.04:1;
					DrawSprite(theObj->frame,x,y,theObj->dir,objTide*invZoom);
				}
			}
		theObj=(tObject*)theObj->next;
	}
}

void DrawSpriteLayerBlurZoomed(float xDrawStart,float yDrawStart,float zoom,float tide,int layer)
{
	tObject *theObj=gFirstVisObj;
	float invZoom=1/zoom;
	float maxDrawOffs=128*invZoom;
	while(theObj!=gLastVisObj)
	{
		if(theObj->layer==layer)
			if(theObj->frame&&!theObj->jumpHeight)
			{
				float x=(theObj->pos.x-xDrawStart)*invZoom;
				float y=(yDrawStart-theObj->pos.y)*invZoom;
				if((y>-maxDrawOffs)&&(y<gYSize+maxDrawOffs))
				{
					t2DPoint velDiff=VEC2D_Difference(gCameraObj->velo,theObj->velo);
					float objTide=((*gRoadInfo).water&&(*theObj->type).flags2&kObjectFloating)?1+tide*0.5+tide*VEC2D_Value(theObj->velo)*0.04:1;
					DrawSprite(theObj->frame,x,y,theObj->dir,objTide*invZoom);
					if(velDiff.x*velDiff.x+velDiff.y*velDiff.y>35*35)
					{
						t2DPoint fuzzPos=VEC2D_Scale(velDiff,kFrameDuration*kScale*0.2);
						DrawSpriteTranslucent(theObj->frame,x+fuzzPos.x,y+fuzzPos.y,theObj->dir-theObj->rotVelo*kFrameDuration,objTide*invZoom);
					}
				}
			}
		theObj=(tObject*)theObj->next;
	}
}

void DrawSpritesZoomed(float xDrawStart,float yDrawStart,float zoom)
{
	float invZoom=1/zoom;
	int layer;
	float tide=0.05*sin(gGameTime*2.6);
	tObject	*theObj;
	for(layer=0;layer<kNumLayers;layer++)
		if(gPrefs.motionBlur)
			DrawSpriteLayerBlurZoomed(xDrawStart,yDrawStart,zoom,tide,layer);
		else
			DrawSpriteLayerZoomed(xDrawStart,yDrawStart,zoom,tide,layer);
	theObj=gFirstVisObj;
	while(theObj!=gLastVisObj)
	{
		if(theObj->frame&&theObj->jumpHeight)
		{
			float x=(theObj->pos.x-xDrawStart)*invZoom;
			float y=(yDrawStart-theObj->pos.y)*invZoom;
			if((y>-256)&&(y<gYSize+256))
				DrawSprite(theObj->frame,x,y,theObj->dir,(theObj->jumpHeight*0.18+1)*invZoom);
		}
		theObj=(tObject*)theObj->next;
	}
}

static void BlendDot(tPixel *fb,int stride,int x,int y,int size,tPixel color,int alpha)
{
	int dx,dy;
	if(x<0||y<0||x+size>gXSize||y+size>gYSize)
		return;
	for(dy=0;dy<size;dy++)
		for(dx=0;dx<size;dx++)
		{
			tPixel *p=fb+(y+dy)*stride+x+dx;
			*p=BlendAlpha(color,*p,alpha);
		}
}

/* Shield crate: a pulsing ring around each shielded car; it blinks while
   a used-up shield still protects. */
static void DrawShieldRings(float xDrawStart,float yDrawStart,float zoom)
{
	tPixel *fb=(tPixel*)gBaseAddr;
	int stride=gRowBytes/4,slot,size=Platform_GetRenderScale()*2;
	for(slot=0;slot<kNetworkMaxPlayers;slot++)
	{
		tPlayer *player=&gPlayers[slot];
		tObject *car=player->obj;
		float cx,cy,radius,angle,step;
		int alpha;
		if(!car||(Network_IsActive()?!Network_IsSlotActive(slot):slot!=0))
			continue;
		if(!player->shield&&player->shieldGrace<=0)
			continue;
		if(!player->shield&&(gFrameCount/6)&1)
			continue;
		cx=(car->pos.x-xDrawStart)/zoom;
		cy=(yDrawStart-car->pos.y)/zoom;
		radius=(*car->type).length*kScale*1.35f/zoom;
		alpha=150+(int)(70*sin(gFrameCount*0.2));
		step=size*0.7f/radius;
		for(angle=0;angle<2*PI;angle+=step)
			BlendDot(fb,stride,(int)(cx+cos(angle)*radius),(int)(cy+sin(angle)*radius),
				size,PixelRGB(120,230,255),alpha);
	}
}

/* Rain crate: streaks over the view of a driver someone else rains on. */
static void DrawRain(void)
{
	tPixel *fb=(tPixel*)gBaseAddr;
	int stride=gRowBytes/4,i,scale=Platform_GetRenderScale();
	int y;
	if(!gPlayerObj||!Network_RainHits(gPlayerObj))
		return;
	/* A darker sky, then the streaks. */
	for(y=0;y<gYSize;y++)
	{
		tPixel *row=fb+y*stride;
		int x;
		for(x=0;x<gXSize;x++)
			row[x]=0xFF000000u|(((row[x]&0xFEFEFEu)>>1)+((row[x]&0xFCFCFCu)>>2));
	}
	for(i=0;i<240;i++)
	{
		int x=(int)((i*97u+gFrameCount*5u+(i*i)%53)%(unsigned)gXSize);
		int sy=(int)((i*61u*7u+gFrameCount*(18u+i%5))%(unsigned)gYSize);
		int len=(14+i%10)*scale,j;
		for(j=0;j<len;j+=scale)
			BlendDot(fb,stride,x-j/3,sy+j,scale,PixelRGB(225,235,255),150);
	}
}

void RenderFrame()
{
	float zoom=(0.5+gCurPlayer->zoomVelo/kZoomVeloFactor+gCameraObj->jumpHeight*0.05)/
		Platform_GetRenderScale();
	float xDrawStart=gCameraObj->pos.x-gXSize*kXCameraScreenPos*zoom;
	float yDrawStart=gCameraObj->pos.y+gYSize*kYCameraScreenPos*zoom;
	int preSpecBlit=gScreenBlitSpecial;
	if(gCurPlayer->finishDelay)
		yDrawStart=gLevelData->levelEnd+gYSize*kYCameraScreenPos*zoom;
	if(gPrefs.hiColor){
		DrawRoadZoomed16(xDrawStart,yDrawStart,zoom);
		DrawMarksZoomed16(xDrawStart,yDrawStart,zoom);
		DrawTracksZoomed16(xDrawStart,yDrawStart,zoom);
	}
	else{
		DrawRoadZoomed(xDrawStart,yDrawStart,zoom);
		DrawMarksZoomed(xDrawStart,yDrawStart,zoom);
		DrawTracksZoomed(xDrawStart,yDrawStart,zoom);
	}
	DrawParticleFXZoomed(xDrawStart,yDrawStart,zoom,0);
	DrawSpritesZoomed(xDrawStart,yDrawStart,zoom);
	DrawShieldRings(xDrawStart,yDrawStart,zoom);
	DrawParticleFXZoomed(xDrawStart,yDrawStart,zoom,1);
	DrawRain();
	DrawTextFXZoomed(xDrawStart,yDrawStart,zoom);
	if(DrawPanel())
		if(preSpecBlit!=gScreenBlitSpecial)
			RenderFrame();
		else
			Blit2Screen();
}

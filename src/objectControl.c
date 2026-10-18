#include "compat.h"
#include <math.h>
#include "input.h"
#include "levelgen.h"
#include "route.h"
#include "objects.h"
#include "screen.h"
#include "vec2d.h"
#include "roads.h"
#include "trig.h"
#include "gamesounds.h"
#include "objectcontrol.h"
#include "random.h"
#include "netgame.h"
#include "extended.h"
#include "preferences.h"

#define kOvertakeDist		7.0					//Distance in meters cars move to the left to overtake.
#define kControlSteering	0.8					//Maximal steering input given to ai cars.
#define kPreSteerTime		4*kFrameDuration
#define kTargetSwitchDist	50.0

#define kChaseSpeed			2.4
#define kChaseDist			40.0
#define kWaitSpeed			0.15
#define	kWaitDist			60.0
#define kFinalChaseSpeed	20.0
#define kCallFriendProbility	0.1				//Probility that a cop will call Assistance within each second
#define kCopExtraBurstVelo	30.0				//when cops move slower than this, they will get extra acceleration
#define kCopExtraBurst		1.8					//amount of extra acceleration recieved (1= no extra acceleration,2= double acceleration)

float GetCloseCar(t2DPoint pos)
{
	tObject *theObj=(tObject*)gFirstObj->next;
	float dist=INFINITY;
	while(theObj!=gFirstObj)
	{
		/* other vehicles and animals; loose things on the road (crates,
		   barrels) are no reason to stop: traffic knocks them aside */
		if(((theObj->type->flags&(kObjectBackCollFlag|kObjectBounce))&&
			(theObj->type->flags&kObjectWheelFlag||theObj->type->flags&kObjectBackCollFlag))||
			(theObj->type->flags2&kObjectRoadKill))
		{
			float xdist=theObj->pos.x-pos.x;
			float ydist=theObj->pos.y-pos.y;
			float sqdist=xdist*xdist+ydist*ydist;
			if(sqdist<dist)
				dist=sqdist;
		}
		theObj=theObj->next;
	}
	return dist;
}

void CheckTarget(tObject *theObj)
{
	tTrackInfo *track=(theObj->control==kObjectDriveUp||theObj->control==kObjectCopControl)?gTrackUp:gTrackDown;
	if(!track->num) return;
	t2DPoint targDist=VEC2D_Difference(P2D(track->track[theObj->target].x,track->track[theObj->target].y),theObj->pos);
	float sqTargDist=targDist.x*targDist.x+targDist.y*targDist.y;
	int passed=gRouteActive?Route_TargetPassed(track,theObj):	/* along the road, any direction */
		(theObj->control==kObjectDriveUp||theObj->control==kObjectCopControl)?track->track[theObj->target].y<theObj->pos.y:track->track[theObj->target].y>theObj->pos.y;
	if(sqTargDist<kTargetSwitchDist*kTargetSwitchDist||passed)
		theObj->target=(theObj->target+1)%track->num;
}

void TargetObject(tObject *theObj,tTrackInfoSeg *target)
{
	target->x=theObj->pos.x;
	target->y=theObj->pos.y;
	if(!(theObj->type->flags&kObjectKilledByCars))
		target->flags=kTargetNoStop;
	target->velo=kFinalChaseSpeed+VEC2D_Value(theObj->velo);
}

#define kOvertakeFrames	150	/* extended mode: how long traffic stays pulled out */

/* Extended mode: ordinary traffic may pull out past a slower car ahead. */
static int CanOvertake(const tObject *theObj)
{
	return Prefs_IsExtendedMode()&&theObj->type->flags&kObjectWheelFlag&&
		!(theObj->type->flags&kObjectCop)&&!Network_IsDriverObject(theObj)&&
		(theObj->control==kObjectDriveUp||theObj->control==kObjectDriveDown);
}

/* Is the lane offs to the side clear for lookAhead car lengths ahead? */
static int LaneClear(const tObject *theObj,int offs,float lookAhead)
{
	float ahead=(theObj->control==kObjectDriveUp?1:-1)*kMinCarDist*kScale*lookAhead;
	return GetCloseCar(P2D(theObj->pos.x+offs,theObj->pos.y+ahead))>
		kScale*kScale*kMinCarDist*kMinCarDist;
}

void ObjectFollow(tObject *theObj,tTrackInfoSeg *target,tInputData *input)
{
	t2DPoint objDir=P2D(sin(theObj->dir),cos(theObj->dir));
	if(Extended_PanicTurn(theObj,input))
		return;
	tObjectTypePtr objType=theObj->type;
	float velo=VEC2D_DotProduct(objDir,theObj->velo)<0?-VEC2D_Value(theObj->velo):VEC2D_Value(theObj->velo);
	int nearDriver=Network_NearestDriverDistance(theObj->pos.y)<kVisDist&&
		!((*objType).flags&kObjectHeliFlag);
	int dist=nearDriver?GetCloseCar(VEC2D_Sum(theObj->pos,VEC2D_Scale(objDir,kMinCarDist*0.5*kScale))):0;
	int overtaking=theObj->overtakeUntil>gFrameCount;
	int panicking=theObj->panicUntil>gFrameCount;
	int sideOffs=theObj->control==kObjectDriveUp?-kOvertakeDist*kScale:kOvertakeDist*kScale;
	int reckless=theObj->recklessUntil>gFrameCount;
	int dodging=false;
	float laneOffs=0;
	float speed=panicking?1.6:(reckless&&CanOvertake(theObj)?1.25:1);
	int offs;
	t2DPoint targetDiff;
	float targetDist;
	if(nearDriver&&!overtaking&&!panicking&&CanOvertake(theObj)&&
		dist<kScale*kScale*kMinCarDist*kMinCarDist*0.25)
	{
		/* Stuck behind a slow or stopped car: almost always try to get by,
		   on whichever side is free; otherwise only now and then. */
		int jammed=velo<0.5*target->velo;
		int wild=Network_HasAggressiveCops();	/* chaos modes overtake a lot */
		if(RanProb(reckless?(wild?0.8:0.4):jammed?(wild?0.9:0.5):(wild?0.3:0.08)))
		{
			int side=0;
			if(reckless||LaneClear(theObj,sideOffs,2))
				side=1;
			else if(LaneClear(theObj,-sideOffs,2))
				side=-1;
			if(side)
			{
				theObj->overtakeUntil=gFrameCount+kOvertakeFrames;
				theObj->overtakeSide=side;
				overtaking=true;
			}
		}
	}
	if(overtaking&&!panicking&&CanOvertake(theObj))
	{
		/* Something coming the other way: dodge to the other side, or pull
		   back in and queue. Drivers shaken by a crash react late. */
		int side=theObj->overtakeSide?theObj->overtakeSide:1;
		float lookAhead=reckless?1.5:3;
		if(!LaneClear(theObj,side*sideOffs,lookAhead))
		{
			if(LaneClear(theObj,-side*sideOffs,lookAhead))
				theObj->overtakeSide=-side;
			else
			{
				theObj->overtakeUntil=0;
				overtaking=false;
			}
		}
	}
	offs=(((*objType).flags&kObjectOvertake)&&(target->flags==kTargetOvertake))?sideOffs:0;
	if(overtaking)
		offs=(theObj->overtakeSide<0?-1:1)*sideOffs;
	/* A rammed car swerves from lane to lane while it flees. */
	if(panicking)
		offs=((gFrameCount/40)&1)?sideOffs:-sideOffs;
	/* Clear road crate: make way early for the driver, to the side away
	   from him but staying on this carriageway (not into the ditch). */
	if(!panicking&&theObj->type->flags&kObjectWheelFlag&&!(theObj->type->flags&kObjectCop)&&
		!Network_IsDriverObject(theObj)&&theObj!=gPlayerObj&&
		(theObj->control==kObjectDriveUp||theObj->control==kObjectDriveDown))
	{
		tObject *driver=Network_ClearRoadDriver(theObj);
		if(driver)
		{
			tRoad road=gRoadData+(int)(theObj->pos.y/2);
			float left,right,want;
			int side=theObj->pos.x>=driver->pos.x?1:-1;
			if(gRouteActive)	/* free-form road: the piece the car is on */
			{
				if(!Route_SpanAt(theObj->pos.x,theObj->pos.y,&left,&right))
					left=right=theObj->pos.x;
			}
			else if((*road)[1]==(*road)[2]){left=(*road)[0];right=(*road)[3];}
			else if(theObj->pos.x>=(*road)[2]){left=(*road)[2];right=(*road)[3];}
			else{left=(*road)[0];right=(*road)[1];}
			left+=18;
			right-=18;
			want=driver->pos.x+side*85;
			if(want<left||want>right)	/* no room on that side: the other */
				want=driver->pos.x-side*85;
			if(want<left)want=left;
			if(want>right)want=right;
			offs=(int)(want-target->x);
			dodging=true;
		}
	}
	/* Generated multi-lane road: each car keeps to its own lane beside the
	   track (not an overtaking offset: no extra speed). */
	if(!offs&&gGenLanes>1&&!theObj->ownTrack&&theObj->type->flags&kObjectWheelFlag&&!(theObj->type->flags&kObjectCop)&&
		!Network_IsDriverObject(theObj)&&theObj!=gPlayerObj&&
		(theObj->control==kObjectDriveUp||theObj->control==kObjectDriveDown))
	{
		if(!theObj->lane)
			theObj->lane=(SInt8)RanInt(1,7);
		laneOffs=LevelGen_LaneOffset(theObj->lane,target->x,target->y);
		if(theObj->control==kObjectDriveDown)
			laneOffs=-laneOffs;
	}
	targetDiff=VEC2D_Difference(P2D(target->x+offs+laneOffs,target->y),theObj->pos);
	targetDist=VEC2D_Value(targetDiff);
	input->throttle=(velo<(((*objType).flags&kObjectSlow)?0.6:1)*(offs&&!dodging?2:1)*speed*target->velo)?1:0;
	input->brake=0;
	if(nearDriver&&!overtaking&&!panicking)
		{
			if(dist<kScale*kScale*5*5)
				input->brake=0.1;
			else if(dist<kScale*kScale*kMinCarDist*kMinCarDist*0.5*0.5)
			{
				if(GetCloseCar(VEC2D_Sum(theObj->pos,VEC2D_Scale(objDir,kMinCarDist*kScale)))<kScale*kScale*8*8)
					input->throttle=0.0;
				else if(GetCloseCar(VEC2D_Sum(theObj->pos,VEC2D_Scale(objDir,kMinCarDist*0.2*kScale)))<kScale*kScale*3*3)
				{
					input->brake=0.4;
					if(!(theObj->velo.x>0.5||theObj->velo.y>0.5))
						if(RanProb(0.012))
							PlaySound(theObj->pos,theObj->velo,1.0,1.0,136);
				}				
			}
		}
	objDir=P2D(sin(theObj->dir+theObj->rotVelo*kPreSteerTime),cos(theObj->dir+theObj->rotVelo*kPreSteerTime));
	input->steering=(-VEC2D_CrossProduct(objDir,targetDiff)/(targetDist>100?targetDist:100))*kControlSteering;
	if(VEC2D_DotProduct(objDir,targetDiff)<0)
		if(!(objType->flags2&kObjectRoadKill))
			input->brake=0.6;
	if(theObj->control==kObjectCopControl)
		if(VEC2D_Value(theObj->velo)<kCopExtraBurstVelo)
			input->throttle*=kCopExtraBurst;
	if(input->brake)input->throttle=0;
}

void ObjectStartChase(tObject *theObj,int distance,float velo)
{
	ObjectStartChaseBehind(theObj,gPlayerObj,distance,velo);
}

/* Put a cop on the road behind a driver and send it after them. */
void ObjectStartChaseBehind(tObject *theObj,const tObject *driver,int distance,float velo)
{
	int target;
	if(gTrackUp->num<2)
		return;
	if(gRouteActive)
	{
		/* free-form road: behind the driver along the road, on its lane */
		float s=Route_Nearest(driver->pos,driver->routeS>0?driver->routeS:-1)-kStartChaseDist*kScale,dir;
		t2DPoint heading;
		if(s<50)
			s=50;
		Route_Point(s,Route_HalfWidth()*0.45f,&theObj->pos,&dir);
		heading=Route_Heading(s);
		theObj->dir=dir;
		theObj->velo=VEC2D_Scale(heading,velo);
		theObj->control=kObjectCopControl;
		theObj->target=Route_TrackTarget(gTrackUp,theObj->pos);
		return;
	}
	for(target=1;
		(target<gTrackUp->num)&&(gTrackUp->track[target].y<driver->pos.y-distance);
		target++);
	target++;
	/* The loop can leave target at num and the ++ pushes it to num+1; the
	 * track[target]/track[target-1] reads below need target <= num-1. */
	if(target>=gTrackUp->num)
		target=gTrackUp->num-1;
	theObj->target=target;
	theObj->control=kObjectCopControl;
	theObj->pos.y=driver->pos.y-kStartChaseDist*kScale;
	theObj->pos.x=gTrackUp->track[target-1].x+(gTrackUp->track[target].x-gTrackUp->track[target-1].x)/
					(gTrackUp->track[target].y-gTrackUp->track[target-1].y)*(theObj->pos.y-gTrackUp->track[target-1].y);
	theObj->dir=atan((theObj->pos.x-gTrackUp->track[target].x)/(theObj->pos.y-gTrackUp->track[target].y));
	theObj->velo=VEC2D_Scale(VEC2D_Norm(P2D((theObj->pos.x-gTrackUp->track[target].x),(theObj->pos.y-gTrackUp->track[target].y))),-velo);
}

void CallFriend(tObject *copObj)
{
	tObject	*theObj=gFirstObj->next;
	tObject *farObj=nil;
	int dist=0;
	while(theObj!=gFirstObj)
	{
		if((*theObj->type).flags&kObjectCop&&theObj!=gPlayerObj)
		{
			float xdist=theObj->pos.x-copObj->pos.x;
			float ydist=theObj->pos.y-copObj->pos.y;
			float sqdist=xdist*xdist+ydist*ydist;
			if(sqdist>dist)
			{
				dist=sqdist;
				farObj=theObj;
			}
		}
		theObj=theObj->next;
	}	
	if(farObj)
		if(fabs(farObj->pos.y-gPlayerObj->pos.y)>kVisDist*2)
			ObjectStartChase(farObj,gPlayerObj->pos.y-copObj->pos.y+50,kStartChaseSpeed);
}

void HandleShot(float,tObject*,t2DPoint);

/* Package and police modes: cops that hunt instead of escort. */
#define kHuntDist		(5*kChaseDist*kScale)	/* steer straight at the driver within this */
#define kHuntLead		0.35f					/* seconds ahead of the driver they aim */
#define kHuntExtraSpeed	15.0f

void CopFollow(tObject *theObj,tInputData *input)
{
	tTrackInfoSeg target;
	tObject *player=Network_GetPoliceTarget(theObj);
	UInt32 playerAddOns=Network_GetPlayerAddOns(player);
	float playerDeathDelay=Network_GetPlayerDeathDelay(player);
	int zombie=!(theObj->type->flags&kObjectCop);
	int hunt=Network_HasAggressiveCops()||zombie;
	if(theObj->warUntil>gFrameCount)
	{
		/* Car war crate: ram the enemy car off the road. */
		tObject *enemy=FindObjectByNetId(theObj->warTargetId);
		if(enemy&&enemy->control!=kObjectNoInput&&
			VEC2D_Value(VEC2D_Difference(enemy->pos,theObj->pos))<700)
		{
			target.x=enemy->pos.x+enemy->velo.x*kScale*kHuntLead;
			target.y=enemy->pos.y+enemy->velo.y*kScale*kHuntLead;
			target.flags=kTargetNoStop;
			target.velo=kFinalChaseSpeed+kHuntExtraSpeed+VEC2D_Value(enemy->velo);
			ObjectFollow(theObj,&target,input);
			input->brake=0;
			if(input->throttle<1)
				input->throttle=1;
			return;
		}
		theObj->warUntil=0;	/* enemy wrecked or gone: back to traffic */
	}
	/* During a car war nobody hunts the drivers: a car without an enemy
	   drives on as traffic. */
	if(zombie&&(Network_WarActive()||
		(!Network_IsZombieTarget(player)&&theObj->rageUntil<=gFrameCount)))
	{
		theObj->rageUntil=0;
		/* Zombie crate, road rage or car war over: back to ordinary traffic. */
		int down=cos(theObj->dir)<0;
		tTrackInfo *track=down?gTrackDown:gTrackUp;
		theObj->chaseTarget=NULL;
		theObj->warTargetId=0;
		theObj->control=down?kObjectDriveDown:kObjectDriveUp;
		{for(theObj->target=0;theObj->target<track->num-1&&
			(down?track->track[theObj->target].y>theObj->pos.y:track->track[theObj->target].y<theObj->pos.y);
			theObj->target++); if(gRouteActive)theObj->target=Route_TrackTarget(track,theObj->pos);}
		input->throttle=0;
		input->brake=0;
		input->steering=0;
		return;
	}
	if(!player||fabs(theObj->pos.y-player->pos.y)>(hunt?3*kVisDist:kVisDist)||
		(playerAddOns&kAddOnCop&&theObj->type->flags&kObjectCop)||	/* zombies have no radio */
		Network_DriverIsFinished(player)||playerDeathDelay)
		theObj->control=kObjectDriveUp;
	else if(hunt&&fabs(theObj->pos.y-player->pos.y)<kHuntDist)
	{
		/* Straight at where the driver will be, from any side, and ram. */
		target.x=player->pos.x+player->velo.x*kScale*kHuntLead;
		target.y=player->pos.y+player->velo.y*kScale*kHuntLead;
		target.flags=kTargetNoStop;
		target.velo=kFinalChaseSpeed+kHuntExtraSpeed+VEC2D_Value(player->velo);
		ObjectFollow(theObj,&target,input);
		input->brake=0;
		if(input->throttle<1)
			input->throttle=1;
	}
	else{
		if((player->pos.y>theObj->pos.y)&&(player->pos.y<theObj->pos.y+kChaseDist*kScale))
		{
			tRoad objRoadData=gRoadData+(int)(theObj->pos.y/2);			
			tRoad plrRoadData=gRoadData+(int)(player->pos.y/2);
			if(RanProb(kCallFriendProbility*kLowFrameDuration))
				CallFriend(theObj);
			if(((*objRoadData)[1]==(*objRoadData)[2])||(((*objRoadData)[1]>theObj->pos.x)==((*plrRoadData)[1]>player->pos.x)))
			{	
				tInputData playerTarget,roadTarget;
				TargetObject(player,&target);
				ObjectFollow(theObj,&target,&playerTarget);
				target=gTrackUp->track[theObj->target];
				target.velo=kFinalChaseSpeed+VEC2D_Value(player->velo);
				if(!(theObj->type->flags&kObjectKilledByCars))
					target.flags|=kTargetNoStop;
				ObjectFollow(theObj,&target,&roadTarget);
				*input=(fabs(playerTarget.steering-roadTarget.steering)>0.9)?roadTarget:playerTarget;
				return;
			}
		}
		if(player->pos.y>theObj->pos.y){
			target=gTrackUp->track[theObj->target];
			target.velo*=kChaseSpeed;
			if(player->pos.y<theObj->pos.y+5*kChaseDist*kScale)
			{
				if(!(theObj->type->flags&kObjectKilledByCars))
					target.flags|=kTargetNoStop;
				target.velo=kFinalChaseSpeed+VEC2D_Value(player->velo);
			}
			ObjectFollow(theObj,&target,input);
		}else{
			target=gTrackUp->track[theObj->target];
			target.velo=0;
			ObjectFollow(theObj,&target,input);
			input->brake=1;
			input->throttle=0;
		}
	}
}

void ObjectCrossRoad(tObject *theObj,tInputData *input)
{
	tTrackInfoSeg target;
	tRoad roadData=gRoadData+(int)(theObj->pos.y/2);
	target.y=theObj->pos.y;
	target.flags=theObj->type->flags&kObjectOvertake?kTargetNoStop:0;
	target.velo=INFINITY;
	if((*roadData)[3]+200<theObj->pos.x)
		target.x=(*roadData)[0];
	else if((*roadData)[0]-200>theObj->pos.x)
		target.x=(*roadData)[3];
	else if(theObj->dir>PI)	
		target.x=(*roadData)[0];
	else
		target.x=(*roadData)[3];
	ObjectFollow(theObj,&target,input);
}

/* Highway traffic on its own track: follows it on its lane (obj->lane),
   faster than the road's traffic; at the end of the highway it comes back
   at the start, out of every driver's sight. */
/* A highway car's lane: across the carriageway, to the right of its track. */
#define kHighwayLane 84.0f

static float HighwayLaneOffset(int lane)
{
	return ((lane&3)-1)*kHighwayLane;
}

/* The nearest car ahead of theObj in the given lane of its carriageway
   (heading hx,hy): how far ahead and how fast; 0 if none within range. */
/* The nearest car in a lane of the carriageway, between from and to
   along the way (negative: behind). A car changing into the lane already
   counts as in it (it has signalled). myLat: how far right of the track
   this car is. */
static int CarInLane(const tObject *theObj,float hx,float hy,float myLat,int lane,float from,float to,float *dist,float *speed)
{
	tObject *other=(tObject*)gFirstObj->next;
	float wanted=HighwayLaneOffset(lane),best=1e9f,range=fabsf(from)>fabsf(to)?fabsf(from):fabsf(to);
	int found=0;
	for(;other!=gFirstObj;other=(tObject*)other->next)
	{
		float dx,dy,along,lat,off;
		if(other==theObj||!(other->type->flags&kObjectWheelFlag))	/* (drives over ramps and crates) */
			continue;
		dx=other->pos.x-theObj->pos.x;
		dy=other->pos.y-theObj->pos.y;
		if(dx>range+60||dx<-range-60||dy>range+60||dy<-range-60)
			continue;
		along=dx*hx+dy*hy;
		if(along<from||along>to||fabsf(along)>=best)
			continue;
		if(!Route_SameLevel(theObj,other))	/* on a bridge over it, or below */
			continue;
		lat=myLat+dx*hy-dy*hx;
		off=fabsf(lat-wanted);
		if(off>44&&!(other->ownTrack==theObj->ownTrack&&(other->lane&3)==lane&&off<110))
			continue;
		best=fabsf(along);
		*dist=best;
		*speed=other->velo.x*hx+other->velo.y*hy;
		found=1;
	}
	return found;
}

/* A highway car comes out of the tunnel at the track's start, in a lane
   that is free there; 0 if all are taken. */
static int OwnTrackRestart(tObject *theObj,tTrackInfo *track)
{
	float dir=atan2f((float)(track->track[1].x-track->track[0].x),(float)(track->track[1].y-track->track[0].y));
	float hx=sinf(dir),hy=cosf(dir);
	int k;
	for(k=0;k<3;++k)
	{
		int lane=((theObj->lane&3)+k)%3;
		float lateral=HighwayLaneOffset(lane);
		t2DPoint at=P2D(track->track[0].x+hy*lateral,track->track[0].y-hx*lateral);
		tObject *other=(tObject*)gFirstObj->next;
		int taken=0;
		for(;other!=gFirstObj&&!taken;other=(tObject*)other->next)
			if(other!=theObj&&other->type->flags&(kObjectWheelFlag|kObjectBounce))
			{
				/* the lane free a good way ahead (no queue to run into) */
				float dx=other->pos.x-at.x,dy=other->pos.y-at.y,along=dx*hx+dy*hy,across=dx*hy-dy*hx;
				if(along>-80&&along<350&&fabsf(across)<50)
					taken=1;
			}
		if(taken)
			continue;
		theObj->lane=(SInt8)lane;
		theObj->pos=at;
		theObj->dir=dir;
		theObj->rotVelo=0;
		theObj->damage=0;
		theObj->stuckSince=0;
		theObj->laneLatSet=0;
		theObj->jumpHeight=0;
		theObj->jumpVelo=0;
		theObj->velo=VEC2D_Scale(P2D(hx,hy),track->track[0].velo*0.8f);
		theObj->target=1;
		return 1;
	}
	return 0;
}

/* Highway traffic on its own track: its own driving, lane-disciplined. It
   only brakes for what is in its own lane, changes lanes round a stopped
   or slow car, and never pulls out the way country road traffic does. At
   the highway's ends it goes into the tunnel and comes out of the other
   one. */
static void OwnTrackFollow(tObject *theObj,tInputData *input)
{
	tTrackInfo *track=(tTrackInfo*)theObj->ownTrack;
	float tx,ty,length,hx,hy,lateral,velo,wanted,ahead,aheadSpeed,myLat;
	int ramp;
	t2DPoint target,objDir,diff;
	int i=theObj->target;
	if(i<1)i=1;
	if(i>=(int)track->num)i=track->num-1;
	tx=(float)(track->track[i].x-track->track[i-1].x);
	ty=(float)(track->track[i].y-track->track[i-1].y);
	length=sqrtf(tx*tx+ty*ty);
	if(length<1)length=1;
	hx=tx/length;
	hy=ty/length;
	if(((theObj->pos.x-track->track[i].x)*tx+(theObj->pos.y-track->track[i].y)*ty)/length>-40)
	{
		theObj->target=++i;
		Route_HighwayCarAt(theObj,i);
		if(!theObj->ownTrack)	/* turned off onto the road */
			return;
		if(i>=(int)track->num&&Route_OwnTrackEnd(theObj))	/* off a ramp */
			return;
		if(i>=(int)track->num&&Route_HighwayFull(track))
		{
			/* too busy: it does not come out again (out of the way, it is
			   cleared away like a wreck nobody sees) */
			theObj->control=kObjectNoInput;
			theObj->velo=P2D(0,0);
			theObj->pos=P2D(-1e5f,-1e5f);
			return;
		}
		if(i>=(int)track->num)
		{
			/* in the tunnel at the end: out of the one at the start */
			if(!OwnTrackRestart(theObj,track))
				theObj->target=track->num-1;	/* (the way out is full: waits in the dark) */
			return;
		}
	}
	/* stuck for a while where nobody sees it: on again from the tunnel */
	if(VEC2D_Value(theObj->velo)<3)
	{
		if(!theObj->stuckSince)
			theObj->stuckSince=gFrameCount;
		else if(gFrameCount-theObj->stuckSince>4*60&&Network_NearestDriverDistance2D(theObj->pos)>800&&
			OwnTrackRestart(theObj,track))
			return;
	}
	else
		theObj->stuckSince=0;
	velo=theObj->velo.x*hx+theObj->velo.y*hy;
	myLat=(theObj->pos.x-track->track[i].x)*hy-(theObj->pos.y-track->track[i].y)*hx;
	if(!theObj->laneLatSet)
	{
		theObj->laneLat=myLat;
		theObj->laneLatSet=1;
	}
	ramp=Route_IsRamp(track);
	if(ramp)
		theObj->lane=1;	/* one lane */
	wanted=track->track[i].velo*(ramp?1:1.05f-0.1f*(theObj->lane&3));	/* the left lane fastest */
	/* something slower ahead in the lane: another lane if there is room
	   ahead and behind (more behind for a faster car coming), else keep a
	   distance that grows with the speed */
	if(CarInLane(theObj,hx,hy,myLat,theObj->lane&3,0,480,&ahead,&aheadSpeed)&&aheadSpeed<wanted-2)
	{
		float gap=80+(velo>0?velo:0)*3.5f;
		int side,changed=0;
		int settled=fabsf(theObj->laneLat-HighwayLaneOffset(theObj->lane))<4;
		if(ahead<gap+260&&settled&&theObj->laneUntil<gFrameCount&&!ramp)
			for(side=-1;side<=1&&!changed;side+=2)
			{
				int lane=(theObj->lane&3)+side;
				float d,sp;
				if(lane<0||lane>2)
					continue;
				if(CarInLane(theObj,hx,hy,myLat,lane,0,gap+160,&d,&sp)&&(sp<wanted-2||d<130))
					continue;	/* slow or close ahead there */
				if(CarInLane(theObj,hx,hy,myLat,lane,-400,0,&d,&sp)&&
					d<130+(sp>velo?(sp-velo)*7:0))
					continue;	/* someone beside or coming up behind */
				theObj->lane=(SInt8)lane;
				theObj->laneUntil=gFrameCount+150;
				changed=1;
			}
		if(!changed)
		{
			float follow=aheadSpeed+(ahead-gap)*0.12f;
			if(ahead<70)
				follow=0;
			if(follow<wanted)
				wanted=follow<0?0:follow;
		}
	}
	/* across to the lane smoothly (a lane change takes about a second) */
	{
		float to=HighwayLaneOffset(theObj->lane),step=1.4f;
		if(theObj->laneLat<to-step)theObj->laneLat+=step;
		else if(theObj->laneLat>to+step)theObj->laneLat-=step;
		else theObj->laneLat=to;
	}
	lateral=theObj->laneLat;
	target=P2D(track->track[i].x+hy*lateral,track->track[i].y-hx*lateral);
	/* steer for a point ahead on the lane */
	{
		float look=70+(velo>0?velo:0)*2.5f;
		target=VEC2D_Sum(target,P2D(hx*look,hy*look));
	}
	objDir=P2D(sin(theObj->dir+theObj->rotVelo*kPreSteerTime),cos(theObj->dir+theObj->rotVelo*kPreSteerTime));
	diff=VEC2D_Difference(target,theObj->pos);
	{
		float dist=VEC2D_Value(diff);
		input->steering=(-VEC2D_CrossProduct(objDir,diff)/(dist>100?dist:100))*kControlSteering;
	}
	input->throttle=velo<wanted?1:0;
	input->brake=velo>wanted+2?(velo>wanted+8?1:0.3f):0;
	if(input->brake)input->throttle=0;
	input->handbrake=0;
	input->reverse=0;
}

void ObjectControl(tObject *theObj,tInputData *input)
{
	int ownTrack=theObj->ownTrack&&theObj->control==kObjectDriveUp&&gRouteActive,waitToCross=0;
	float before=theObj->routeS;
	if(!ownTrack)
		CheckTarget(theObj);
	/* a road car at a highway junction: stays on the highway or not */
	if(!ownTrack&&gRouteActive&&theObj!=gPlayerObj&&!theObj->replica&&
		(theObj->control==kObjectDriveUp||theObj->control==kObjectDriveDown)&&before>0)
		waitToCross=Route_RoadCarAt(theObj,before);
	ownTrack=theObj->ownTrack&&theObj->control==kObjectDriveUp&&gRouteActive;
	if(theObj==gPlayerObj)
	{
		theObj->input=*input;
		if(gCurPlayer->finishDelay)
		{
			theObj->input.handbrake=1;
			theObj->input.throttle=0;
		}
	}
	else if(Network_GetLocalInput(theObj,&theObj->input))
	{
	}
	else if(!((*theObj->type).flags2&kObjectDamageble)||theObj->damage<(*theObj->type).maxDamage)
		switch(theObj->control)
		{
			case kObjectNoInput:
				theObj->input.brake=0;theObj->input.throttle=0;theObj->input.steering=0;
				break;
			case kObjectDriveUp:			
				if(ownTrack)
					OwnTrackFollow(theObj,&theObj->input);
				else
					ObjectFollow(theObj,&gTrackUp->track[theObj->target],&theObj->input);
				break;
			case kObjectDriveDown:
				ObjectFollow(theObj,&gTrackDown->track[theObj->target],&theObj->input);
				if(waitToCross)	/* at a highway junction: lets the highway traffic pass */
				{
					theObj->input.throttle=0;
					theObj->input.brake=1;
				}
				break;
			case kObjectCrossRoad:
				ObjectCrossRoad(theObj,&theObj->input);
				break;
			case kObjectCopControl:
				CopFollow(theObj,&theObj->input);
				break;
		}
	else{
		theObj->input.brake=0;
		theObj->input.throttle=0;
		theObj->input.steering=0;
	}
}

#include "route.h"
#include <math.h>
#include "extended.h"
#include "objectcontrol.h"
#include "packs.h"
#include "random.h"
#include "roads.h"
#include "vec2d.h"
#include "trig.h"
#include "netgame.h"
#include "preferences.h"
#include "gamesounds.h"

#define kSwarmCops		6
#define kJamRows		4
#define kJamColumns		4
#define kJamDistance	1400	/* how far ahead of the leader the jam starts */
#define kCopEntryDistance 700	/* swarm cops start this far ahead, off screen */
#define kCopEntryGap	90		/* road between two swarm cops */

/* Object type resource ID for a type pointer, by walking the type pack. */
static int TypeIDOf(tObjectTypePtr type)
{
	tPackHeader *pack=(tPackHeader*)*gPacks[kPackObTy];
	int count=SWAP16S(pack[0].id),i;
	for(i=1;i<=count;i++)
	{
		int id=SWAP16S(pack[i].id);
		if((tObjectTypePtr)GetUnsortedPackEntry(kPackObTy,id,nil)==type)
			return id;
	}
	return 0;
}

static int IsGroundCop(tObjectTypePtr type)
{
	return type->flags&kObjectCop&&type->flags&kObjectWheelFlag&&
		!(type->flags&kObjectHeliFlag);
}

/* Swarms and police mode send cars (or boats), not motorbikes. */
static int IsCopCar(tObjectTypePtr type)
{
	return IsGroundCop(type)&&type->width>=1.0f;
}

/* A cop car type that suits this level: one already on the road if possible. */
static int FindCopType(void)
{
	tObject *theObj=(tObject*)gFirstObj->next;
	tPackHeader *pack;
	int count,i;
	while(theObj!=gFirstObj)
	{
		if(IsCopCar(theObj->type))
			return TypeIDOf(theObj->type);
		theObj=(tObject*)theObj->next;
	}
	pack=(tPackHeader*)*gPacks[kPackObTy];
	count=SWAP16S(pack[0].id);
	for(i=1;i<=count;i++)
	{
		int id=SWAP16S(pack[i].id);
		tObjectTypePtr type=(tObjectTypePtr)GetUnsortedPackEntry(kPackObTy,id,nil);
		if(type&&IsCopCar(type)&&
			!(type->flags2&kObjectFloating)==!(*gRoadInfo).water)
			return id;
	}
	return 0;
}

/* Up to max distinct ordinary traffic types driving in this level. */
static int FindTrafficTypes(int *ids,int max)
{
	tObject *theObj=(tObject*)gFirstObj->next;
	int found=0,i;
	while(theObj!=gFirstObj&&found<max)
	{
		tObjectTypePtr type=theObj->type;
		if(type->flags&kObjectWheelFlag&&!(type->flags&kObjectCop)&&
			theObj->control==kObjectDriveUp)
		{
			int id=TypeIDOf(type),known=0;
			for(i=0;i<found;i++)
				if(ids[i]==id)
					known=1;
			if(id&&!known)
				ids[found++]=id;
		}
		theObj=(tObject*)theObj->next;
	}
	return found;
}

/* Extra weapon packages about every gap pixels of road, on a random side.
   Placed by the host only on a LAN (clients get its). */
static void AddPackages(float gap)
{
	/* The same package as the level's own (the game data has more than one
	   look); without any in the level, the usual crate, or the one for water. */
	tObject *theObj=(tObject*)gFirstObj->next;
	int packageType=(*gRoadInfo).water?255:158;
	float y;
	while(theObj!=gFirstObj)
	{
		if(theObj->type->flags&kObjectBonusFlag&&theObj->type->flags2&kObjectAddOnFlag)
		{
			packageType=theObj->typeRes;
			break;
		}
		theObj=(tObject*)theObj->next;
	}
	if(!GetUnsortedPackEntry(kPackObTy,packageType,nil))
		return;
	if(gRouteActive)
	{
		/* free-form road: along it, anywhere across */
		for(y=900;y<Route_FinishS()-300;y+=gap+RanFl(-60,60))
		{
			tObject *package=NewObject(gFirstObj,packageType);
			if(package)
			{
				/* along the road (also tells bridge from road below) */
				Route_Point(y,RanFl(-Route_HalfWidth()+20,Route_HalfWidth()-20),&package->pos,&package->dir);
			}
		}
		return;
	}
	for(y=900;y<gLevelData->levelEnd-300;y+=gap+RanFl(-60,60))
	{
		tRoad road=gRoadData+(int)(y/2);
		float left=(*road)[0]+20,right=(*road)[1]-20;
		tObject *package;
		/* Pick one carriageway; the other one if this is too narrow. */
		if((RanProb(0.5)&&(*road)[3]-(*road)[2]>40)||right-left<0)
		{
			left=(*road)[2]+20;
			right=(*road)[3]-20;
		}
		if(right-left<0)
			continue;
		package=NewObject(gFirstObj,packageType);
		if(!package)
			continue;
		package->pos=P2D(RanFl(left,right),y);
		package->dir=0;
	}
}

/* Wildlife mode: animals that cross the road (the game's road-kill types),
   placed at the road side facing the road; they walk across and back. */
static const SInt16 kAnimalTypes[]={163,185};

void Extended_SpawnAnimal(float y)
{
	tRoad road;
	tObject *animal;
	int index=(int)(y/2),left=RanProb(0.5);
	if(gRouteActive)
	{
		/* free-form road: y is how far along it; beside it, off any road */
		t2DPoint pos;
		float dir;
		if((*gRoadInfo).water||y<300||y>Route_Length()-300)
			return;
		Route_Point(y,(left?-1:1)*(Route_HalfWidth()+RanFl(20,90)),&pos,&dir);
		if(Route_BackCollision(pos,0)!=2)
			return;
		animal=NewObject(gFirstObj,kAnimalTypes[RanInt(0,2)]);
		if(!animal)
			return;
		animal->pos=pos;
		animal->dir=dir+(left?PI/2:-PI/2);	/* facing the road */
		animal->control=kObjectCrossRoad;
		return;
	}
	if(index<1||index>=(int)*gRoadLenght-1||(*gRoadInfo).water)
		return;
	road=gRoadData+index;
	animal=NewObject(gFirstObj,kAnimalTypes[RanInt(0,2)]);
	if(!animal)
		return;
	animal->pos=P2D(left?(*road)[0]-RanFl(20,90):(*road)[3]+RanFl(20,90),y);
	animal->dir=left?PI/2:3*PI/2;	/* facing the road */
	animal->control=kObjectCrossRoad;
}

int Extended_CountAnimalsNear(const tObject *target)
{
	tObject *theObj=(tObject*)gFirstObj->next;
	int count=0;
	while(theObj!=gFirstObj)
	{
		if(theObj->control==kObjectCrossRoad&&fabs(theObj->pos.y-target->pos.y)<1200)
			count++;
		theObj=(tObject*)theObj->next;
	}
	return count;
}

/* At the start of a level: an animal about every 220 pixels of road. */
void Extended_AddWildlife(void)
{
	float y;
	if((*gRoadInfo).water)
		return;
	for(y=700;y<(gRouteActive?Route_FinishS():gLevelData->levelEnd)-200;y+=220+RanFl(-60,60))
		Extended_SpawnAnimal(y);
}

/* Crate settings: fewer (half of the level's add-on packages), the level's
   own, more (one every 640 pixels) or many (every 320 pixels). */
void Extended_AdjustPackages(int amount)
{
	if(amount==kPackagesFew)
	{
		tObject *theObj=(tObject*)gFirstObj->next;
		int keep=0;
		while(theObj!=gFirstObj)
		{
			tObject *next=(tObject*)theObj->next;
			if(theObj->type->flags&kObjectBonusFlag&&theObj->type->flags2&kObjectAddOnFlag&&
				(keep^=1)==0)
				RemoveObject(theObj);
			theObj=next;
		}
	}
	else if(amount==kPackagesMore)
		AddPackages(640);
	else if(amount==kPackagesMany)
		AddPackages(320);
}

/* Extended mode, after the level's own objects are placed: about half as
   much traffic again, only in gaps that are still free. */
void Extended_AddTraffic(void)
{
	tObject *theObj=(tObject*)gFirstObj->next;
	int count=0,i,extra;
	tObjectTypePtr types[512];
	while(theObj!=gFirstObj&&count<512)
	{
		if(theObj->type->flags&kObjectWheelFlag&&!(theObj->type->flags&kObjectCop)&&
			(theObj->control==kObjectDriveUp||theObj->control==kObjectDriveDown))
			types[count++]=theObj->type;
		theObj=(tObject*)theObj->next;
	}
	/* Calm in the normal game, crowded in the chaos modes. */
	for(i=0,extra=0;i<(Network_HasAggressiveCops()?count/2:count/4);i++)
	{
		int id=TypeIDOf(types[RanInt(0,count)]),control,found;
		float dir=-1;
		tObject *car;
		t2DPoint pos;
		if(!id)
			continue;
		pos=GetUniquePosLimited(0,0,&dir,&control,40,&found);
		if(!found)
			break;	/* the road is full */
		car=NewObject(gFirstObj,id);
		if(!car)
			continue;
		car->pos=pos;
		car->dir=dir;
		car->control=control;
		if(control==kObjectDriveUp)
			{for(car->target=0;
				car->target<gTrackUp->num&&gTrackUp->track[car->target].y<car->pos.y;
				car->target++); if(gRouteActive)car->target=Route_TrackTarget(gTrackUp,car->pos);}
		else
			{for(car->target=0;
				car->target<gTrackDown->num&&gTrackDown->track[car->target].y>car->pos.y;
				car->target++); if(gRouteActive)car->target=Route_TrackTarget(gTrackDown,car->pos);}
		extra++;
	}
}

/* Normal game: only hard crashes shake a driver, and not for long. The
   chaos modes (packages, police, zombie) keep the wild traffic. */
#define kPanicFrames		150	/* 2.5 seconds */
#define kRecklessFrames		240	/* then 4 seconds of reckless driving */
#define kCrashSpeed			14	/* impact speed (m/s) that shakes a driver */
#define kTurnRoundChance	0.15
#define kWildPanicFrames	240
#define kWildRecklessFrames	600
#define kWildCrashSpeed		8
#define kWildTurnRoundChance 0.35

/* Extended mode: ordinary traffic that crashes panics. It floors it and
   swerves away, or turns round and flees the other way; afterwards it drives
   recklessly for a while. Without a crash traffic behaves normally. */
void Extended_Panic(tObject *car)
{
	tObjectTypePtr type=car->type;
	if(!(Prefs_IsExtendedMode()||Network_UsesCrateSettings())||!(type->flags&kObjectWheelFlag)||
		type->flags&kObjectCop||Network_IsDriverObject(car)||
		(car->control!=kObjectDriveUp&&car->control!=kObjectDriveDown)||
		car->panicUntil>gFrameCount)
		return;
	{
		int wild=Network_HasAggressiveCops();
		car->panicUntil=gFrameCount+(wild?kWildPanicFrames:kPanicFrames);
		car->recklessUntil=car->panicUntil+(wild?kWildRecklessFrames:kRecklessFrames);
	}
	/* (not on a highway: there nobody turns round) */
	if(!Route_OnHighway(car)&&RanProb(Network_HasAggressiveCops()?kWildTurnRoundChance:kTurnRoundChance))
	{
		/* Turn round: steer hard until facing the other way (see
		   Extended_PanicTurn), then flee in the opposite direction. */
		car->panicTurn=RanProb(0.5)?1:-1;
		car->panicTurnDir=car->dir+PI;
	}
}

static void ReverseTravelDirection(tObject *car)
{
	if(car->control==kObjectDriveUp)
	{
		car->control=kObjectDriveDown;
		{for(car->target=0;
			car->target<gTrackDown->num-1&&gTrackDown->track[car->target].y>car->pos.y;
			car->target++); if(gRouteActive)car->target=Route_TrackTarget(gTrackDown,car->pos);}
	}
	else
	{
		car->control=kObjectDriveUp;
		{for(car->target=0;
			car->target<gTrackUp->num-1&&gTrackUp->track[car->target].y<car->pos.y;
			car->target++); if(gRouteActive)car->target=Route_TrackTarget(gTrackUp,car->pos);}
	}
}

/* Drives a panicking car through its U-turn. Returns 1 while it is still
   turning (the input is set here), 0 when it drives normally. */
int Extended_PanicTurn(tObject *car,tInputData *input)
{
	float diff;
	if(!car->panicTurn)
		return 0;
	if(car->panicUntil<=gFrameCount)
	{
		car->panicTurn=0;	/* took too long: carry on the old way */
		return 0;
	}
	diff=fmodf(car->dir-car->panicTurnDir+3*PI,2*PI)-PI;
	if(fabsf(diff)<0.5f)
	{
		car->panicTurn=0;
		ReverseTravelDirection(car);
		return 0;
	}
	input->steering=car->panicTurn;
	input->throttle=0.7f;
	input->brake=0;
	input->handbrake=0.3f;
	input->reverse=0;
	return 1;
}

/* A collision between two objects: traffic involved panics if it was hard. */
#define kRageFrames (12*60)

static int IsDriverCar(const tObject *obj)
{
	return obj==gPlayerObj||Network_IsDriverObject(obj);
}

/* Road rage: a car rammed by a driver chases him for a while (with the
   hunting control) instead of fleeing. */
static void RoadRage(tObject *car,tObject *driver)
{
	car->chaseTarget=driver;
	car->rageUntil=gFrameCount+kRageFrames;
	car->panicUntil=0;
	car->panicTurn=0;
	car->control=kObjectCopControl;
	{for(car->target=0;
		car->target<gTrackUp->num-1&&gTrackUp->track[car->target].y<car->pos.y;
		car->target++); if(gRouteActive)car->target=Route_TrackTarget(gTrackUp,car->pos);}
	PlaySound(car->pos,car->velo,1.0,1.0,136);	/* horn */
}

void Extended_Crash(tObject *a,tObject *b)
{
	t2DPoint impact=VEC2D_Difference(a->velo,b->velo);
	tObject *driver,*car;
	if(!(Prefs_IsExtendedMode()||Network_UsesCrateSettings())||
		VEC2D_Value(impact)<(Network_HasAggressiveCops()?kWildCrashSpeed:kCrashSpeed))
		return;
	/* Rammed by a driver: in multiplayer half of the cars, alone a quarter,
	   go after him instead of panicking. */
	driver=IsDriverCar(a)?a:IsDriverCar(b)?b:NULL;
	car=driver==a?b:a;
	if(driver&&!IsDriverCar(car)&&!car->replica&&car->type->flags&kObjectWheelFlag&&
		!(car->type->flags&kObjectCop)&&
		(car->control==kObjectDriveUp||car->control==kObjectDriveDown)&&
		RanProb(Network_UsesCrateSettings()?0.5:0.25))
	{
		RoadRage(car,driver);
		return;
	}
	Extended_Panic(a);
	Extended_Panic(b);
}

/* Package mode: the swarm brings the cops around its target up to at
   least kPackageSwarmCops. */
#define kPackageSwarmCops 20

void Extended_SpawnCopSwarm(tObject *target)
{
	if(!target)
		return;
	if(Network_IsPackageMode())
		Extended_SpawnCops(target,kPackageSwarmCops-Extended_CountCopsNear(target));
	else
		Extended_SpawnCops(target,kSwarmCops);
}

int Extended_CountCopsNear(const tObject *target)
{
	tObject *theObj=(tObject*)gFirstObj->next;
	int count=0;
	while(theObj!=gFirstObj)
	{
		if(IsGroundCop(theObj->type)&&fabs(theObj->pos.y-target->pos.y)<1500)
			count++;
		theObj=(tObject*)theObj->next;
	}
	return count;
}

/* Cops that come from behind and hunt target. */
void Extended_SpawnCops(tObject *target,int count)
{
	int copType=FindCopType(),i;
	if(!target||!copType)
		return;
	for(i=0;i<count;i++)
	{
		tObject *cop=NewObject(gFirstObj,copType);
		/* Like the level's own cops: they come down the oncoming lanes from
		   above the screen, staggered into a stream, and turn round to
		   chase the driver once he is past them. */
		float y=target->pos.y+kCopEntryDistance+i*kCopEntryGap+RanFl(0,30);
		int track;
		if(!cop)
			continue;
		cop->chaseTarget=target;
		if(gRouteActive)
		{
			/* free-form road: down the oncoming lane ahead along the road */
			float s=Route_Nearest(target->pos,target->routeS>0?target->routeS:-1)+
				kCopEntryDistance+i*kCopEntryGap+RanFl(0,30),dir;
			if(s>Route_FinishS()-100)
			{
				ObjectStartChaseBehind(cop,target,kStartChaseDist*kScale,kStartChaseSpeed+10);
				continue;
			}
			Route_Point(s,-Route_HalfWidth()*0.45f+((i%3)-1)*14,&cop->pos,&dir);
			cop->dir=dir+PI;
			cop->velo=VEC2D_Scale(Route_Heading(s),-kStartChaseSpeed);
			cop->control=kObjectDriveDown;
			cop->target=Route_TrackTarget(gTrackDown,cop->pos);
			continue;
		}
		if(gTrackDown->num<2||y>gLevelData->levelEnd-100)
		{
			/* Too close to the finish: join the chase from behind. */
			ObjectStartChaseBehind(cop,target,kStartChaseDist*kScale,kStartChaseSpeed+10);
			cop->pos.y-=(i%4)*45;
			continue;
		}
		for(track=1;track<gTrackDown->num-1&&gTrackDown->track[track].y>y;track++);
		cop->target=track;
		cop->control=kObjectDriveDown;
		cop->pos.y=y;
		cop->pos.x=gTrackDown->track[track-1].x+(gTrackDown->track[track].x-gTrackDown->track[track-1].x)*
			(y-gTrackDown->track[track-1].y)/(gTrackDown->track[track].y-gTrackDown->track[track-1].y);
		cop->pos.x+=((i%3)-1)*14;
		cop->velo=VEC2D_Scale(VEC2D_Norm(VEC2D_Difference(
			P2D(gTrackDown->track[track].x,gTrackDown->track[track].y),cop->pos)),kStartChaseSpeed);
		cop->dir=PI;
	}
}

void Extended_SpawnTrafficJam(tObject *target)
{
	int types[6],typeCount=FindTrafficTypes(types,6),row,column;
	if(!target||!typeCount)
		return;
	if(gRouteActive)
	{
		/* free-form road: the jam ahead along the road, across its width */
		float start=Route_Nearest(target->pos,target->routeS>0?target->routeS:-1)+kJamDistance;
		for(row=0;row<kJamRows;row++)
			for(column=0;column<kJamColumns;column++)
			{
				float hw=Route_HalfWidth()-24,dir;
				tObject *car=NewObject(gFirstObj,types[RanInt(0,typeCount)]);
				if(!car)
					continue;
				Route_Point(start+row*70,-hw+2*hw*(column+0.5f)/kJamColumns,&car->pos,&dir);
				car->dir=dir;
				car->velo=VEC2D_Scale(Route_Heading(start+row*70),5);
				car->control=kObjectDriveUp;
				car->target=Route_TrackTarget(gTrackUp,car->pos);
			}
		return;
	}
	for(row=0;row<kJamRows;row++)
	{
		float y=target->pos.y+kJamDistance+row*70;
		int roadIndex=(int)(y/2);
		tRoad road;
		float left,right;
		if(roadIndex<0||roadIndex>=(int)*gRoadLenght)
			continue;
		road=gRoadData+roadIndex;
		left=(*road)[0]+24;
		right=(*road)[3]-24;
		for(column=0;column<kJamColumns;column++)
		{
			tObject *car=NewObject(gFirstObj,types[RanInt(0,typeCount)]);
			if(!car)
				continue;
			car->pos=P2D(left+(right-left)*(column+0.5f)/kJamColumns,y);
			car->dir=0;
			car->velo=P2D(0,5);
			car->control=kObjectDriveUp;
			{for(car->target=0;
				car->target<gTrackUp->num-1&&gTrackUp->track[car->target].y<car->pos.y;
				car->target++); if(gRouteActive)car->target=Route_TrackTarget(gTrackUp,car->pos);}
		}
	}
}

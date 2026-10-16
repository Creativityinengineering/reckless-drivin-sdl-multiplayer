#include <SDL.h>
#include <stdio.h>
#include "jobs.h"

#define kMaxWorkers 15

static SDL_Thread *sWorkers[kMaxWorkers];
static SDL_sem *sStartSem,*sDoneSem;
static int sWorkerCount=-1;
static volatile int sQuit;
static tJobFunc sFunc;
static void *sContext;
static int sCount;
static SDL_atomic_t sNextIndex;

static void RunPendingJobs(void)
{
	int index;
	while((index=SDL_AtomicAdd(&sNextIndex,1))<sCount)
		sFunc(sContext,index);
}

static int WorkerMain(void *unused)
{
	(void)unused;
	for(;;)
	{
		SDL_SemWait(sStartSem);
		if(sQuit)
			break;
		RunPendingJobs();
		SDL_SemPost(sDoneSem);
	}
	return 0;
}

static void StartWorkers(void)
{
	int i,count=SDL_GetCPUCount()-1;
	sWorkerCount=0;
	if(count>kMaxWorkers)
		count=kMaxWorkers;
	if(count<1)
		return;
	sStartSem=SDL_CreateSemaphore(0);
	sDoneSem=SDL_CreateSemaphore(0);
	if(!sStartSem||!sDoneSem)
	{
		fprintf(stderr,"[jobs] Could not create semaphores; rendering single-threaded\n");
		return;
	}
	for(i=0;i<count;i++)
	{
		sWorkers[i]=SDL_CreateThread(WorkerMain,"RenderWorker",NULL);
		if(!sWorkers[i])
			break;
		sWorkerCount++;
	}
}

void Jobs_Run(tJobFunc func,void *context,int count)
{
	int i;
	if(sWorkerCount<0)
		StartWorkers();
	sFunc=func;
	sContext=context;
	sCount=count;
	SDL_AtomicSet(&sNextIndex,0);
	for(i=0;i<sWorkerCount;i++)
		SDL_SemPost(sStartSem);
	RunPendingJobs();
	for(i=0;i<sWorkerCount;i++)
		SDL_SemWait(sDoneSem);
}

void Jobs_Shutdown(void)
{
	int i;
	if(sWorkerCount<=0)
		return;
	sQuit=1;
	for(i=0;i<sWorkerCount;i++)
		SDL_SemPost(sStartSem);
	for(i=0;i<sWorkerCount;i++)
		SDL_WaitThread(sWorkers[i],NULL);
	SDL_DestroySemaphore(sStartSem);
	SDL_DestroySemaphore(sDoneSem);
	sWorkerCount=-1;
	sQuit=0;
}

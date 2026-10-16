#ifndef __JOBS_H
#define __JOBS_H

/* Runs func(context,index) for index 0..count-1 spread over worker threads
   and the calling thread; returns when all calls are done. The functions must
   only touch data that no other index writes. */
typedef void (*tJobFunc)(void *context,int index);

void Jobs_Run(tJobFunc func,void *context,int count);
void Jobs_Shutdown(void);

#endif

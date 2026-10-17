#include "compat.h"
#include "route.h"
#include <math.h>
#include <string.h>
#include "roads.h"
#include "objects.h"
#include "packs.h"
#include "preferences.h"
#include "screen.h"
#include "error.h"
#include "platform.h"
#include "hqassets.h"
#include "jobs.h"

/* Swap a 16-bit pixel from big-endian pack data to native endian */
#ifdef __BIG_ENDIAN__
  #define PIXEL16(x) (x)
#else
  #define PIXEL16(x) SWAP16(x)
#endif

/*
Ptr DrawLine(Ptr drawPos,int xDrawStart,int x1,int x2,int y,int xDrift,int yDrift,Ptr data)
{
	int u,v;
	if(x1<xDrawStart)x1=xDrawStart;
	if(x2>xDrawStart+gXSize)x2=xDrawStart+gXSize;
	if(x2<x1) return drawPos;
	v=((-y+yDrift&0x007f)<<7);
	u=x1+xDrift&0x007f;
	x2-=x1;	
	data+=v;
	while(u&0x0003)
		if(x2)
		{
			*(drawPos++)=data[u++&0x007f];
			x2--;
		}
		else return drawPos;	
	while(x2>=sizeof(long))
	{
		*((long*)drawPos)=*(long*)(data+(u&0x007f));	
		drawPos+=sizeof(long);
		u+=sizeof(long);		
		x2-=sizeof(long);
	}
	if(x2>=sizeof(short))
	{
		*((short*)drawPos)=*(short*)(data+(u&0x007f));
		drawPos+=sizeof(short);
		u+=sizeof(short);		
		x2-=sizeof(short);
	}
	if(x2)
		*(drawPos++)=data[(u&0x007f)];
	return drawPos;
}

inline Ptr DrawBorder(Ptr drawPos,int xDrawStart,int x1,int x2,int y,Ptr data)
{
	int v=(-y&0x007f)<<4;
	int u=0;
	if(x2<xDrawStart) return drawPos;
	if(x1<xDrawStart){
		u=xDrawStart-x1;
		x1=xDrawStart;
	}
	if(x2>xDrawStart+gXSize)x2=xDrawStart+gXSize;
	if(x2<x1) return drawPos;
	data+=v;
	x2-=x1;	
	while(x2>=sizeof(long))
	{
		*((long*)drawPos)=*(long*)(data+u);	
		drawPos+=sizeof(long);
		u+=sizeof(long);		
		x2-=sizeof(long);
	}
	if(x2>=sizeof(short))
	{
		*((short*)drawPos)=*(short*)(data+u);
		drawPos+=sizeof(short);
		u+=sizeof(short);		
		x2-=sizeof(short);
	}
	if(x2)
		*(drawPos++)=data[u];
	return drawPos;
}

Ptr DrawBorderLine(Ptr drawPos,int xDrawStart,int x1,int x2,int y,Ptr data,Ptr leftBorder,Ptr rightBorder)
{
	int leftBordEnd=x1+16;
	int rightBordEnd=x2-16;
	if(leftBordEnd>rightBordEnd)
	{
		leftBordEnd=x1+((x2-x1)>>1);
		rightBordEnd=leftBordEnd;
	}
	drawPos=DrawBorder(drawPos,xDrawStart,x1,leftBordEnd,y,leftBorder);
	drawPos=DrawLine(drawPos,xDrawStart,leftBordEnd,rightBordEnd,y,gXDriftPos,gYDriftPos,data);
	return DrawBorder(drawPos,xDrawStart,rightBordEnd,x2,y,rightBorder);
}

void DrawRoad(int xDrawStart,int yDrawStart)
{
	int y;
	int rowBytesSkip=gRowBytes-gXSize;
	Ptr drawPos=gBaseAddr;	
	Ptr backgrTex=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).backgroundTex);
	Ptr roadTex=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).foregroundTex);
	Ptr leftBorder=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).roadLeftBorder);
	Ptr rightBorder=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).roadRightBorder);
	tRoad roadData=gRoadData+yDrawStart/2;
	if(gPrefs.lineSkip)
		if(!gOddLines) for(y=yDrawStart;y>yDrawStart-gYSize+(gCurPlayer->finishDelay?0:kInvLines);y--)
		{
			drawPos=DrawBorderLine(drawPos,xDrawStart,0x80000000,(*roadData)[0],y,backgrTex,leftBorder,rightBorder);
			drawPos=DrawLine(drawPos,xDrawStart,(*roadData)[0],(*roadData)[1],y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
			drawPos=DrawBorderLine(drawPos,xDrawStart,(*roadData)[1],(*roadData)[2],y,backgrTex,leftBorder,rightBorder);
			drawPos=DrawLine(drawPos,xDrawStart,(*roadData)[2],(*roadData)[3],y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
			drawPos=DrawBorderLine(drawPos,xDrawStart,(*roadData)[3],0x7fffffff,y,backgrTex,leftBorder,rightBorder);
			y--;
			drawPos+=gRowBytes+rowBytesSkip;
			roadData--;
		}
		else for(y=yDrawStart;y>yDrawStart-gYSize+(gCurPlayer->finishDelay?0:kInvLines);y--)
		{
			drawPos+=gRowBytes+rowBytesSkip;
			y--;
			drawPos=DrawBorderLine(drawPos,xDrawStart,0x80000000,((*roadData)[0]+(*(roadData-1))[0])>>1,y,backgrTex,leftBorder,rightBorder);
			drawPos=DrawLine(drawPos,xDrawStart,((*roadData)[0]+(*(roadData-1))[0])>>1,((*roadData)[1]+(*(roadData-1))[1])>>1,y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
			drawPos=DrawBorderLine(drawPos,xDrawStart,((*roadData)[1]+(*(roadData-1))[1])>>1,((*roadData)[2]+(*(roadData-1))[2])>>1,y,backgrTex,leftBorder,rightBorder);
			drawPos=DrawLine(drawPos,xDrawStart,((*roadData)[2]+(*(roadData-1))[2])>>1,((*roadData)[3]+(*(roadData-1))[3])>>1,y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
			drawPos=DrawBorderLine(drawPos,xDrawStart,((*roadData)[3]+(*(roadData-1))[3])>>1,0x7fffffff,y,backgrTex,leftBorder,rightBorder);
			roadData--;
		}
	else for(y=yDrawStart;y>yDrawStart-gYSize+(gCurPlayer->finishDelay?0:kInvLines);y--)
	{
		drawPos=DrawBorderLine(drawPos,xDrawStart,0x80000000,(*roadData)[0],y,backgrTex,leftBorder,rightBorder);
		drawPos=DrawLine(drawPos,xDrawStart,(*roadData)[0],(*roadData)[1],y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
		drawPos=DrawBorderLine(drawPos,xDrawStart,(*roadData)[1],(*roadData)[2],y,backgrTex,leftBorder,rightBorder);
		drawPos=DrawLine(drawPos,xDrawStart,(*roadData)[2],(*roadData)[3],y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
		drawPos=DrawBorderLine(drawPos,xDrawStart,(*roadData)[3],0x7fffffff,y,backgrTex,leftBorder,rightBorder);
		drawPos+=rowBytesSkip;
		y--;
		drawPos=DrawBorderLine(drawPos,xDrawStart,0x80000000,((*roadData)[0]+(*(roadData-1))[0])>>1,y,backgrTex,leftBorder,rightBorder);
		drawPos=DrawLine(drawPos,xDrawStart,((*roadData)[0]+(*(roadData-1))[0])>>1,((*roadData)[1]+(*(roadData-1))[1])>>1,y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
		drawPos=DrawBorderLine(drawPos,xDrawStart,((*roadData)[1]+(*(roadData-1))[1])>>1,((*roadData)[2]+(*(roadData-1))[2])>>1,y,backgrTex,leftBorder,rightBorder);
		drawPos=DrawLine(drawPos,xDrawStart,((*roadData)[2]+(*(roadData-1))[2])>>1,((*roadData)[3]+(*(roadData-1))[3])>>1,y,gXFrontDriftPos,gYFrontDriftPos,roadTex);
		drawPos=DrawBorderLine(drawPos,xDrawStart,((*roadData)[3]+(*(roadData-1))[3])>>1,0x7fffffff,y,backgrTex,leftBorder,rightBorder);
		drawPos+=rowBytesSkip;
		roadData--;
	}
}
*/

Ptr DrawLineZoomed(Ptr drawPos,int xDrawStart,int x1,int x2,int y,int xDrift,int yDrift,Ptr data,float zoom)
{
	UInt32 u,v;
	UInt32 dudx;
	if(x1<0)x1=0;
	if(x2>gXSize)x2=gXSize;
	if(x2<x1) return drawPos;
	v=(-y+yDrift&0x007f)<<7;
	u=(int)((x1*zoom+xDrawStart+xDrift)*256.0f)&0x7fff;
	x2-=x1;
	data+=v;
	dudx=(int)(zoom*256.0f+0.5f);	
	while(x2)
	{
		*(drawPos++)=data[((u>>8)&0x007f)];
		u+=dudx;
		x2--;
	}
	return drawPos;
}

Ptr DrawBorderZoomed(Ptr drawPos,int xDrawStart,int x1,int x2,int y,Ptr data,float zoom)
{
	UInt32 dudx;
	UInt32 v=(-y&0x007f)<<4;
	UInt32 u=0;
	if(x2<0) return drawPos;
	if(x1<0){
		u=-x1<<8;
		x1=0;
	}
	if(x2>gXSize)x2=gXSize;
	if(x2<x1) return drawPos;
	x2-=x1;	
	data+=v;
	dudx=(int)(zoom*256.0f+0.5f);	
	while(x2)
	{
		*(drawPos++)=data[u>>8];
		u+=dudx;
		x2--;
	}
	return drawPos;
}

Ptr DrawBorderLineZoomed(Ptr drawPos,int xDrawStart,int x1,int x2,int y,Ptr data,Ptr leftBorder,Ptr rightBorder,float zoom)
{
	int leftBordEnd=x1+(int)(16/zoom);
	int rightBordEnd=x2-(int)(16/zoom);
	if(leftBordEnd>rightBordEnd)
	{
		leftBordEnd=x1+((x2-x1)>>1);
		rightBordEnd=leftBordEnd;
	}
	drawPos=DrawBorderZoomed(drawPos,xDrawStart,x1,leftBordEnd,y,leftBorder,zoom);
	drawPos=DrawLineZoomed(drawPos,xDrawStart,leftBordEnd,rightBordEnd,y,gXDriftPos,gYDriftPos,data,zoom);
	return DrawBorderZoomed(drawPos,xDrawStart,rightBordEnd,x2,y,rightBorder,zoom);
}

float InZeroMax(float x,float max)
{
	if(x<0)return 0;
	else if(x>max) return max;
	else return x;
}

void DrawRoadZoomed(float xDrawStart,float yDrawStart,float zoom)
{
	float invZoom=1/zoom;
	int screenY;
	int rowBytesSkip=gRowBytes-gXSize;
	Ptr drawPos=gBaseAddr;	
	Ptr backgrTex=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).backgroundTex,0);
	Ptr roadTex=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).foregroundTex,0);
	Ptr leftBorder=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).roadLeftBorder,0);
	Ptr rightBorder=GetUnsortedPackEntry(kPackTxtR,(*gRoadInfo).roadRightBorder,0);
	if(gPrefs.lineSkip)
		rowBytesSkip+=gRowBytes;
	for(screenY=0;screenY<gYSize;screenY+=(gPrefs.lineSkip?2:1))
	{
		float worldY=InZeroMax(yDrawStart-screenY*zoom,*gRoadLenght*2);
		float ceilRoadLine=ceil(worldY*0.5);
		float floorRoadLine=floor(worldY*0.5);
		float floorPerc=ceilRoadLine-worldY*0.5;
		int roadData[4]; /* SInt32: storing an out-of-range clip position into
		                    an SInt16 is UB and disorders the spans, overrunning
		                    the framebuffer; only the on-disk road data is SInt16 */
		tRoad ceilRoad=(gRoadData+(int)(ceilRoadLine));
		tRoad floorRoad=(gRoadData+(int)(floorRoadLine));
		int ceilSplit=(*ceilRoad)[1]!=(*ceilRoad)[2];
		int floorSplit=(*floorRoad)[1]!=(*floorRoad)[2];
		if(ceilSplit!=floorSplit)
			if(ceilSplit)floorPerc=1;
			else floorPerc=0;
		else if(ceilSplit&&floorSplit)
			if((*ceilRoad)[3]<(*floorRoad)[2]||(*ceilRoad)[2]>(*floorRoad)[3])
				floorPerc=1;
		roadData[0]=((floorPerc*(*floorRoad)[0]+(1-floorPerc)*(*ceilRoad)[0])-xDrawStart)*invZoom;
		roadData[1]=((floorPerc*(*floorRoad)[1]+(1-floorPerc)*(*ceilRoad)[1])-xDrawStart)*invZoom;
		roadData[2]=((floorPerc*(*floorRoad)[2]+(1-floorPerc)*(*ceilRoad)[2])-xDrawStart)*invZoom;
		roadData[3]=((floorPerc*(*floorRoad)[3]+(1-floorPerc)*(*ceilRoad)[3])-xDrawStart)*invZoom;
		drawPos=DrawBorderLineZoomed(drawPos,xDrawStart,0x80000000,roadData[0],worldY,backgrTex,leftBorder,rightBorder,zoom);
		drawPos=DrawLineZoomed(drawPos,xDrawStart,roadData[0],roadData[1],worldY,0,0,roadTex,zoom);
		drawPos=DrawBorderLineZoomed(drawPos,xDrawStart,roadData[1],roadData[2],worldY,backgrTex,leftBorder,rightBorder,zoom);
		drawPos=DrawLineZoomed(drawPos,xDrawStart,roadData[2],roadData[3],worldY,0,0,roadTex,zoom);
		drawPos=DrawBorderLineZoomed(drawPos,xDrawStart,roadData[3],0x7fffffff,worldY,backgrTex,leftBorder,rightBorder,zoom);		
		drawPos+=rowBytesSkip;
	}
	if(drawPos!=gBaseAddr+gXSize*(gPrefs.lineSkip?gYSize/2:gYSize)+
		(gPrefs.lineSkip?gYSize/2:gYSize)*rowBytesSkip)DoError(-50);
}


static void GetRoadMipLevels(float zoom,int *level0,int *level1,int *blend)
{
	float lod=log2f(zoom*4.0f);
	if(lod<0)lod=0;
	if(lod>3)lod=3;
	*level0=(int)lod;
	*level1=*level0<3?*level0+1:*level0;
	*blend=(int)((lod-*level0)*256.0f+0.5f);
	if(*level0==*level1)
		*blend=0;
}

/* Mip levels for one span; resolved once per span instead of per pixel. */
typedef struct {
	const tHQImage *mip[2];
	int width[2],height[2],level[2];
	int blend;
	int textureWidth;
} tRoadSampler;

static int InitRoadSampler(tRoadSampler *sampler,const tHQImage *hq,
	int textureWidth,float zoom)
{
	int i;
	if(!hq||hq->width!=textureWidth||hq->height!=128||hq->scale!=4)
		return 0;
	GetRoadMipLevels(zoom,&sampler->level[0],&sampler->level[1],&sampler->blend);
	for(i=0;i<2;i++)
	{
		/* Read-only lookup: render workers must not build mips themselves. */
		int level=sampler->level[i];
		sampler->mip[i]=level>0&&hq->mipLevels[level-1]?hq->mipLevels[level-1]:hq;
		if(sampler->mip[i]==hq)
			sampler->level[i]=0;
		sampler->width[i]=sampler->mip[i]->width*sampler->mip[i]->scale;
		sampler->height[i]=sampler->mip[i]->height*sampler->mip[i]->scale;
	}
	/* A missing coarser level falls back to the finer one, not to the base. */
	if(sampler->mip[1]==hq&&sampler->mip[0]!=hq)
	{
		sampler->mip[1]=sampler->mip[0];
		sampler->width[1]=sampler->width[0];
		sampler->height[1]=sampler->height[0];
		sampler->level[1]=sampler->level[0];
	}
	if(sampler->level[0]==sampler->level[1])
		sampler->blend=0;
	sampler->textureWidth=textureWidth;
	return 1;
}

/* Bilinear sample of one mip level; channels come back in 8.8 fixed point. */
static inline void SampleRoadMip(const tRoadSampler *sampler,int index,
	int u,int v,int rgb[3])
{
	int width=sampler->width[index],height=sampler->height[index];
	int level=sampler->level[index];
	int uu=((u&((sampler->textureWidth<<8)-1))*4)>>level;
	int vv=((v&((128<<8)-1))*4)>>level;
	int x0=(uu>>8)&(width-1),y0=(vv>>8)&(height-1);
	int x1=(x0+1)&(width-1),y1=(y0+1)&(height-1);
	int fx=uu&255,fy=vv&255;
	const UInt8 *rgba=sampler->mip[index]->rgba;
	const UInt8 *p00=rgba+((size_t)y0*width+x0)*4;
	const UInt8 *p10=rgba+((size_t)y0*width+x1)*4;
	const UInt8 *p01=rgba+((size_t)y1*width+x0)*4;
	const UInt8 *p11=rgba+((size_t)y1*width+x1)*4;
	int channel;
	for(channel=0;channel<3;channel++)
	{
		int top=p00[channel]*(256-fx)+p10[channel]*fx;
		int bottom=p01[channel]*(256-fx)+p11[channel]*fx;
		rgb[channel]=(top*(256-fy)+bottom*fy)>>8;
	}
}

static inline tPixel SampleRoadHQ(const tRoadSampler *sampler,int u,int v)
{
	int rgb[3],channel;
	SampleRoadMip(sampler,0,u,v,rgb);
	if(sampler->blend)
	{
		int rgb1[3];
		SampleRoadMip(sampler,1,u,v,rgb1);
		for(channel=0;channel<3;channel++)
			rgb[channel]=(rgb[channel]*(256-sampler->blend)+
				rgb1[channel]*sampler->blend)>>8;
	}
	return PixelRGB((rgb[0]+128)>>8,(rgb[1]+128)>>8,(rgb[2]+128)>>8);
}

static tPixel SampleRoadTexture(UInt16 *data,int u,int v,int textureWidth,
	int smooth)
{
	int fx,fy;
	int x0,y0,x1,y1;
	int xMask=textureWidth-1;
	tPixel c00,c10,c01,c11;
	if(!smooth)
	{
		x0=(u>>8)&xMask;
		y0=(v>>8)&127;
		return Pixel555(PIXEL16(data[y0*textureWidth+x0]));
	}
	u&=(textureWidth<<8)-1;
	v&=(128<<8)-1;
	x0=u>>8;
	y0=v>>8;
	x1=(x0+1)&xMask;
	y1=(y0+1)&127;
	fx=u&255;
	fy=v&255;
	c00=Pixel555(PIXEL16(data[y0*textureWidth+x0]));
	c10=Pixel555(PIXEL16(data[y0*textureWidth+x1]));
	c01=Pixel555(PIXEL16(data[y1*textureWidth+x0]));
	c11=Pixel555(PIXEL16(data[y1*textureWidth+x1]));
	{
		int channel,out[3];
		for(channel=0;channel<3;channel++)
		{
			int shift=16-channel*8;
			int top=((c00>>shift)&255)*(256-fx)+((c10>>shift)&255)*fx;
			int bottom=((c01>>shift)&255)*(256-fx)+((c11>>shift)&255)*fx;
			out[channel]=(top*(256-fy)+bottom*fy+32768)>>16;
		}
		return PixelRGB(out[0],out[1],out[2]);
	}
}

static tPixel *FillRoadSpan16(tPixel *drawPos,int count,int u,int v,
	int dudx,UInt16 *data,int textureWidth,float zoom,const tHQImage *hq)
{
	tRoadSampler sampler;
	if(InitRoadSampler(&sampler,hq,textureWidth,zoom))
	{
		while(count--)
		{
			*(drawPos++)=SampleRoadHQ(&sampler,u,v);
			u+=dudx;
		}
	}
	else
	{
		int smooth=Platform_GetRenderScale()>1;
		while(count--)
		{
			*(drawPos++)=SampleRoadTexture(data,u,v,textureWidth,smooth);
			u+=dudx;
		}
	}
	return drawPos;
}

static Ptr DrawLineZoomed16(tPixel *drawPos,float xDrawStart,int x1,int x2,float y,
	int xDrift,int yDrift,UInt16 *data,float zoom,const tHQImage *hq)
{
	int u,v,dudx;
	int smooth=Platform_GetRenderScale()>1;
	if(x1<0)x1=0;
	if(x2>gXSize)x2=gXSize;
	if(x2<x1) return (Ptr)drawPos;
	v=smooth?(int)((-y+yDrift)*256.0f):(((int)(-y+yDrift)&127)<<8);
	u=(int)((x1*zoom+xDrawStart+xDrift)*256.0f);
	dudx=(int)(zoom*256.0f+0.5f);
	return (Ptr)FillRoadSpan16(drawPos,x2-x1,u,v,dudx,data,128,zoom,hq);
}

/* edge: exact screen x where the border texture starts (u=0). */
static Ptr DrawBorderZoomed16(tPixel *drawPos,int x1,int x2,float edge,float y,
	UInt16 *data,float zoom,const tHQImage *hq)
{
	int u;
	int smooth=Platform_GetRenderScale()>1;
	int v=smooth?(int)(-y*256.0f):(((int)-y&127)<<8);
	int dudx=(int)(zoom*256.0f+0.5f);
	if(x2<0) return (Ptr)drawPos;
	if(x1<0)x1=0;
	if(x2>gXSize)x2=gXSize;
	if(x2<x1) return (Ptr)drawPos;
	/* Phase from the sub-pixel edge instead of the rounded pixel, so the
	   road borders glide instead of crawling as the road moves. */
	u=(int)((x1-edge)*zoom*256.0f);
	if(u<0)u=0;
	return (Ptr)FillRoadSpan16(drawPos,x2-x1,u,v,dudx,data,16,zoom,hq);
}

/* Everything one road row needs; shared read-only by the render workers. */
typedef struct {
	float xDrawStart,yDrawStart,zoom,invZoom;
	Ptr backgrTex,roadTex,leftBorder,rightBorder;
	const tHQImage *backgroundHQ,*roadHQ,*leftBorderHQ,*rightBorderHQ;
	int rowStep,bandRows;
	SDL_atomic_t spanErrors;
} tRoadFrame;

static Ptr DrawBorderLineZoomed16(Ptr drawPos,const tRoadFrame *frame,
	int x1,int x2,float x1Exact,float x2Exact,float y)
{
	float zoom=frame->zoom;
	int leftBordEnd=x1+(int)(16/zoom);
	int rightBordEnd=x2-(int)(16/zoom);
	float rightEdge=x2Exact-16/zoom;
	if(leftBordEnd>rightBordEnd)
	{
		leftBordEnd=x1+((x2-x1)>>1);
		rightBordEnd=leftBordEnd;
		rightEdge=rightBordEnd;
	}
	drawPos=DrawBorderZoomed16((tPixel*)drawPos,x1,leftBordEnd,x1Exact,
		y,(UInt16*)frame->leftBorder,zoom,frame->leftBorderHQ);
	drawPos=DrawLineZoomed16((tPixel*)drawPos,frame->xDrawStart,leftBordEnd,
		rightBordEnd,y,gXDriftPos,gYDriftPos,(UInt16*)frame->backgrTex,zoom,
		frame->backgroundHQ);
	drawPos=DrawBorderZoomed16((tPixel*)drawPos,rightBordEnd,x2,rightEdge,
		y,(UInt16*)frame->rightBorder,zoom,frame->rightBorderHQ);
	return drawPos;
}

/* Free-form road (route.c): a row is any number of road pieces with grass
   and borders between them. */
static void DrawRouteRow16(const tRoadFrame *frame,int screenY)
{
	float xDrawStart=frame->xDrawStart,zoom=frame->zoom,invZoom=frame->invZoom;
	Ptr rowStart=gBaseAddr+screenY*gRowBytes;
	Ptr drawPos=rowStart;
	float worldY=frame->yDrawStart-screenY*zoom;
	const SInt16 *spans;
	int count=Route_RowSpans((int)(worldY*0.5f),0,&spans),k;
	int previous=0x80000000;
	float previousExact=-2147483648.0f;
	for(k=0;k<count;k++)
	{
		float leftExact=(spans[2*k]-xDrawStart)*invZoom;
		float rightExact=(spans[2*k+1]-xDrawStart)*invZoom;
		int left=(int)leftExact,right=(int)rightExact;
		if(left>=gXSize)
			break;
		if(right<=0)
		{
			previous=right;
			previousExact=rightExact;
			continue;
		}
		drawPos=DrawBorderLineZoomed16(drawPos,frame,previous,left,previousExact,leftExact,worldY);
		drawPos=DrawLineZoomed16((tPixel*)drawPos,xDrawStart,left,right,worldY,0,0,
			(UInt16*)frame->roadTex,zoom,frame->roadHQ);
		previous=right;
		previousExact=rightExact;
	}
	drawPos=DrawBorderLineZoomed16(drawPos,frame,previous,0x7fffffff,previousExact,
		2147483647.0f,worldY);
	if(drawPos!=rowStart+gXSize*(int)sizeof(tPixel))
		SDL_AtomicAdd((SDL_atomic_t*)&frame->spanErrors,1);
}

/* The bridges' road surface, drawn after the cars below them. */
void DrawRouteDeck16(float xDrawStart,float yDrawStart,float zoom)
{
	UInt16 *roadTex=(UInt16*)GetUnsortedPackEntry(kPackTx16,(*gRoadInfo).foregroundTex,0);
	const tHQImage *roadHQ=HQAssets_Load("textures",(*gRoadInfo).foregroundTex,0);
	int screenY;
	for(screenY=0;screenY<gYSize;screenY++)
	{
		float worldY=yDrawStart-screenY*zoom;
		const SInt16 *spans;
		int count=Route_RowSpans((int)(worldY*0.5f),1,&spans),k;
		for(k=0;k<count;k++)
		{
			int left=(int)((spans[2*k]-xDrawStart)/zoom),right=(int)((spans[2*k+1]-xDrawStart)/zoom);
			if(right<=0||left>=gXSize)
				continue;
			DrawLineZoomed16((tPixel*)(gBaseAddr+screenY*gRowBytes)+(left<0?0:left),xDrawStart,
				left,right,worldY,0,0,roadTex,zoom,roadHQ);
		}
	}
}

static void DrawRoadRow16(const tRoadFrame *frame,int screenY)
{
	if(gRouteActive)
	{
		DrawRouteRow16(frame,screenY);
		return;
	}
	float xDrawStart=frame->xDrawStart,zoom=frame->zoom,invZoom=frame->invZoom;
	Ptr rowStart=gBaseAddr+screenY*gRowBytes;
	Ptr drawPos=rowStart;
	float worldY=InZeroMax(frame->yDrawStart-screenY*zoom,*gRoadLenght*2);
	float ceilRoadLine=ceil(worldY*0.5);
	float floorRoadLine=floor(worldY*0.5);
	float floorPerc=ceilRoadLine-worldY*0.5;
	int roadData[4]; /* SInt32: storing an out-of-range clip position into
	                    an SInt16 is UB and disorders the spans, overrunning
	                    the framebuffer; only the on-disk road data is SInt16 */
	float edge[4];
	tRoad ceilRoad=(gRoadData+(int)(ceilRoadLine));
	tRoad floorRoad=(gRoadData+(int)(floorRoadLine));
	int ceilSplit=(*ceilRoad)[1]!=(*ceilRoad)[2];
	int floorSplit=(*floorRoad)[1]!=(*floorRoad)[2];
	if(ceilSplit!=floorSplit)
		if(ceilSplit)floorPerc=1;
		else floorPerc=0;
	else if(ceilSplit&&floorSplit)
		if((*ceilRoad)[3]<(*floorRoad)[2]||(*ceilRoad)[2]>(*floorRoad)[3])
			floorPerc=1;
	{
		int i;
		for(i=0;i<4;i++)
		{
			edge[i]=((floorPerc*(*floorRoad)[i]+(1-floorPerc)*(*ceilRoad)[i])-xDrawStart)*invZoom;
			roadData[i]=edge[i];
		}
	}
	drawPos=DrawBorderLineZoomed16(drawPos,frame,0x80000000,roadData[0],
		-2147483648.0f,edge[0],worldY);
	drawPos=DrawLineZoomed16((tPixel*)drawPos,xDrawStart,roadData[0],
		roadData[1],worldY,0,0,(UInt16*)frame->roadTex,zoom,frame->roadHQ);
	drawPos=DrawBorderLineZoomed16(drawPos,frame,roadData[1],roadData[2],
		edge[1],edge[2],worldY);
	drawPos=DrawLineZoomed16((tPixel*)drawPos,xDrawStart,roadData[2],
		roadData[3],worldY,0,0,(UInt16*)frame->roadTex,zoom,frame->roadHQ);
	drawPos=DrawBorderLineZoomed16(drawPos,frame,roadData[3],0x7fffffff,
		edge[3],2147483647.0f,worldY);
	if(drawPos!=rowStart+gXSize*(int)sizeof(tPixel))
		SDL_AtomicAdd((SDL_atomic_t*)&frame->spanErrors,1);
}

static void DrawRoadBand16(void *context,int band)
{
	const tRoadFrame *frame=(const tRoadFrame*)context;
	int screenY=band*frame->bandRows;
	int endY=screenY+frame->bandRows;
	if(endY>gYSize)
		endY=gYSize;
	for(;screenY<endY;screenY+=frame->rowStep)
		DrawRoadRow16(frame,screenY);
}

static void PrepareRoadMips(const tHQImage *hq)
{
	/* Mip levels are built lazily; do it here, not racing in the workers. */
	if(hq)
		HQAssets_GetMipLevel(hq,3);
}

void DrawRoadZoomed16(float xDrawStart,float yDrawStart,float zoom)
{
	tRoadFrame frame;
	int backgroundID=(*gRoadInfo).backgroundTex;
	int foregroundID=(*gRoadInfo).foregroundTex;
	int leftBorderID=(*gRoadInfo).roadLeftBorder;
	int rightBorderID=(*gRoadInfo).roadRightBorder;
	int bands;
	frame.xDrawStart=xDrawStart;
	frame.yDrawStart=yDrawStart;
	frame.zoom=zoom;
	frame.invZoom=1/zoom;
	frame.backgrTex=GetUnsortedPackEntry(kPackTx16,backgroundID,0);
	frame.roadTex=GetUnsortedPackEntry(kPackTx16,foregroundID,0);
	frame.leftBorder=GetUnsortedPackEntry(kPackTx16,leftBorderID,0);
	frame.rightBorder=GetUnsortedPackEntry(kPackTx16,rightBorderID,0);
	frame.backgroundHQ=HQAssets_Load("textures",backgroundID,0);
	frame.roadHQ=HQAssets_Load("textures",foregroundID,0);
	frame.leftBorderHQ=HQAssets_Load("textures",leftBorderID,0);
	frame.rightBorderHQ=HQAssets_Load("textures",rightBorderID,0);
	PrepareRoadMips(frame.backgroundHQ);
	PrepareRoadMips(frame.roadHQ);
	PrepareRoadMips(frame.leftBorderHQ);
	PrepareRoadMips(frame.rightBorderHQ);
	frame.rowStep=gPrefs.lineSkip?2:1;
	/* Small bands (a multiple of the row step) keep all cores busy. */
	frame.bandRows=16;
	SDL_AtomicSet(&frame.spanErrors,0);
	bands=(gYSize+frame.bandRows-1)/frame.bandRows;
	Jobs_Run(DrawRoadBand16,&frame,bands);
	if(SDL_AtomicGet(&frame.spanErrors))
		DoError(-50);
}

#include "compat.h"
#include <math.h>
#include "objects.h"
#include "screen.h"
#include "error.h"
#include "trig.h"
#include "packs.h"
#include "preferences.h"
#include "random.h"
#include "platform.h"
#include <stdio.h>
#include "assetfile.h"

#define kNumSprites			300
#define kNumSpecialSprites	100
#define kLifeBarId			318

enum{
	kDrawModeMaskColor=0,
	kDrawModeTransparent=1<<0,
	kDrawModeDeepMask=1<<1,
	kDrawModeDoubleSize=1<<2
};

typedef struct{
	UInt16 xSize,ySize;
	UInt8 log2xSize;
	UInt8 filler1;
	UInt8 drawMode;
	UInt8 filler2;
	UInt8 data[1];
} tSpriteHeader;

typedef struct{
	UInt16 xSize,ySize;
	UInt8 log2xSize;
	UInt8 filler1;
	UInt8 drawMode;
	UInt8 filler2;
	UInt16 data[1];
} tSpriteHeader16;

/* AI sprites in the 32-bit format: texels with alpha, 0 = fully clear. */
typedef struct{
	UInt16 xSize,ySize;
	UInt8 log2xSize;
	UInt8 filler1;
	UInt8 drawMode;
	UInt8 filler2;
	tPixel data[1];
} tSpriteHeader32;

/* gSpriteAIDetail values */
enum{kAISprite16=1,kAISprite32=2};

typedef struct{
	int x1,x2,u,v;
} tSlopeInfo;

typedef struct{
	float x,y;
	int u,v;
} tVertex;

tSlopeInfo gSlope[kMaxScreenY];
Handle gSprites[kNumSprites+kNumSpecialSprites];
static UInt8 gSpriteAIDetail[kNumSprites+kNumSpecialSprites];
/* AI sprites at half resolution, used when the camera zooms out so the 4x
   art is not point-sampled at 2:1 (rotating cars shimmered). */
static Handle gHalfSprites[kNumSprites];

static void BuildHalfSprite32(int index)
{
	tSpriteHeader32 *source=(tSpriteHeader32*)*gSprites[index];
	int width=source->xSize/2,height=source->ySize/2,x,y;
	tSpriteHeader32 *half;
	if(width<2||height<2)
		return;
	gHalfSprites[index]=NewHandle(8+(long)width*height*4);
	if(!gHalfSprites[index]||!*gHalfSprites[index])
	{
		gHalfSprites[index]=nil;
		return;
	}
	half=(tSpriteHeader32*)*gHalfSprites[index];
	*half=*source;
	half->xSize=width;
	half->ySize=height;
	half->log2xSize=source->log2xSize-1;
	for(y=0;y<height;y++)
		for(x=0;x<width;x++)
		{
			/* Alpha-weighted 2x2 average, so clear texels add no color. */
			const tPixel *row=source->data+(2*y)*source->xSize+2*x;
			tPixel texels[4]={row[0],row[1],row[source->xSize],row[source->xSize+1]};
			UInt32 alphaSum=0,red=0,green=0,blue=0;
			int i;
			for(i=0;i<4;i++)
			{
				UInt32 alpha=texels[i]>>24;
				alphaSum+=alpha;
				red+=((texels[i]>>16)&255)*alpha;
				green+=((texels[i]>>8)&255)*alpha;
				blue+=(texels[i]&255)*alpha;
			}
			half->data[y*width+x]=alphaSum?
				(((alphaSum+2)/4)<<24)|((red/alphaSum)<<16)|((green/alphaSum)<<8)|(blue/alphaSum):0;
		}
}

static void BuildHalfSprite(int index)
{
	tSpriteHeader16 *source=(tSpriteHeader16*)*gSprites[index];
	if(gSpriteAIDetail[index]==kAISprite32)
	{
		BuildHalfSprite32(index);
		return;
	}
	int width=source->xSize/2,height=source->ySize/2,x,y;
	UInt16 mask=source->data[0];
	tSpriteHeader16 *half;
	if(width<2||height<2)
		return;
	gHalfSprites[index]=NewHandle(8+(long)width*height*2);
	if(!gHalfSprites[index]||!*gHalfSprites[index])
	{
		gHalfSprites[index]=nil;
		return;
	}
	half=(tSpriteHeader16*)*gHalfSprites[index];
	*half=*source;
	half->xSize=width;
	half->ySize=height;
	half->log2xSize=source->log2xSize-1;
	for(y=0;y<height;y++)
		for(x=0;x<width;x++)
		{
			/* Average the opaque texels of each 2x2 block; keep the block
			   opaque when at least half of it is. */
			const UInt16 *row=source->data+(2*y)*source->xSize+2*x;
			UInt16 texels[4]={row[0],row[1],row[source->xSize],row[source->xSize+1]};
			int i,count=0,red=0,green=0,blue=0;
			UInt16 color=mask;
			for(i=0;i<4;i++)
				if(texels[i]!=mask)
				{
					red+=(texels[i]>>10)&31;
					green+=(texels[i]>>5)&31;
					blue+=texels[i]&31;
					count++;
				}
			if(count>=2)
			{
				color=(UInt16)(((red/count)<<10)|((green/count)<<5)|(blue/count));
				if(color==mask)
					color^=1;
			}
			half->data[y*width+x]=color;
		}
	half->data[0]=mask; /* the draw routines read the mask from texel 0 */
}

/* Helper: Duplicate a Handle (replaces Mac HandToHand) */
static OSErr HandToHandCompat(Handle *h)
{
	if (!h || !*h) return -1;
	/* We need to know the size. We'll use a simple approach:
	   Since sprites are loaded by PtrToHand with known sizes,
	   we store the size after the pointer. For now, use a large copy. */
	/* Actually for sprite handles, we'll track sizes separately.
	   For this port, we store size in a simple side array. */
	return 0; /* handled below */
}

/* Sprite handle size tracking */
static long gSpriteHandleSizes[kNumSprites+kNumSpecialSprites];

static int AIDetailPathExists(const char *path)
{
	FILE *file=fopen(path,"rb");
	if(!file)
		return false;
	fclose(file);
	return true;
}

static int BuildAIDetailPath(char *path, size_t pathSize, const char *suffix)
{
	const char *overridePath=getenv("RECKLESS_AI_SPRITES");
	char *basePath;
	int written;
	if(overridePath&&overridePath[0])
	{
		written=snprintf(path,pathSize,"%s/%s",overridePath,suffix);
		return written>0&&(size_t)written<pathSize&&AIDetailPathExists(path);
	}
	basePath=SDL_GetBasePath();
	if(!basePath)
		return false;
	written=snprintf(path,pathSize,"%s/../Resources/AIAssets/sprites/%s",basePath,suffix);
	if(written<=0||(size_t)written>=pathSize)
	{
		SDL_free(basePath);
		return false;
	}
	if(AIDetailPathExists(path))
	{
		SDL_free(basePath);
		return true;
	}
	written=snprintf(path,pathSize,"%s/AIAssets/sprites/%s",basePath,suffix);
	if(written<=0||(size_t)written>=pathSize)
	{
		SDL_free(basePath);
		return false;
	}
	if(AIDetailPathExists(path))
	{
		SDL_free(basePath);
		return true;
	}
	written=snprintf(path,pathSize,"%s/../../../AIAssets/sprites/%s",basePath,suffix);
	SDL_free(basePath);
	return written>0&&(size_t)written<pathSize&&AIDetailPathExists(path);
}

int Sprites_HasAIDetailAssets(void)
{
	char path[1024];
	return BuildAIDetailPath(path,sizeof(path),"AI_SPRITES");
}

static void LoadAIDetailSprite(int id)
{
	char path[1024];
	char assetPath[1024];
	long fileSize;
	UInt8 *asset;
	tSpriteHeader *source;
	UInt16 width,height,assetWidth,assetHeight;
	int index=id-128;
	Handle enhanced;
	int directoryLength,format;
	/* Keep the animated end-of-game lettering on the original sprite path. */
	if(id==265||id==kLifeBarId||gPrefs.unused[6]!=1||!gPrefs.hiColor||
		!BuildAIDetailPath(path,sizeof(path),"AI_SPRITES"))
		return;
	directoryLength=(int)strlen(path)-(int)strlen("AI_SPRITES");
	if(directoryLength<1||
		snprintf(assetPath,sizeof(assetPath),"%.*s%d.spr",directoryLength,path,id)<=0)
		return;
	source=(tSpriteHeader*)*(gSprites[index]);
	width=source->xSize;
	height=source->ySize;
	{
		size_t assetSize;
		asset=AssetFile_Read(assetPath,&assetSize);
		if(!asset)
			return;
		fileSize=(long)assetSize;
	}
	/* 16-bit files: 2 bytes per texel; 32-bit files: RGBA, 4 bytes. */
	if((size_t)fileSize==8+(size_t)width*height*64)
		format=kAISprite32;
	else if((size_t)fileSize==8+(size_t)width*height*32)
		format=kAISprite16;
	else
	{
		fprintf(stderr,"[sprites] unexpected AI sprite size: %s\n",assetPath);
		free(asset);
		return;
	}
	assetWidth=(UInt16)(((UInt16)asset[0]<<8)|asset[1]);
	assetHeight=(UInt16)(((UInt16)asset[2]<<8)|asset[3]);
	if(assetWidth!=(UInt16)(width*4)||assetHeight!=(UInt16)(height*4))
	{
		fprintf(stderr,"[sprites] malformed AI sprite asset: %s\n",assetPath);
		free(asset);
		return;
	}
	enhanced=NewHandle(fileSize);
	if(!enhanced||!*enhanced)
	{
		fprintf(stderr,"[sprites] could not allocate AI sprite asset: %s\n",assetPath);
		free(asset);
		return;
	}
	memcpy(*enhanced,asset,(size_t)fileSize);
	free(asset);
	DisposeHandle(gSprites[index]); /* the 4x art replaces the original */
	gSprites[index]=enhanced;
	gSpriteHandleSizes[index]=fileSize;
	{
		tSpriteHeader *hdr=(tSpriteHeader*)*enhanced;
		hdr->xSize=SWAP16(hdr->xSize);
		hdr->ySize=SWAP16(hdr->ySize);
	}
	if(format==kAISprite32)
	{
		tPixel *texels=((tSpriteHeader32*)*enhanced)->data;
		size_t p,pixelCount=((size_t)fileSize-8)/4;
		for(p=0;p<pixelCount;p++)
		{
			const UInt8 *rgba=(const UInt8*)&texels[p];
			UInt32 alpha=rgba[3];
			texels[p]=alpha<8?0:
				(alpha<<24)|((UInt32)rgba[0]<<16)|((UInt32)rgba[1]<<8)|rgba[2];
		}
		gSpriteAIDetail[index]=kAISprite32;
		BuildHalfSprite(index);
		Platform_SetAIDetailSprite(id,width*4,height*4,texels);
	}
	else
	{
		tSpriteHeader16 *hdr=(tSpriteHeader16*)*enhanced;
		size_t p,pixelCount=((size_t)fileSize-8)/2;
		tPixel *argb;
#ifndef __BIG_ENDIAN__
		for(p=0;p<pixelCount;p++)
			hdr->data[p]=SWAP16(hdr->data[p]);
#endif
		gSpriteAIDetail[index]=kAISprite16;
		BuildHalfSprite(index);
		argb=(tPixel*)malloc(pixelCount*sizeof(tPixel));
		if(argb)
		{
			for(p=0;p<pixelCount;p++)
				argb[p]=hdr->data[p]==hdr->data[0]?0:Pixel555(hdr->data[p]);
			Platform_SetAIDetailSprite(id,width*4,height*4,argb);
			free(argb);
		}
	}
}

static Handle PtrToHandCompat(Ptr data, long size)
{
	Handle h = NewHandle(size);
	if (h && *h) {
		memcpy(*h, data, size);
	}
	return h;
}

static Handle DuplicateHandle(Handle src, int srcIdx)
{
	long size = gSpriteHandleSizes[srcIdx];
	if (size <= 0 || !src || !*src) return nil;
	Handle h = NewHandle(size);
	if (h && *h) {
		memcpy(*h, *src, size);
	}
	return h;
}

void SlopeInitUVLine(tVertex v1,tVertex v2,int dudx,int dvdx)
{
	int y;
	int	dxdy=((v2.x-v1.x)*65536.0)/(v2.y-v1.y);
	int dudy=((v2.u-v1.u)<<8 )/(v2.y-v1.y);
	int dvdy=((v2.v-v1.v)<<8 )/(v2.y-v1.y);
	int firsty,lasty;
	if(v1.y>=gYSize)
		return;
	if(v2.y<0)
		return;
	if(v1.y<0)
	{
		v1.x+=((-v1.y)*dxdy)/65536.0;
		v1.u+=((-v1.y)*dudy)/256.0;
		v1.v+=((-v1.y)*dvdy)/256.0;
		v1.y=0;
	}
	if(v2.y>=gYSize)
	{
		v2.x+=((gYSize-v1.y)*dxdy)/65536.0;
		v2.u+=((gYSize-v1.y)*dudy)/256.0;
		v2.v+=((gYSize-v1.y)*dvdy)/256.0;
		v2.y=gYSize;
	}
	firsty=ceil(v1.y);
	lasty=floor(v2.y);
	gSlope[firsty].x2=(v1.x*65536.0)+(firsty-v1.y)*dxdy;
	gSlope[firsty].u =(v1.u<<8)+(firsty-v1.y)*dudy;
	gSlope[firsty].v =(v1.v<<8)+(firsty-v1.y)*dvdy;
	for(y=firsty+1;y<=lasty;y++)
	{
		gSlope[y].x2=gSlope[y-1].x2+dxdy;
		gSlope[y].u=gSlope[y-1].u+dudy;
		gSlope[y].v=gSlope[y-1].v+dvdy;
	}
	gSlope[firsty].u+=dudx;
	gSlope[firsty].v+=dvdx;
	gSlope[firsty].x2+=65536;
}

void SlopeInitLine(tVertex v1,tVertex v2)
{
	int y;
	int	dxdy=((v2.x-v1.x)*65536.0)/(v2.y-v1.y);
	int firsty,lasty;
	if(v1.y>=gYSize)
		return;
	if(v2.y<0)
		return;
	if(v1.y<0)
	{
		v1.x+=((-v1.y)*dxdy)/65535.0;
		v1.y=0;
	}
	if(v2.y>=gYSize)
	{
		v2.x+=((gYSize-v1.y)*dxdy)/65535.0;
		v2.y=gYSize;
	}
	firsty=ceil(v1.y);
	lasty=floor(v2.y);
	gSlope[firsty].x1=(v1.x*65536.0)+(firsty-v1.y)*dxdy;
	for(y=firsty+1;y<=lasty;y++)
		gSlope[y].x1=gSlope[y-1].x1+dxdy;
}

int SlopeInit(float cx,float cy,int *y1,int *y2,float dirCos,float dirSin,tSpriteHeader *sprite,int dudx,int dvdx)
{
	tVertex vertices[4];
	int hi;
	float hiY=INFINITY;
	int i;
	vertices[0].x=cx-(dirCos*sprite->xSize/2)+(dirSin*sprite->ySize/2);
	vertices[1].x=cx+(dirCos*sprite->xSize/2)+(dirSin*sprite->ySize/2);
	vertices[2].x=cx+(dirCos*sprite->xSize/2)-(dirSin*sprite->ySize/2);
	vertices[3].x=cx-(dirCos*sprite->xSize/2)-(dirSin*sprite->ySize/2);
	vertices[0].y=cy-(dirSin*sprite->xSize/2)-(dirCos*sprite->ySize/2);
	vertices[1].y=cy+(dirSin*sprite->xSize/2)-(dirCos*sprite->ySize/2);
	vertices[2].y=cy+(dirSin*sprite->xSize/2)+(dirCos*sprite->ySize/2);
	vertices[3].y=cy-(dirSin*sprite->xSize/2)+(dirCos*sprite->ySize/2);
	vertices[0].u=0;
	vertices[1].u=sprite->xSize;
	vertices[2].u=sprite->xSize;
	vertices[3].u=0;
	vertices[0].v=0;
	vertices[1].v=0;
	vertices[2].v=sprite->ySize;
	vertices[3].v=sprite->ySize;
	for(i=0;i<4;i++)
		if(vertices[i].y<hiY)
		{
			hi=i;
			hiY=vertices[i].y;
		}
	if((vertices[(hi+1)&3].x<0)||(vertices[(hi-1)&3].x>=gXSize)) {*y2=0;*y1=0;return false;}
	SlopeInitUVLine(vertices[hi],vertices[(hi-1)&3],dudx,dvdx);
	SlopeInitUVLine(vertices[(hi-1)&3],vertices[(hi-2)&3],dudx,dvdx);
	SlopeInitLine(vertices[hi],vertices[(hi+1)&3]);
	SlopeInitLine(vertices[(hi+1)&3],vertices[(hi+2)&3]);
	*y2=ceil((vertices[(hi+2)&3].y>gYSize?gYSize:vertices[(hi+2)&3].y)-1);
	if(hiY<0) *y1=0;
	else      *y1=ceil(hiY);
	return ((vertices[(hi-1)&3].x<0)||(vertices[(hi+1)&3].x>=gXSize));
}

void DrawSpriteRotatedClippedTranslucent(tSpriteHeader *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt8 mask=sprite->data[0];
	UInt8 *trTab=*gTranslucenceTab;
	for(;y<y2;y++)
	{
		int x=		gSlope[y].x2>>16;
		int endX=	gSlope[y].x1>>16;
		UInt8 *dst=	gBaseAddr+y*gRowBytes;
		UInt8 *endDst;
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		if(x>=gXSize)continue;
		if(endX<0)continue;
		if(x<0){u-=dudx*x;v-=dvdx*x;x=0;}
		if(endX>=gXSize)endX=gXSize-1;
		endDst=dst+endX;
		dst+=x;
		while(dst<endDst)
		{
			UInt8 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=trTab[(color<<8)+*dst];
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

void DrawSpriteRotatedClippedTranslucent16(tSpriteHeader16 *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt16 mask=sprite->data[0];
	for(;y<y2;y++)
	{
		int x=		gSlope[y].x2>>16;
		int endX=	gSlope[y].x1>>16;
		tPixel *dst=	(tPixel*)(gBaseAddr+y*gRowBytes);
		tPixel *endDst;
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		if(x>=gXSize)continue;
		if(endX<0)continue;
		if(x<0){u-=dudx*x;v-=dvdx*x;x=0;}
		if(endX>=gXSize)endX=gXSize-1;
		endDst=dst+endX;
		dst+=x;
		while(dst<endDst)
		{
			UInt16 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=BlendPixel(Pixel555(color),*dst);
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

void DrawSpriteRotatedClipped(tSpriteHeader *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt8 mask=sprite->data[0];
	for(;y<y2;y++)
	{
		int x=		gSlope[y].x2>>16;
		int endX=	gSlope[y].x1>>16;
		UInt8 *dst=	gBaseAddr+y*gRowBytes;
		UInt8 *endDst;
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		if(x>=gXSize)continue;
		if(endX<0)continue;
		if(x<0){u-=dudx*x;v-=dvdx*x;x=0;}
		if(endX>=gXSize)endX=gXSize-1;
		endDst=dst+endX;
		dst+=x;
		while(dst<endDst)
		{
			UInt8 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=color;
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

void DrawSpriteRotatedClipped16(tSpriteHeader16 *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt16 mask=sprite->data[0];
	for(;y<y2;y++)
	{
		int x=		gSlope[y].x2>>16;
		int endX=	gSlope[y].x1>>16;
		tPixel *dst=	(tPixel*)(gBaseAddr+y*gRowBytes);
		tPixel *endDst;
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		if(x>=gXSize)continue;
		if(endX<0)continue;
		if(x<0){u-=dudx*x;v-=dvdx*x;x=0;}
		if(endX>=gXSize)endX=gXSize-1;
		endDst=dst+endX;
		dst+=x;
		while(dst<endDst)
		{
			UInt16 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=Pixel555(color);
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

void DrawSpriteRotatedTranslucent(tSpriteHeader *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt8 mask=sprite->data[0];
	UInt8 *trTab=*gTranslucenceTab;
	for(;y<y2;y++)
	{
		UInt8 *dst=	gBaseAddr+y*gRowBytes+(gSlope[y].x2>>16);
		UInt8 *endDst=	gBaseAddr+y*gRowBytes+(gSlope[y].x1>>16);
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		while(dst<endDst)
		{
			UInt8 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=trTab[(color<<8)+*dst];
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

void DrawSpriteRotatedTranslucent16(tSpriteHeader16 *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt16 mask=sprite->data[0];
	for(;y<y2;y++)
	{
		tPixel *dst=	(tPixel*)(gBaseAddr+y*gRowBytes)+(gSlope[y].x2>>16);
		tPixel *endDst=	(tPixel*)(gBaseAddr+y*gRowBytes)+(gSlope[y].x1>>16);
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		while(dst<endDst)
		{
			UInt16 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=BlendPixel(Pixel555(color),*dst);
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

void DrawSpriteRotated(tSpriteHeader *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt8 mask=sprite->data[0];
	for(;y<y2;y++)		//no clip, no translucence
	{
		UInt8 *dst=	gBaseAddr+y*gRowBytes+(gSlope[y].x2>>16);
		UInt8 *endDst=	gBaseAddr+y*gRowBytes+(gSlope[y].x1>>16);
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		while(dst<endDst)
		{
			UInt8 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=color;
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

void DrawSpriteRotated16(tSpriteHeader16 *sprite,int dudx,int dvdx,int y,int y2)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize; /* row stride; AI sprites exceed 256 texels */
	UInt16 mask=sprite->data[0];
	for(;y<y2;y++)		//no clip, no translucence
	{
		tPixel *dst=	(tPixel*)(gBaseAddr+y*gRowBytes)+(gSlope[y].x2>>16);
		tPixel *endDst=	(tPixel*)(gBaseAddr+y*gRowBytes)+(gSlope[y].x1>>16);
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		while(dst<endDst)
		{
			UInt16 color=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			if(color!=mask)
				*dst=Pixel555(color);
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

/* 32-bit AI sprites: soft edges are mixed with the background by alpha. */
static void DrawSpriteRotated32(tSpriteHeader32 *sprite,int dudx,int dvdx,int y,int y2,
	int translucent)
{
	int vMask=sprite->ySize-1<<8;
	int vShift=sprite->log2xSize;
	for(;y<y2;y++)
	{
		int x=		gSlope[y].x2>>16;
		int endX=	gSlope[y].x1>>16;
		tPixel *dst=	(tPixel*)(gBaseAddr+y*gRowBytes);
		tPixel *endDst;
		int u=		gSlope[y].u;
		int v=		gSlope[y].v;
		if(x>=gXSize)continue;
		if(endX<0)continue;
		if(x<0){u-=dudx*x;v-=dvdx*x;x=0;}
		if(endX>=gXSize)endX=gXSize-1;
		endDst=dst+endX;
		dst+=x;
		while(dst<endDst)
		{
			tPixel texel=sprite->data[(u>>8)+(((v&vMask)>>8)<<vShift)];
			UInt32 alpha=texel>>24;
			if(translucent)
				alpha>>=1;
			if(alpha>=255)
				*dst=texel;
			else if(alpha)
				*dst=BlendAlpha(texel,*dst,alpha);
			u+=dudx;
			v+=dvdx;
			dst++;
		}
	}
}

static void DrawSpriteFromHandle(int id, Handle spriteHandle, int aiDetail,
	float cx, float cy, float dir, float zoom)
{
	if(!spriteHandle) return;
	tSpriteHeader *sprite=(tSpriteHeader*)*spriteHandle;
	if(sprite->drawMode&kDrawModeDoubleSize)
		zoom*=0.5;
	if(aiDetail&&!(sprite->drawMode&kDrawModeTransparent))
		Platform_QueueAIDetailSprite(id,cx,cy,dir,zoom);
	if(aiDetail)
	{
		Handle half=id-128<kNumSprites&&spriteHandle==gSprites[id-128]?
			gHalfSprites[id-128]:nil;
		if(half&&zoom*0.25f<0.75f)
		{
			sprite=(tSpriteHeader*)*half;
			zoom*=0.5f;
		}
		else
			zoom*=0.25f;
	}
	{
		int y,y2;
		float dirCos=cos(dir);
		float dirSin=sin(dir);
		int dudx=dirCos*256/zoom;
		int dvdx=-dirSin*256/zoom;
		dirCos*=zoom;
		dirSin*=zoom;
		if(aiDetail==kAISprite32)
		{
			SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx);
			DrawSpriteRotated32((tSpriteHeader32*)sprite,dudx,dvdx,y,y2,
				sprite->drawMode&kDrawModeTransparent);
		}
		else if(gPrefs.hiColor)
			if(sprite->drawMode&kDrawModeTransparent)
				if(SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx))
					DrawSpriteRotatedClippedTranslucent16((tSpriteHeader16*)sprite,dudx,dvdx,y,y2);
				else
					DrawSpriteRotatedTranslucent16((tSpriteHeader16*)sprite,dudx,dvdx,y,y2);
			else
				if(SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx))
					DrawSpriteRotatedClipped16((tSpriteHeader16*)sprite,dudx,dvdx,y,y2);
				else
					DrawSpriteRotated16((tSpriteHeader16*)sprite,dudx,dvdx,y,y2);
		else
			if(sprite->drawMode&kDrawModeTransparent)
				if(SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx))
					DrawSpriteRotatedClippedTranslucent(sprite,dudx,dvdx,y,y2);
				else
					DrawSpriteRotatedTranslucent(sprite,dudx,dvdx,y,y2);
			else
				if(SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx))
					DrawSpriteRotatedClipped(sprite,dudx,dvdx,y,y2);
				else
					DrawSpriteRotated(sprite,dudx,dvdx,y,y2);
	}
}

void DrawSprite(int id, float cx, float cy, float dir, float zoom)
{
	if(id<128||id-128>=kNumSprites+kNumSpecialSprites)
		return;
	DrawSpriteFromHandle(id,gSprites[id-128],gSpriteAIDetail[id-128],
		cx,cy,dir,zoom);
}

void DrawSpriteTranslucent(int id, float cx, float cy, float dir, float zoom)
{
	if(id<128||!gSprites[id-128]) return;
	tSpriteHeader *sprite=(tSpriteHeader*)*(gSprites[id-128]);
	if(sprite->drawMode&kDrawModeDoubleSize)
		zoom*=0.5;
	if(sprite->drawMode)
		return;
	if(gSpriteAIDetail[id-128])
		zoom*=0.25f;
	{
		int y,y2;
		float dirCos=cos(dir);
		float dirSin=sin(dir);
		int dudx=dirCos*256/zoom;
		int dvdx=-dirSin*256/zoom;
		dirCos*=zoom;
		dirSin*=zoom;
		if(gSpriteAIDetail[id-128]==kAISprite32)
		{
			SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx);
			DrawSpriteRotated32((tSpriteHeader32*)sprite,dudx,dvdx,y,y2,1);
		}
		else if(gPrefs.hiColor)
			if(SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx))
				DrawSpriteRotatedClippedTranslucent16((tSpriteHeader16*)sprite,dudx,dvdx,y,y2);
			else
				DrawSpriteRotatedTranslucent16((tSpriteHeader16*)sprite,dudx,dvdx,y,y2);
		else
			if(SlopeInit(cx,cy,&y,&y2,dirCos,dirSin,sprite,dudx,dvdx))
				DrawSpriteRotatedClippedTranslucent(sprite,dudx,dvdx,y,y2);
			else
				DrawSpriteRotatedTranslucent(sprite,dudx,dvdx,y,y2);
	}
}

/* For damaged copies: the game sprite they were copied from. */
static int gSpriteSource[kNumSprites+kNumSpecialSprites];

/* A loaded sprite, either from the game data or a damaged copy. */
int SpriteIsValid(int id)
{
	return id>=128&&id-128<kNumSprites+kNumSpecialSprites&&gSprites[id-128];
}

/* A damaged copy made by X/YDistortSprite: it exists only on this computer. */
int SpriteIsDynamic(int id)
{
	return id-128>=kNumSprites;
}

/* The undamaged sprite behind a frame (0 when unknown). Other computers
   only know these. */
int SpriteBaseFrame(int id)
{
	if(!SpriteIsDynamic(id))
		return id;
	return SpriteIsValid(id)?gSpriteSource[id-128]:0;
}

int GetUniqueSpriteNum()
{
	int i;
	for(i=kNumSprites;i<kNumSprites+kNumSpecialSprites;i++)
		if(!gSprites[i])return i;
	return 0;
}

void XDistortSprite8(tSpriteHeader *sprite,int startY,int endY,int startX,int endX,int dir,float damage)
{
	UInt8 *lineStart;
	int line;
	lineStart=&sprite->data[sprite->xSize*startY+startX];
	if(damage>2)damage=2;
	if(!dir)
		for(line=0;line<endY;line++)
		{
			int i;
			float c=(float)line/endY;
			float crunch=(-c*c+c)*(1+RanFl(-0.3,0.3))*endX*damage;
			if(crunch){
				float crunchFactor=endX/(endX-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endX-crunch);
				for(i=endX;i>(int)crunch;i--){
					UInt8 pixelValue=lineStart[(int)(endX-((endX-i)*crunchFactor))];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*((endX-i)/crunchLength);
						lineStart[i]=gLightningTab[shade][pixelValue];
					}
				}
				for(i=0;i<=(int)crunch;i++)
					lineStart[i]=sprite->data[0];
			}
			lineStart+=sprite->xSize;
		}
	else for(line=0;line<endY;line++)
		{
			int i;
			float c=(float)line/endY;
			float crunch=(-c*c+c)*(1+RanFl(-0.3,0.3))*endX*damage;
			if(crunch){
				float crunchFactor=endX/(endX-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endX-crunch);
				for(i=0;i<endX-(int)crunch;i++){
					UInt8 pixelValue=lineStart[(int)(i*crunchFactor)];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*(i/crunchLength);
						lineStart[i]=gLightningTab[shade][pixelValue];
					}
				}
				for(i=endX;i>=endX-(int)crunch;i--)
					lineStart[i]=sprite->data[0];
			}
			lineStart+=sprite->xSize;
		}
}

void XDistortSprite16(tSpriteHeader16 *sprite,int startY,int endY,int startX,int endX,int dir,float damage)
{
	UInt16 *lineStart;
	int line;
	lineStart=&sprite->data[sprite->xSize*startY+startX];
	if(damage>2)damage=2;
	if(!dir)
		for(line=0;line<endY;line++)
		{
			int i;
			float c=(float)line/endY;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endX*damage;
			if(crunch){
				float crunchFactor=endX/(endX-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endX-crunch);
				for(i=endX;i>(int)crunch;i--){
					UInt16 pixelValue=lineStart[(int)(endX-((endX-i)*crunchFactor))];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*((endX-i)/crunchLength);
						lineStart[i]=ShadeRGB16(shade,pixelValue);
					}
				}
				for(i=0;i<=(int)crunch;i++)
					lineStart[i]=sprite->data[0];
			}
			lineStart+=sprite->xSize;
		}
	else for(line=0;line<endY;line++)
		{
			int i;
			float c=(float)line/endY;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endX*damage;
			if(crunch){
				float crunchFactor=endX/(endX-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endX-crunch);
				for(i=0;i<endX-(int)crunch;i++){
					UInt16 pixelValue=lineStart[(int)(i*crunchFactor)];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*(i/crunchLength);
						lineStart[i]=ShadeRGB16(shade,pixelValue);
					}
				}
				for(i=endX;i>=endX-(int)crunch;i--)
					lineStart[i]=sprite->data[0];
			}
			lineStart+=sprite->xSize;
		}
}

void XDistortSprite32(tSpriteHeader32 *sprite,int startY,int endY,int startX,int endX,int dir,float damage)
{
	tPixel *lineStart;
	int line;
	lineStart=&sprite->data[sprite->xSize*startY+startX];
	if(damage>2)damage=2;
	if(!dir)
		for(line=0;line<endY;line++)
		{
			int i;
			float c=(float)line/endY;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endX*damage;
			if(crunch){
				float crunchFactor=endX/(endX-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endX-crunch);
				for(i=endX;i>(int)crunch;i--){
					tPixel pixelValue=lineStart[(int)(endX-((endX-i)*crunchFactor))];
					if(pixelValue>>24){
						int shade=(kLightValues-1)-maxShade*((endX-i)/crunchLength);
						lineStart[i]=ShadeTexel(shade,pixelValue);
					}
				}
				for(i=0;i<=(int)crunch;i++)
					lineStart[i]=0;
			}
			lineStart+=sprite->xSize;
		}
	else for(line=0;line<endY;line++)
		{
			int i;
			float c=(float)line/endY;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endX*damage;
			if(crunch){
				float crunchFactor=endX/(endX-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endX-crunch);
				for(i=0;i<endX-(int)crunch;i++){
					tPixel pixelValue=lineStart[(int)(i*crunchFactor)];
					if(pixelValue>>24){
						int shade=(kLightValues-1)-maxShade*(i/crunchLength);
						lineStart[i]=ShadeTexel(shade,pixelValue);
					}
				}
				for(i=endX;i>=endX-(int)crunch;i--)
					lineStart[i]=0;
			}
			lineStart+=sprite->xSize;
		}
}

int XDistortSprite(int id,int startY,int endY,int startX,int endX,int dir,float damage)
{
	tSpriteHeader *sprite;
	if(!SpriteIsValid(id))
		return id;
	if(id-128<kNumSprites)
	{
		int newID=GetUniqueSpriteNum();
		if(!newID) return id;
		gSprites[newID]=DuplicateHandle(gSprites[id-128], id-128);
		gSpriteHandleSizes[newID]=gSpriteHandleSizes[id-128];
		if(!gSprites[newID]) return id;
		gSpriteAIDetail[newID]=gSpriteAIDetail[id-128];
		gSpriteSource[newID]=id;
		sprite=(tSpriteHeader*)*(gSprites[newID]);
		id=newID+128;
	}
	else
		sprite=(tSpriteHeader*)*(gSprites[id-128]);
	if(gSpriteAIDetail[id-128])
	{
		startY*=4;
		endY*=4;
		startX*=4;
		endX*=4;
	}
	if(sprite->drawMode&kDrawModeDoubleSize)
	{
		startY*=2;
		endY*=2;
		startX*=2;
		endX*=2;
	}
	endY-=startY;
	endX-=startX;
	startX+=sprite->xSize/2;
	startY+=sprite->ySize/2;
	if(startX<0){endX+=startX;startX=0;}
	if(startY<0){endY+=startY;startY=0;}
	if(startX+endX>=sprite->xSize){endX-=startX+endX+1-sprite->xSize;}
	if(startY+endY>=sprite->ySize){endY-=startY+endY+1-sprite->ySize;}
	if(gSpriteAIDetail[id-128]==kAISprite32)
		XDistortSprite32((tSpriteHeader32*)sprite,startY,endY,startX,endX,dir,damage);
	else if(gPrefs.hiColor)
		XDistortSprite16((tSpriteHeader16*)sprite,startY,endY,startX,endX,dir,damage);
	else
		XDistortSprite8(sprite,startY,endY,startX,endX,dir,damage);
	return id;
}

void YDistortSprite8(tSpriteHeader *sprite,int startX,int endX,int startY,int endY,int dir,float damage)
{
	UInt8 *collumStart;
	int collum;
	collumStart=&sprite->data[sprite->xSize*startY+startX];
	if(damage>2)damage=2;
	if(!dir)
		for(collum=0;collum<endX;collum++)
		{
			int i;
			float c=(float)collum/endX;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endY*damage;
			if(crunch){
				float crunchFactor=endY/(endY-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endY-crunch);
				for(i=endY;i>(int)crunch;i--){
					UInt8 pixelValue=collumStart[(int)(endY-((endY-i)*crunchFactor))*sprite->xSize];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*((endY-i)/crunchLength);
						collumStart[i*sprite->xSize]=gLightningTab[shade][pixelValue];
					}
				}
				for(i=0;i<=(int)crunch;i++)
					collumStart[i*sprite->xSize]=sprite->data[0];
			}
			collumStart++;
		}
	else for(collum=0;collum<endX;collum++)
		{
			int i;
			float c=(float)collum/endX;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endY*damage;
			if(crunch){
				float crunchFactor=endY/(endY-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endY-crunch);
				for(i=0;i<endY-(int)crunch;i++){
					UInt8 pixelValue=collumStart[(int)(i*crunchFactor)*sprite->xSize];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*(i/crunchLength);
						collumStart[i*sprite->xSize]=gLightningTab[shade][pixelValue];
					}
				}
				for(i=endY;i>=endY-(int)crunch;i--)
					collumStart[i*sprite->xSize]=sprite->data[0];
			}
			collumStart++;
		}
}

void YDistortSprite16(tSpriteHeader16 *sprite,int startX,int endX,int startY,int endY,int dir,float damage)
{
	UInt16 *collumStart;
	int collum;
	collumStart=&sprite->data[sprite->xSize*startY+startX];
	if(damage>2)damage=2;
	if(!dir)
		for(collum=0;collum<endX;collum++)
		{
			int i;
			float c=(float)collum/endX;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endY*damage;
			if(crunch){
				float crunchFactor=endY/(endY-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endY-crunch);
				for(i=endY;i>(int)crunch;i--){
					UInt16 pixelValue=collumStart[(int)(endY-((endY-i)*crunchFactor))*sprite->xSize];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*((endY-i)/crunchLength);
						collumStart[i*sprite->xSize]=ShadeRGB16(shade,pixelValue);
					}
				}
				for(i=0;i<=(int)crunch;i++)
					collumStart[i*sprite->xSize]=sprite->data[0];
			}
			collumStart++;
		}
	else for(collum=0;collum<endX;collum++)
		{
			int i;
			float c=(float)collum/endX;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endY*damage;
			if(crunch){
				float crunchFactor=endY/(endY-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endY-crunch);
				for(i=0;i<endY-(int)crunch;i++){
					UInt16 pixelValue=collumStart[(int)(i*crunchFactor)*sprite->xSize];
					if(pixelValue!=sprite->data[0]){
						int shade=(kLightValues-1)-maxShade*(i/crunchLength);
						collumStart[i*sprite->xSize]=ShadeRGB16(shade,pixelValue);
					}
				}
				for(i=endY;i>=endY-(int)crunch;i--)
					collumStart[i*sprite->xSize]=sprite->data[0];
			}
			collumStart++;
		}
}

void YDistortSprite32(tSpriteHeader32 *sprite,int startX,int endX,int startY,int endY,int dir,float damage)
{
	tPixel *collumStart;
	int collum;
	collumStart=&sprite->data[sprite->xSize*startY+startX];
	if(damage>2)damage=2;
	if(!dir)
		for(collum=0;collum<endX;collum++)
		{
			int i;
			float c=(float)collum/endX;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endY*damage;
			if(crunch){
				float crunchFactor=endY/(endY-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endY-crunch);
				for(i=endY;i>(int)crunch;i--){
					tPixel pixelValue=collumStart[(int)(endY-((endY-i)*crunchFactor))*sprite->xSize];
					if(pixelValue>>24){
						int shade=(kLightValues-1)-maxShade*((endY-i)/crunchLength);
						collumStart[i*sprite->xSize]=ShadeTexel(shade,pixelValue);
					}
				}
				for(i=0;i<=(int)crunch;i++)
					collumStart[i*sprite->xSize]=0;
			}
			collumStart++;
		}
	else for(collum=0;collum<endX;collum++)
		{
			int i;
			float c=(float)collum/endX;
			float crunch=(-c*c+c)*(1+RanFl(-0.5,0.5))*endY*damage;
			if(crunch){
				float crunchFactor=endY/(endY-crunch);
				float invCrunchFac=1/crunchFactor;
				float maxShade=(kLightValues-1)*(1-invCrunchFac);
				float crunchLength=(endY-crunch);
				for(i=0;i<endY-(int)crunch;i++){
					tPixel pixelValue=collumStart[(int)(i*crunchFactor)*sprite->xSize];
					if(pixelValue>>24){
						int shade=(kLightValues-1)-maxShade*(i/crunchLength);
						collumStart[i*sprite->xSize]=ShadeTexel(shade,pixelValue);
					}
				}
				for(i=endY;i>=endY-(int)crunch;i--)
					collumStart[i*sprite->xSize]=0;
			}
			collumStart++;
		}
}

int YDistortSprite(int id,int startX,int endX,int startY,int endY,int dir,float damage)
{
	tSpriteHeader *sprite;
	if(!SpriteIsValid(id))
		return id;
	if(id-128<kNumSprites)
	{
		int newID=GetUniqueSpriteNum();
		if(!newID) return id;
		gSprites[newID]=DuplicateHandle(gSprites[id-128], id-128);
		gSpriteHandleSizes[newID]=gSpriteHandleSizes[id-128];
		if(!gSprites[newID]) return id;
		gSpriteAIDetail[newID]=gSpriteAIDetail[id-128];
		gSpriteSource[newID]=id;
		sprite=(tSpriteHeader*)*(gSprites[newID]);
		id=newID+128;
	}
	else
		sprite=(tSpriteHeader*)*(gSprites[id-128]);
	if(gSpriteAIDetail[id-128])
	{
		startY*=4;
		endY*=4;
		startX*=4;
		endX*=4;
	}
	if(sprite->drawMode&kDrawModeDoubleSize)
	{
		startY*=2;
		endY*=2;
		startX*=2;
		endX*=2;
	}
	endY-=startY;
	endX-=startX;
	startX+=sprite->xSize/2;
	startY+=sprite->ySize/2;
	if(startX<0){endX+=startX;startX=0;}
	if(startY<0){endY+=startY;startY=0;}
	if(startX+endX>=sprite->xSize){endX-=startX+endX+1-sprite->xSize;}
	if(startY+endY>=sprite->ySize){endY-=startY+endY+1-sprite->ySize;}
	if(gSpriteAIDetail[id-128]==kAISprite32)
		YDistortSprite32((tSpriteHeader32*)sprite,startX,endX,startY,endY,dir,damage);
	else if(gPrefs.hiColor)
		YDistortSprite16((tSpriteHeader16*)sprite,startX,endX,startY,endY,dir,damage);
	else
		YDistortSprite8(sprite,startX,endX,startY,endY,dir,damage);
	return id;
}

void BulletHitSprite8(tSpriteHeader *sprite,int x,int y,int size)
{
	int offs;
	float invSize=1.0f/size;
	int xPos,yPos;
	for(yPos=y-size;yPos<=y+size;yPos++)
		if(yPos>=0&&yPos<sprite->ySize)
			for(xPos=x-size;xPos<=x+size;xPos++)
				if(xPos>=0&&xPos<sprite->ySize)
					if(sprite->data[offs=(yPos*sprite->xSize+xPos)]!=sprite->data[0])
						sprite->data[offs]=gLightningTab[(int)(kLightValues*(0.6+0.2*invSize*(fabs(x-xPos)+fabs(y-yPos))))][sprite->data[offs]];
}

void BulletHitSprite16(tSpriteHeader16 *sprite,int x,int y,int size)
{
	int offs;
	float invSize=1.0f/size;
	int xPos,yPos;
	for(yPos=y-size;yPos<=y+size;yPos++)
		if(yPos>=0&&yPos<sprite->ySize)
			for(xPos=x-size;xPos<=x+size;xPos++)
				if(xPos>=0&&xPos<sprite->ySize)
					if(sprite->data[offs=(yPos*sprite->xSize+xPos)]!=sprite->data[0])
						sprite->data[offs]=ShadeRGB16((int)(kLightValues*(0.6+0.2*invSize*(fabs(x-xPos)+fabs(y-yPos)))),sprite->data[offs]);
}

void BulletHitSprite32(tSpriteHeader32 *sprite,int x,int y,int size)
{
	int offs;
	float invSize=1.0f/size;
	int xPos,yPos;
	for(yPos=y-size;yPos<=y+size;yPos++)
		if(yPos>=0&&yPos<sprite->ySize)
			for(xPos=x-size;xPos<=x+size;xPos++)
				if(xPos>=0&&xPos<sprite->ySize)
					if(sprite->data[offs=(yPos*sprite->xSize+xPos)]>>24)
						sprite->data[offs]=ShadeTexel((int)(kLightValues*(0.6+0.2*invSize*(fabs(x-xPos)+fabs(y-yPos)))),sprite->data[offs]);
}

int BulletHitSprite(int id,int x,int y)
{
	tSpriteHeader *sprite;
	int size;
	if(!SpriteIsValid(id))
		return id;
	if(id-128<kNumSprites)
	{
		int newID=GetUniqueSpriteNum();
		if(!newID) return id;
		gSprites[newID]=DuplicateHandle(gSprites[id-128], id-128);
		gSpriteHandleSizes[newID]=gSpriteHandleSizes[id-128];
		if(!gSprites[newID]) return id;
		gSpriteAIDetail[newID]=gSpriteAIDetail[id-128];
		gSpriteSource[newID]=id;
		sprite=(tSpriteHeader*)*(gSprites[newID]);
		id=newID+128;
	}
	else
		sprite=(tSpriteHeader*)*(gSprites[id-128]);
	if(gSpriteAIDetail[id-128])
	{
		x*=4;
		y*=4;
	}
	if(sprite->drawMode&kDrawModeDoubleSize)
	{
		x*=2;
		y*=2;
		size=2;
	}else
		size=1;
	x+=sprite->xSize/2;
	y-=sprite->ySize/2;
	y=-y;
	if(gSpriteAIDetail[id-128]==kAISprite32)
		BulletHitSprite32((tSpriteHeader32*)sprite,x,y,size);
	else if(gPrefs.hiColor)
		BulletHitSprite16((tSpriteHeader16*)sprite,x,y,size);
	else
		BulletHitSprite8(sprite,x,y,size);
	return id;
}

void SpriteUnused(int id)
{
	if(id-128>=kNumSprites)
		if(gSprites[id-128])
		{
			DisposeHandle(gSprites[id-128]);
			gSprites[id-128]=nil;
			gSpriteHandleSizes[id-128]=0;
			gSpriteAIDetail[id-128]=0;
			gSpriteSource[id-128]=0;
		}
}

void LoadSprites()
{
	int i;
	int spritePack=gPrefs.hiColor?kPackSp16:kPackSprt;
	LoadPack(spritePack);
	for(i=0;i<kNumSprites;i++)
		gHalfSprites[i]=nil;
	for(i=128;i<128+kNumSprites;i++)
	{
		int size;
		Ptr data=GetUnsortedPackEntry(spritePack,i,&size);
		if(data)
		{
			gSprites[i-128]=PtrToHandCompat(data, size);
			gSpriteHandleSizes[i-128]=size;
			/* Byte-swap sprite header and pixel data from big-endian to native */
			if(gSprites[i-128] && *gSprites[i-128]) {
				tSpriteHeader *hdr = (tSpriteHeader*)*gSprites[i-128];
				hdr->xSize = SWAP16(hdr->xSize);
				hdr->ySize = SWAP16(hdr->ySize);
				/* log2xSize, drawMode, fillers are single bytes - no swap needed */
#ifndef __BIG_ENDIAN__
				if(gPrefs.hiColor) {
					/* Swap all UInt16 pixel data in-place */
					int numPixels = (size - 8) / 2; /* 8-byte header */
					UInt16 *pixels = ((tSpriteHeader16*)hdr)->data;
					int p;
					for(p = 0; p < numPixels; p++)
						pixels[p] = SWAP16(pixels[p]);
				}
#endif
			}
			gSpriteAIDetail[i-128]=0;
			if(gSprites[i-128]&&*gSprites[i-128])
				LoadAIDetailSprite(i);
		}
		else
		{
			gSprites[i-128]=nil;
			gSpriteHandleSizes[i-128]=0;
			gSpriteAIDetail[i-128]=0;
		}
	}
	for(i=kNumSprites;i<kNumSprites+kNumSpecialSprites;i++)
	{
		gSprites[i]=nil;
		gSpriteHandleSizes[i]=0;
		gSpriteAIDetail[i]=0;
	}
	UnloadPack(spritePack);
}

void DrawLifeBar8(int cy,int cx,int shift)
{
	tSpriteHeader *sprite=(tSpriteHeader*)*(gSprites[kLifeBarId-128]);
	int y=cy-sprite->ySize/2;
	int x=cx-sprite->xSize/2-shift;
	int vShift=sprite->log2xSize;
	UInt8 mask=sprite->data[0];
	int v=0;
	int endU=sprite->xSize;
	int startU=shift;
	int endV=sprite->ySize;
	UInt8 *dst=gBaseAddr+y*gRowBytes+x;
	UInt8 *trTab=*gTranslucenceTab;
	for(;v<endV;v++)
	{
		int u=startU;
		for(;u<endU;u++)
		{
			UInt8 color=sprite->data[u+(v<<vShift)];
			if(color!=mask)
				*(dst+u)=trTab[(color<<8)+*(dst+u)];
		}
		dst+=gRowBytes;
	}
}

void DrawLifeBar16(int cy,int cx,int shift)
{
	tSpriteHeader16 *sprite=(tSpriteHeader16*)*(gSprites[kLifeBarId-128]);
	int y=cy-sprite->ySize/2;
	int x=cx-sprite->xSize/2-shift;
	int vShift=sprite->log2xSize;
	UInt16 mask=sprite->data[0];
	int v=0;
	int endU=sprite->xSize;
	int startU=shift;
	int endV=sprite->ySize;
	tPixel *dst=(tPixel*)(gBaseAddr+y*gRowBytes)+x;
	for(;v<endV;v++)
	{
		int u=startU;
		for(;u<endU;u++)
		{
			UInt16 color=sprite->data[u+(v<<vShift)];
			if(color!=mask)
				*(dst+u)=BlendPixel(Pixel555(color),*(dst+u));
		}
		dst+=gRowBytes/4;
	}
}

void DrawLifeBar(int cx,int cy,int shift)
{
	if(gPrefs.hiColor)
		DrawLifeBar16(cy,cx,shift);
	else
		DrawLifeBar8(cy,cx,shift);
}


void UnloadSprites()
{
	int i;
	Platform_ClearAIDetailSprites();
	for(i=0;i<kNumSprites+kNumSpecialSprites;i++)
		if(gSprites[i])
		{
			DisposeHandle(gSprites[i]);
			gSprites[i]=nil;
			gSpriteHandleSizes[i]=0;
			gSpriteAIDetail[i]=0;
		}
	for(i=0;i<kNumSprites;i++)
	{
		if(gHalfSprites[i])
		{
			DisposeHandle(gHalfSprites[i]);
			gHalfSprites[i]=nil;
		}
	}
}

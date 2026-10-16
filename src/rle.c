#include "compat.h"
#include <stdio.h>
#include <string.h>
#include "screen.h"
#include "platform.h"
#include "packs.h"
#include "error.h"
#include "preferences.h"
#include "hqassets.h"

#define kEndShapeToken		0				// the end of shape maker
#define kLineStartToken		1				// the line start marker
#define kDrawPixelsToken	2				// the draw run marker
#define kSkipPixelsToken	3				// the skip pixels marker

/* Note: The RLE data from packs is big-endian. The token byte is the first byte
   of a 32-bit word, and tokenData is the lower 24 bits (big-endian).
   On little-endian, we need to read these correctly. */

static inline UInt32 ReadRLEToken(UInt8 *ptr, UInt8 *tokenType, UInt32 *tokenData)
{
	/* Original Mac (big-endian) format: byte0=token, bytes1-3=data (24-bit) */
	*tokenType = ptr[0];
	*tokenData = ((UInt32)ptr[1] << 16) | ((UInt32)ptr[2] << 8) | (UInt32)ptr[3];
	return *tokenData;
}

/* Skip past the Rect structure at the start of RLE data (8 bytes) */
#define kRectSize 8

/* Pad byte count up to next 4-byte boundary */
static inline int RLEPad4(int n) { return (n+3) & ~3; }

static void DrawRLE8Scaled(int h,int v,int id,int clipTop)
{
	int scale=Platform_GetRenderScale();
	int logicalX=0,logicalY=0,stop=0;
	int maxHeight=gYSize+FRAMEBUFFER_GUARD_ROWS*scale;
	UInt8 *spritePos=GetSortedPackEntry(kPacksRLE,id,nil);
	if(!spritePos) return;
	spritePos+=kRectSize;
	do
	{
		UInt8 tokenType;
		UInt32 tokenData;
		ReadRLEToken(spritePos,&tokenType,&tokenData);
		switch(tokenType)
		{
			case kDrawPixelsToken:
			{
				UInt8 *src=spritePos+4;
				UInt32 p;
				spritePos+=4+RLEPad4((int)tokenData);
				for(p=0;p<tokenData;p++)
				{
					int x=(h+logicalX+(int)p)*scale;
					int y=(v+logicalY)*scale;
					int sy,sx;
					for(sy=0;sy<scale;sy++)
					{
						if(y+sy<0||y+sy>=maxHeight) continue;
						for(sx=0;sx<scale;sx++)
							if(x+sx>=0&&x+sx<gXSize)
								*(UInt8*)(gBaseAddr+(y+sy)*gRowBytes+x+sx)=src[p];
					}
				}
				logicalX+=(int)tokenData;
				break;
			}
			case kSkipPixelsToken:
				logicalX+=(int)tokenData;
				spritePos+=4;
				break;
			case kLineStartToken:
				logicalY++;
				logicalX=0;
				spritePos+=4;
				if(clipTop&&v+logicalY>=gYSize/scale) stop=1;
				break;
			case kEndShapeToken:
			default:
				stop=1;
				break;
		}
	}
	while(!stop);
}

static void DrawRLE16Scaled(int h,int v,int id,int clipTop)
{
	int scale=Platform_GetRenderScale();
	int logicalX=0,logicalY=0,stop=0;
	int maxHeight=gYSize+FRAMEBUFFER_GUARD_ROWS*scale;
	const tHQImage *hq=HQAssets_Load("rle",id,0);
	if(hq&&scale<=hq->scale&&hq->scale%scale==0)
	{
		int sourceStep=hq->scale/scale;
		int xStart=h*scale;
		int yStart=v*scale;
		int width=hq->width*scale;
		int height=hq->height*scale;
		for(int y=0;y<height;y++)
		{
			int dstY=yStart+y;
			int srcY=y*sourceStep;
			tPixel *dst;
			if(dstY<0||dstY>=maxHeight)
				continue;
			if(clipTop&&dstY>=gYSize)
				break;
			dst=(tPixel*)(gBaseAddr+dstY*gRowBytes);
			for(int x=0;x<width;x++)
			{
				int dstX=xStart+x;
				int srcX=x*sourceStep;
				const UInt8 *src=hq->rgba+
					((size_t)srcY*(hq->width*hq->scale)+srcX)*4;
				if(src[3]>=128&&dstX>=0&&dstX<gXSize)
					dst[dstX]=PixelRGB(src[0],src[1],src[2]);
			}
		}
		return;
	}
	UInt8 *spritePos=GetSortedPackEntry(kPacksR16,id,nil);
	if(!spritePos) return;
	spritePos+=kRectSize;
	do
	{
		UInt8 tokenType;
		UInt32 tokenData;
		ReadRLEToken(spritePos,&tokenType,&tokenData);
		switch(tokenType)
		{
			case kDrawPixelsToken:
			{
				UInt16 *src=(UInt16*)(spritePos+4);
				UInt32 p;
				spritePos+=4+RLEPad4((int)tokenData*2);
				for(p=0;p<tokenData;p++)
				{
					int x=(h+logicalX+(int)p)*scale;
					int y=(v+logicalY)*scale;
					int sy,sx;
					tPixel color=Pixel555(SWAP16(src[p]));
					for(sy=0;sy<scale;sy++)
					{
						tPixel *dst;
						if(y+sy<0||y+sy>=maxHeight) continue;
						dst=(tPixel*)(gBaseAddr+(y+sy)*gRowBytes);
						for(sx=0;sx<scale;sx++)
							if(x+sx>=0&&x+sx<gXSize)
								dst[x+sx]=color;
					}
				}
				logicalX+=(int)tokenData;
				break;
			}
			case kSkipPixelsToken:
				logicalX+=(int)tokenData;
				spritePos+=4;
				break;
			case kLineStartToken:
				logicalY++;
				logicalX=0;
				spritePos+=4;
				if(clipTop&&v+logicalY>=gYSize/scale) stop=1;
				break;
			case kEndShapeToken:
			default:
				stop=1;
				break;
		}
	}
	while(!stop);
}

void DrawRLE8(int h,int v,int id)
{
	DrawRLE8Scaled(h,v,id,0);
}

void DrawRLE16(int h,int v,int id)
{
	DrawRLE16Scaled(h,v,id,0);
}

void DrawRLEYClip8(int h,int v,int id)
{
	DrawRLE8Scaled(h,v,id,1);
}

void DrawRLEYClip16(int h,int v,int id)
{
	DrawRLE16Scaled(h,v,id,1);
}

void DrawRLE(int h,int v,int id)
{
	if(gPrefs.hiColor)
		 DrawRLE16(h,v,id);
	else
		 DrawRLE8(h,v,id);
}

void DrawRLEYClip(int h,int v,int id)
{
	if(gPrefs.hiColor)
		 DrawRLEYClip16(h,v,id);
	else
		 DrawRLEYClip8(h,v,id);
}

#include "compat.h"
#include <string.h>
#include "screen.h"
#include "platform.h"
#include "objects.h"
#include "textfx.h"
#include "trig.h"
#include "renderframe.h"
#include "packs.h"
#include "preferences.h"
#include "netgame.h"
#include "hqassets.h"

#define kMaxFX	32
#define kEndShapeToken		0				// the end of shape maker
#define kLineStartToken		1				// the line start marker
#define kDrawPixelsToken	2				// the draw run marker
#define kSkipPixelsToken	3				// the skip pixels marker
#define kCharSize 32

/* Read big-endian RLE token: byte0=type, bytes1-3=24-bit data */
static inline void ReadToken(UInt8 *ptr, UInt8 *tokenType, UInt32 *tokenData)
{
	*tokenType = ptr[0];
	*tokenData = ((UInt32)ptr[1] << 16) | ((UInt32)ptr[2] << 8) | (UInt32)ptr[3];
}

/* Pad to next 4-byte boundary */
static inline int Pad4(int n) { return (n+3) & ~3; }
#define kExplAccel 10.0
#define kEffectAccel 150.0
#define kEffectTimeOut 2.0
#define kEffectSinAmp	30.0
#define kEffectSinFreq	(2.0/kCharSize)
#define kEffectSinVelo	(2*PI)

tTextEffect gTextFX[kMaxFX];
int gFXCount=0;

void NewTextEffect(tTextEffect *effect)
{
	if(gFXCount<kMaxFX)
	{
		gTextFX[gFXCount]=*effect;
		gTextFX[gFXCount].fxStartFrame=gFrameCount;
		/* Messages raised for one driver (pickups, OUCH, level completed)
		   only show in that driver's split-screen view. */
		gTextFX[gFXCount].owner=(SInt8)Network_GetBoundSlot();
		gTextFX[gFXCount].viewPlaced=0;
		gFXCount++;
	}
}

void MakeFXStringFromNumStr(Str31 numStr,Str31 fxStr)
{
	int i;
	for(i=0;i<numStr[0];i++)
		fxStr[i+1]=numStr[i+1]+0x2b;
	fxStr[0]=numStr[0];
}

void DrawZoomedCharLine8(UInt8 **data,SInt32 x,SInt32 y,UInt32 zoom)
{
	UInt8 *spritePos=*data;
	UInt8 *lineBase=gBaseAddr+y*gRowBytes;
	int stop=0;
	if((y<0)||(y>=gYSize-kInvLines)) goto noDrawZoomed;
	if(((x>>16)+(int)(zoom>>11)<0)||((x>>16)>=gXSize)) goto noDrawZoomed;
	if(((x>>16)<0)||((x>>16)+(int)(zoom>>11)>=gXSize)) goto noDrawZoomed;
	while(!stop)
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				{
					UInt32 i=0;
					UInt8 *src=spritePos+4;
					spritePos+=4+Pad4((int)tokenData);
					while(i<tokenData)
					{
						UInt8 *dst=lineBase+(x>>16);
						*dst=*(src+i);
						i++;
						x+=zoom;
					}
				}
				break;
			case kSkipPixelsToken:
				x+=tokenData*zoom;
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
noDrawZoomed:
	while(!stop)
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				spritePos+=4+Pad4((int)tokenData);
				break;
			case kSkipPixelsToken:
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
	*data=spritePos+4;
}

void DrawCharLine8(UInt8 **data,SInt32 x,SInt32 y)
{
	UInt8 *spritePos=*data;
	UInt8 *dst=gBaseAddr+x+y*gRowBytes;
	int stop=0;
	if((y<0)||(y>=gYSize-kInvLines)) goto noDraw;
	if((x+kCharSize<0)||(x>=gXSize)) goto noDraw;
	if((x<0)||(x+kCharSize>=gXSize)) goto noDraw;
	do
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				{
					UInt8 *src=spritePos+4;
					int byteCount=(int)tokenData;
					spritePos+=4+Pad4(byteCount);
					memcpy(dst, src, byteCount);
					dst+=byteCount;
				}
				break;
			case kSkipPixelsToken:
				dst+=tokenData;
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
	while (!stop);
noDraw:
	do
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				spritePos+=4+Pad4((int)tokenData);
				break;
			case kSkipPixelsToken:
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
	while (!stop);
	*data=spritePos+4;
}

void DrawZoomedCharLine16(UInt8 **data,SInt32 x,SInt32 y,UInt32 zoom)
{
	UInt8 *spritePos=*data;
	UInt8 *lineBase=gBaseAddr+y*gRowBytes;
	int stop=0;
	if((y<0)||(y>=gYSize-kInvLines)) goto noDrawZoomed;
	if(((x>>16)+(int)(zoom>>11)<0)||((x>>16)>=gXSize)) goto noDrawZoomed;
	if(((x>>16)<0)||((x>>16)+(int)(zoom>>11)>=gXSize)) goto noDrawZoomed;
	while(!stop)
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				{
					UInt32 i=0;
					UInt16 *src=(UInt16*)(spritePos+4);
					int byteCount=(int)tokenData*2;
					spritePos+=4+Pad4(byteCount);
					while(i<tokenData)
					{
						tPixel *dst=(tPixel*)lineBase+(x>>16);
						*dst=Pixel555(SWAP16(*(src+i)));
						i++;
						x+=zoom;
					}
				}
				break;
			case kSkipPixelsToken:
				x+=tokenData*zoom;
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
noDrawZoomed:
	while(!stop)
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				spritePos+=4+Pad4((int)tokenData*2);
				break;
			case kSkipPixelsToken:
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
	*data=spritePos+4;
}

void DrawCharLine16(UInt8 **data,SInt32 x,SInt32 y)
{
	UInt8 *spritePos=*data;
	tPixel *dst=(tPixel*)(gBaseAddr+y*gRowBytes)+x;
	int stop=0;
	if((y<0)||(y>=gYSize-kInvLines)) goto noDraw;
	if((x+kCharSize<0)||(x>=gXSize)) goto noDraw;
	if((x<0)||(x+kCharSize>=gXSize)) goto noDraw;
	do
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				{
					UInt16 *src=(UInt16*)(spritePos+4);
					int pixelCount=(int)tokenData;
					int byteCount=pixelCount*2;
					int p;
					spritePos+=4+Pad4(byteCount);
					for(p=0;p<pixelCount;p++)
						dst[p]=Pixel555(SWAP16(src[p]));
					dst+=pixelCount;
				}
				break;
			case kSkipPixelsToken:
				dst+=tokenData;
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
	while (!stop);
noDraw:
	do
	{
		UInt8 tokenType; UInt32 tokenData;
		ReadToken(spritePos, &tokenType, &tokenData);
		switch (tokenType)
		{
			case kDrawPixelsToken:
				spritePos+=4+Pad4((int)tokenData*2);
				break;
			case kSkipPixelsToken:
				spritePos+=4;
				break;
			case kLineStartToken:
			case kEndShapeToken:
				stop=true;
				break;
		}
	}
	while (!stop);
	*data=spritePos+4;
}

static inline void DrawZoomedCharLine(UInt8 **data,SInt32 x,SInt32 y,UInt32 zoom)
{
	if(gPrefs.hiColor)
		DrawZoomedCharLine16(data,x,y,zoom);
	else
		DrawZoomedCharLine8(data,x,y,zoom);	
}

static inline void DrawCharLine(UInt8 **data,SInt32 x,SInt32 y)
{
	if(gPrefs.hiColor)
		DrawCharLine16(data,x,y);
	else
		DrawCharLine8(data,x,y);	
}


void DrawTextFX(int xDrawStart,int yDrawStart)
{
	int i,line,ch;
	for(i=0;i<gFXCount;i++)
	{
		float dt=(gFrameCount-gTextFX[i].fxStartFrame)*kFrameDuration*(gTextFX[i].effectFlags&kEffectTiny?3:1);
		float exploZoom=(gTextFX[i].effectFlags&kEffectExplode?1+0.5*dt*dt*kExplAccel:1)*(gTextFX[i].effectFlags&kEffectTiny?0.25:1)*
			(gTextFX[i].effectFlags&kEffectSmall?0.5:1);
		float baseX=gTextFX[i].x-gTextFX[i].text[0]*kCharSize*0.5*exploZoom;
		float baseY=gTextFX[i].y-kCharSize*0.5*exploZoom;
		if(gTextFX[i].effectFlags&kEffectAbsPos)
		{
			baseY=yDrawStart-gTextFX[i].y-baseY;
			baseX-=xDrawStart;
			gTextFX[i].x-=xDrawStart;
			gTextFX[i].y=yDrawStart-gTextFX[i].y;
			gTextFX[i].effectFlags^=kEffectAbsPos;
		}
		if(gTextFX[i].effectFlags&kEffectMoveUp)
			baseY-=0.5*dt*dt*kEffectAccel;
		else if(gTextFX[i].effectFlags&kEffectMoveDown)
			baseY+=0.5*dt*dt*kEffectAccel;
		if(gTextFX[i].effectFlags&kEffectMoveLeft)
			baseX-=0.5*dt*dt*kEffectAccel;
		else if(gTextFX[i].effectFlags&kEffectMoveRight)
			baseX+=0.5*dt*dt*kEffectAccel;
		for(ch=1;ch<=gTextFX[i].text[0];ch++)
		{
			Ptr rawCH=GetSortedPackEntry(kPackcRLE,gTextFX[i].text[ch]-'A'+128,nil);
			float y=baseY;
			if(!rawCH){baseX+=exploZoom*kCharSize;continue;}
			Ptr theCH=rawCH+8;
			for(line=0;line<kCharSize;line++)
			{
				float x=baseX+((gTextFX[i].effectFlags&kEffectSinLines)?sin(line*kEffectSinFreq+dt*kEffectSinVelo)*kEffectSinAmp*exploZoom:0);
				if(exploZoom!=1)
					DrawZoomedCharLine(&theCH,(SInt32)(x*65536.0),y,(SInt32)(exploZoom*65536.0));
				else
					DrawCharLine(&theCH,(SInt32)(x),(SInt32)(y));
				y+=exploZoom;
			}
			baseX+=exploZoom*kCharSize;
		}
		if(dt*(gTextFX[i].effectFlags&kEffectTiny?2:1)>=kEffectTimeOut)
		{
			if(gFXCount>i+1)
				BlockMoveData(gTextFX+i+1,gTextFX+i,(gFXCount-i-1)*sizeof(tTextEffect));
			gFXCount--;
		}
	}
}

/* AI HQ glyph of the effect font, or NULL outside the HQ AI mode. */
static const tHQImage *GlyphHQ(int ch)
{
	return HQAssets_Load("chars",ch-'A'+128,0);
}

/* Draw an HQ glyph at scale screen pixels per original glyph pixel, with the
   effect's sine wobble; soft edges are alpha blended. */
static void DrawGlyphHQ(const tHQImage *hq,float x0,float y0,float scale,
	int sinLines,float dt)
{
	const tHQImage *image=hq;
	float texPerPixel=hq->scale/scale,step;
	int level=0,width,height,outW,outH,oy;
	while(level<3&&texPerPixel>=2.0f*(1<<level))
		level++;
	if(level)
		image=HQAssets_GetMipLevel(hq,level);
	width=image->width*image->scale;
	height=image->height*image->scale;
	step=(float)width/(hq->width*scale);
	outW=(int)(hq->width*scale+0.5f);
	outH=(int)(hq->height*scale+0.5f);
	for(oy=0;oy<outH;oy++)
	{
		int py=(int)y0+oy,sy=(int)(oy*step),startX,ox;
		tPixel *row;
		if(py<0||py>=gYSize-kInvLines)
			continue;
		if(sy>=height)
			sy=height-1;
		startX=(int)(x0+(sinLines?sin((oy/scale)*kEffectSinFreq+dt*kEffectSinVelo)*kEffectSinAmp*scale:0));
		row=(tPixel*)(gBaseAddr+py*gRowBytes);
		for(ox=0;ox<outW;ox++)
		{
			int px=startX+ox,sx=(int)(ox*step);
			UInt8 rgba[4];
			if(px<0||px>=gXSize)
				continue;
			if(sx>=width)
				sx=width-1;
			if(step<1)
			{
				/* Enlarged past the HQ resolution (the explode effect):
				   bilinear, so the letters stay smooth instead of blocky. */
				float fx=ox*step-0.5f,fy=oy*step-0.5f;
				int x0=fx<0?0:(int)fx,y0=fy<0?0:(int)fy;
				int x1=x0+1<width?x0+1:x0,y1=y0+1<height?y0+1:y0,c;
				int wx=(int)((fx-x0)*256),wy=(int)((fy-y0)*256);
				const UInt8 *p00=image->rgba+((size_t)y0*width+x0)*4;
				const UInt8 *p10=image->rgba+((size_t)y0*width+x1)*4;
				const UInt8 *p01=image->rgba+((size_t)y1*width+x0)*4;
				const UInt8 *p11=image->rgba+((size_t)y1*width+x1)*4;
				if(wx<0)wx=0;
				if(wy<0)wy=0;
				for(c=0;c<4;c++)
				{
					int top=p00[c]*(256-wx)+p10[c]*wx;
					int bottom=p01[c]*(256-wx)+p11[c]*wx;
					rgba[c]=(UInt8)((top*(256-wy)+bottom*wy)>>16);
				}
			}
			else
				memcpy(rgba,image->rgba+((size_t)sy*width+sx)*4,4);
			if(rgba[3])
			{
				tPixel color=PixelRGB(rgba[0],rgba[1],rgba[2]);
				row[px]=rgba[3]==255?color:BlendAlpha(color,row[px],rgba[3]);
			}
		}
	}
}

/* Original RLE glyph at scale > 1: repeat each glyph line over its rows. */
static void DrawGlyphLinesScaled(Ptr theCH,float x0,float y0,float scale,
	int sinLines,float dt)
{
	float y=y0;
	int line;
	for(line=0;line<kCharSize;line++)
	{
		float x=x0+(sinLines?sin(line*kEffectSinFreq+dt*kEffectSinVelo)*kEffectSinAmp*scale:0);
		int rows=(int)(y+scale)-(int)y,row;
		Ptr lineStart=theCH;
		if(rows<1)
			DrawZoomedCharLine((UInt8**)&theCH,(SInt32)(x*65536.0),-1,(SInt32)(scale*65536.0));
		for(row=0;row<rows;row++)
		{
			theCH=lineStart;
			DrawZoomedCharLine((UInt8**)&theCH,(SInt32)(x*65536.0),(int)y+row,(SInt32)(scale*65536.0));
		}
		y+=scale;
	}
}

void DrawTextFXZoomed(float xDrawStart,float yDrawStart,float zoom)
{
	int i,line,ch;
	int slot=(int)(gCurPlayer-gPlayers);
	/* Split screen has at most kTextFXViews views; a LAN computer with
	   more drivers only draws its own one. */
	int view=slot>=0?slot%kTextFXViews:0;
	/* Effect positions are in 640x480 screen units (world positions are
	   already in render pixels); glyphs grow with the render scale. */
	int renderScale=Platform_GetRenderScale();
	for(i=0;i<gFXCount;i++)
	{
		float dt=(gFrameCount-gTextFX[i].fxStartFrame)*kFrameDuration*(gTextFX[i].effectFlags&kEffectTiny?3:1);
		float exploZoom=(gTextFX[i].effectFlags&kEffectExplode?1+0.5*dt*dt*kExplAccel:1)*(gTextFX[i].effectFlags&kEffectTiny?0.25:1)*
			(gTextFX[i].effectFlags&kEffectSmall?0.5:1);
		float centerX=gTextFX[i].x*renderScale,centerY=gTextFX[i].y*renderScale;
		float scale=exploZoom*renderScale;
		int sinLines=(gTextFX[i].effectFlags&kEffectSinLines)!=0;
		float baseX,baseY;
		if(gTextFX[i].owner>=0&&gTextFX[i].owner!=slot)
			goto timeout;
		if(gTextFX[i].effectFlags&kEffectAbsPos)
		{
			/* World positions are fixed on screen the first time each view
			   draws them, so every split-screen camera gets its own spot. */
			if(!(gTextFX[i].viewPlaced&(1<<view)))
			{
				gTextFX[i].viewX[view]=(gTextFX[i].x-xDrawStart)/zoom;
				gTextFX[i].viewY[view]=(yDrawStart-gTextFX[i].y)/zoom;
				gTextFX[i].viewPlaced|=1<<view;
			}
			centerX=gTextFX[i].viewX[view];
			centerY=gTextFX[i].viewY[view];
		}
		baseX=centerX-gTextFX[i].text[0]*kCharSize*0.5*scale;
		baseY=centerY-kCharSize*0.5*scale;
		if(gTextFX[i].effectFlags&kEffectMoveUp)
			baseY-=0.5*dt*dt*kEffectAccel*renderScale;
		else if(gTextFX[i].effectFlags&kEffectMoveDown)
			baseY+=0.5*dt*dt*kEffectAccel*renderScale;
		if(gTextFX[i].effectFlags&kEffectMoveLeft)
			baseX-=0.5*dt*dt*kEffectAccel*renderScale;
		else if(gTextFX[i].effectFlags&kEffectMoveRight)
			baseX+=0.5*dt*dt*kEffectAccel*renderScale;
		for(ch=1;ch<=gTextFX[i].text[0];ch++)
		{
			Ptr rawCH=GetSortedPackEntry(gPrefs.hiColor?kPackcR16:kPackcRLE,gTextFX[i].text[ch]-'A'+128,nil);
			const tHQImage *hq=renderScale>1?GlyphHQ(gTextFX[i].text[ch]):NULL;
			float y=baseY;
			if(!rawCH){baseX+=scale*kCharSize;continue;}
			Ptr theCH=rawCH+8;
			if(renderScale>1)
			{
				if(hq)
					DrawGlyphHQ(hq,baseX,baseY,scale,sinLines,dt);
				else
					DrawGlyphLinesScaled(theCH,baseX,baseY,scale,sinLines,dt);
				baseX+=scale*kCharSize;
				continue;
			}
			for(line=0;line<kCharSize;line++)
			{
				float x=baseX+((gTextFX[i].effectFlags&kEffectSinLines)?sin(line*kEffectSinFreq+dt*kEffectSinVelo)*kEffectSinAmp*exploZoom:0);
				if(exploZoom!=1)
					DrawZoomedCharLine(&theCH,(SInt32)(x*65536.0),y,(SInt32)(exploZoom*65536.0));
				else
					DrawCharLine(&theCH,(SInt32)(x),(SInt32)(y));
				y+=exploZoom;
			}
			baseX+=exploZoom*kCharSize;
		}
	timeout:
		if(dt*(gTextFX[i].effectFlags&kEffectTiny?2:1)>=kEffectTimeOut)
		{
			if(gFXCount>i+1)
				BlockMoveData(gTextFX+i+1,gTextFX+i,(gFXCount-i-1)*sizeof(tTextEffect));
			gFXCount--;
			i--;
		}
	}
}

void SimpleDrawText(Str255 text,int xPos,int yPos)
{
	int ch,line;
	int renderScale=Platform_GetRenderScale();
	xPos*=renderScale;
	yPos*=renderScale;
	for(ch=1;ch<=text[0];ch++)
	{
		Ptr rawCH=GetSortedPackEntry(gPrefs.hiColor?kPackcR16:kPackcRLE,text[ch]-'A'+128,nil);
		const tHQImage *hq=renderScale>1?GlyphHQ(text[ch]):NULL;
		int y=yPos;
		if(!rawCH){xPos+=(kCharSize/2)*renderScale;continue;}
		if(hq)
		{
			DrawGlyphHQ(hq,xPos,yPos,0.5f*renderScale,0,0);
			xPos+=(kCharSize/2)*renderScale;
			continue;
		}
		Ptr theCH=rawCH+8;
		for(line=0;line<kCharSize/2;line++)
		{
			DrawZoomedCharLine(&theCH,xPos<<16,y,renderScale<<15);
			DrawZoomedCharLine(&theCH,xPos<<16,y,renderScale<<15);
			y+=renderScale;
		}
		xPos+=(kCharSize/2)*renderScale;
	}
}

void ClearTextFX()
{
	gFXCount=0;	
}
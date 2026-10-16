#ifndef __TEXTFX
#define __TEXTFX

#include "compat.h"

enum{
	kEffectExplode=1<<0,
	kEffectSinLines=1<<2,
	kEffectMoveUp=1<<3,
	kEffectMoveDown=1<<4,
	kEffectMoveLeft=1<<5,
	kEffectMoveRight=1<<6,
	kEffectTiny=1<<7,
	kEffectAbsPos=1<<8,
	kEffectSmall=1<<9		/* half size, normal duration */
};

#define kTextFXViews 4

typedef struct{
	SInt32 x,y;			
	UInt32 effectFlags;
	UInt32 fxStartFrame;
	Str31 text;
	/* set by NewTextEffect */
	SInt8 owner;                    /* driver slot that sees it, -1 = every view */
	UInt8 viewPlaced;               /* kEffectAbsPos: views that fixed a position */
	float viewX[kTextFXViews],viewY[kTextFXViews];
} tTextEffect;

void NewTextEffect(tTextEffect *);
void DrawTextFX(int,int);
void DrawTextFXZoomed(float,float,float);
void SimpleDrawText(Str255,int,int);
void MakeFXStringFromNumStr(Str31,Str31);
void ClearTextFX();

#endif
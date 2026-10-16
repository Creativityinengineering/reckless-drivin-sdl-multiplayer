#ifndef __PIXEL_H
#define __PIXEL_H

#include "compat.h"

/* Framebuffer pixel: 0xAARRGGBB with alpha always 0xFF, 8 bits per channel.
   The game's own art is RGB555 and goes through Pixel555(); the HQ assets
   keep their full 8-bit color. */
typedef UInt32 tPixel;

extern tPixel gPixel555[32768];

void Pixel_Init(void);

static inline tPixel Pixel555(UInt16 color)
{
	return gPixel555[color&0x7FFF];
}

static inline tPixel PixelRGB(int red,int green,int blue)
{
	return 0xFF000000u|((UInt32)red<<16)|((UInt32)green<<8)|(UInt32)blue;
}

/* 50% mix, the game's translucency. */
static inline tPixel BlendPixel(tPixel a,tPixel b)
{
	return 0xFF000000u|(((a&0xFEFEFEu)>>1)+((b&0xFEFEFEu)>>1));
}

/* Mix src over dst with coverage alpha (0..255). */
static inline tPixel BlendAlpha(tPixel src,tPixel dst,UInt32 alpha)
{
	UInt32 inverse=255-alpha;
	UInt32 redBlue=((src&0xFF00FFu)*alpha+(dst&0xFF00FFu)*inverse+0x800080u)>>8;
	UInt32 green=((src&0x00FF00u)*alpha+(dst&0x00FF00u)*inverse+0x008000u)>>8;
	return 0xFF000000u|(redBlue&0xFF00FFu)|(green&0x00FF00u);
}

/* Darken by shade/kLightValues (16 steps). */
static inline tPixel ShadePixel(int shade,tPixel a)
{
	UInt32 red=((a>>16)&0xFF)*shade>>4;
	UInt32 green=((a>>8)&0xFF)*shade>>4;
	UInt32 blue=(a&0xFF)*shade>>4;
	return 0xFF000000u|(red<<16)|(green<<8)|blue;
}

/* Darken a sprite texel, keeping its alpha. */
static inline tPixel ShadeTexel(int shade,tPixel a)
{
	return (a&0xFF000000u)|(ShadePixel(shade,a)&0x00FFFFFFu);
}

#endif

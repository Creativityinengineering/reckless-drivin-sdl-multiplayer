#ifndef __HQASSETS_H
#define __HQASSETS_H

#include "compat.h"

typedef struct tHQImage {
	UInt16 width;
	UInt16 height;
	UInt16 scale;
	const UInt8 *rgba;
	const struct tHQImage *mipLevels[3];
} tHQImage;

const tHQImage *HQAssets_Load(const char *category, int id, int frame);
const tHQImage *HQAssets_GetMipLevel(const tHQImage *image, int level);
void HQAssets_Release(const char *category, int id, int frame);
void HQAssets_Clear(void);

#endif

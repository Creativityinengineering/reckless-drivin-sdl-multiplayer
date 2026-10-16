#ifndef __PICT_H
#define __PICT_H

#include "compat.h"

int Pict_Decode16(const UInt8 *data, long dataSize, UInt16 *pixels,
                  int stride, int *width, int *height);

#endif

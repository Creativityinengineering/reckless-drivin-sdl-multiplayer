#ifndef __ASSETFILE_H
#define __ASSETFILE_H

#include <stddef.h>
#include "compat.h"

/* Reads a whole AI asset file into a new malloc'd buffer (free it), or
   returns NULL. Files compressed by tools/compress-ai-assets.py ("RDZ1",
   zlib) are unpacked; uncompressed files are returned as they are. */
UInt8 *AssetFile_Read(const char *path, size_t *size);

#endif

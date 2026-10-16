/*
 * assetfile.c - Reading AI asset files, compressed or not.
 *
 * The upscaled sprites and pictures are big RGBA images; stored raw they
 * took over 300 MB. tools/compress-ai-assets.py packs each file as
 *   "RDZ1", unpacked size (4 bytes), zlib stream, or
 *   "RDZ2", unpacked size (4), header length (2), row bytes (4), bytes per
 *           pixel (1), zlib stream of the pixels after the PNG-like average
 *           filter (each byte minus the mean of its left and upper
 *           neighbours), which packs photos about a third smaller.
 * All numbers are big-endian.
 */

#include "assetfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RD_HAVE_ZLIB
#include <zlib.h>
#endif

static size_t ReadBE(const UInt8 *data, int bytes)
{
	size_t value = 0;
	while (bytes--)
		value = (value << 8) | *data++;
	return value;
}

/* Undo the average filter, row by row, left to right. */
static void UnfilterAverage(UInt8 *pixels, size_t rows, size_t rowBytes, size_t bpp)
{
	size_t y, x;
	for (y = 1; y < rows; ++y) {
		UInt8 *row = pixels + y * rowBytes, *up = row - rowBytes;
		for (x = bpp; x < rowBytes; ++x)
			row[x] = (UInt8)(row[x] + ((row[x - bpp] + up[x]) >> 1));
	}
}

UInt8 *AssetFile_Read(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	long length;
	UInt8 *data;
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
		fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	data = (UInt8 *)malloc(length > 0 ? (size_t)length : 1);
	if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	if ((length >= 8 && memcmp(data, "RDZ1", 4) == 0) ||
		(length >= 15 && memcmp(data, "RDZ2", 4) == 0)) {
		int filtered = data[3] == '2';
		size_t rawSize = ReadBE(data + 4, 4), headerLength = 0, rowBytes = 0, bpp = 0;
		size_t streamStart = 8;
		if (filtered) {
			headerLength = ReadBE(data + 8, 2);
			rowBytes = ReadBE(data + 10, 4);
			bpp = data[14];
			streamStart = 15;
		}
#ifdef RD_HAVE_ZLIB
		{
			uLongf unpacked = (uLongf)rawSize;
			UInt8 *raw = (UInt8 *)malloc(rawSize ? rawSize : 1);
			if (!raw || uncompress(raw, &unpacked, data + streamStart,
					(uLong)((size_t)length - streamStart)) != Z_OK || unpacked != rawSize ||
				(filtered && (!rowBytes || !bpp || bpp > rowBytes || headerLength > rawSize ||
					(rawSize - headerLength) % rowBytes))) {
				fprintf(stderr, "[assets] Could not unpack %s\n", path);
				free(raw);
				free(data);
				return NULL;
			}
			if (filtered)
				UnfilterAverage(raw + headerLength, (rawSize - headerLength) / rowBytes,
					rowBytes, bpp);
			free(data);
			*size = rawSize;
			return raw;
		}
#else
		(void)rawSize; (void)headerLength; (void)rowBytes; (void)bpp; (void)streamStart;
		fprintf(stderr, "[assets] %s is compressed, but this build has no zlib\n", path);
		free(data);
		return NULL;
#endif
	}
	*size = (size_t)length;
	return data;
}

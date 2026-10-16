#include "pict.h"
#include <string.h>

#define PICT_MAX_WIDTH 640
#define PICT_MAX_HEIGHT 480

static UInt16 RdBE16(const UInt8 *p)
{
	return (UInt16)(((UInt16)p[0] << 8) | p[1]);
}

static void UnpackBitsRow16(const UInt8 *src, int srcLen, UInt8 *dst, int dstLen)
{
	const UInt8 *srcEnd = src + srcLen;
	UInt8 *dstEnd = dst + dstLen;

	while (src < srcEnd && dst + 1 < dstEnd) {
		int8_t n = (int8_t)*src++;
		if (n >= 0) {
			int count = (n + 1) * 2;
			while (count-- > 0 && src < srcEnd && dst < dstEnd)
				*dst++ = *src++;
		} else if (n != (int8_t)-128) {
			int count = 1 - (int)n;
			UInt8 b0 = (src < srcEnd) ? *src++ : 0;
			UInt8 b1 = (src < srcEnd) ? *src++ : 0;
			while (count-- > 0 && dst + 1 < dstEnd) {
				*dst++ = b0;
				*dst++ = b1;
			}
		}
	}
}

static void UnpackBitsRow8(const UInt8 *src, int srcLen, UInt8 *dst, int dstLen)
{
	const UInt8 *srcEnd = src + srcLen;
	UInt8 *dstEnd = dst + dstLen;

	while (src < srcEnd && dst < dstEnd) {
		int8_t n = (int8_t)*src++;
		if (n >= 0) {
			int count = n + 1;
			while (count-- > 0 && src < srcEnd && dst < dstEnd)
				*dst++ = *src++;
		} else if (n != (int8_t)-128) {
			int count = 1 - (int)n;
			UInt8 val = (src < srcEnd) ? *src++ : 0;
			while (count-- > 0 && dst < dstEnd)
				*dst++ = val;
		}
	}
}

int Pict_Decode16(const UInt8 *data, long dataSize, UInt16 *pixels,
                  int stride, int *width, int *height)
{
	long pos = -1;
	int opcode = 0;
	int boundsTop, boundsLeft, picWidth, picHeight, pixelSize = 16;
	UInt16 pmRowBytes;
	UInt16 clut[256] = {0};
	UInt8 rowBuf[PICT_MAX_WIDTH * 2];

	if (!data || !pixels || !width || !height || stride < PICT_MAX_WIDTH ||
		dataSize < 12)
		return 0;
	for (long i = 10; i < dataSize - 1; ++i) {
		if (data[i] == 0x00 && (data[i + 1] == 0x9A ||
			data[i + 1] == 0x98)) {
			opcode = data[i + 1];
			pos = i + 2;
			break;
		}
	}
	if (pos < 0)
		return 0;
	if (opcode == 0x9A)
		pos += 4;
	if (pos + 46 > dataSize)
		return 0;
	{
		UInt16 pmRowBytesRaw = RdBE16(data + pos);
		int isPixMap = (pmRowBytesRaw & 0x8000) != 0;
		pmRowBytes = pmRowBytesRaw & 0x3FFF;
		pos += 2;
		boundsTop = (int16_t)RdBE16(data + pos); pos += 2;
		boundsLeft = (int16_t)RdBE16(data + pos); pos += 2;
		picHeight = (int16_t)RdBE16(data + pos) - boundsTop; pos += 2;
		picWidth = (int16_t)RdBE16(data + pos) - boundsLeft; pos += 2;
		if (isPixMap && pos + 20 <= dataSize)
			pixelSize = RdBE16(data + pos + 18);
		pos += 36;
		if (pixelSize <= 8 && isPixMap) {
			if (pos + 8 > dataSize)
				return 0;
			pos += 6;
			{
				UInt16 ctSize = RdBE16(data + pos);
				int numColors = (int)ctSize + 1;
				pos += 2;
				if (numColors > 256)
					numColors = 256;
				for (int c = 0; c < numColors && pos + 8 <= dataSize; ++c) {
					UInt16 idx = RdBE16(data + pos); pos += 2;
					UInt16 r = RdBE16(data + pos); pos += 2;
					UInt16 g = RdBE16(data + pos); pos += 2;
					UInt16 b = RdBE16(data + pos); pos += 2;
					UInt16 color = (UInt16)(0x8000 |
						(((r >> 11) & 31) << 10) |
						(((g >> 11) & 31) << 5) | ((b >> 11) & 31));
					clut[idx < 256 ? idx : c] = color;
				}
			}
		}
	}
	pos += 18;
	if (picWidth <= 0 || picHeight <= 0 || picWidth > PICT_MAX_WIDTH ||
		picHeight > PICT_MAX_HEIGHT || !pmRowBytes ||
		pmRowBytes > (int)sizeof(rowBuf) ||
		(pixelSize != 8 && pixelSize != 16))
		return 0;
	for (int y = 0; y < picHeight; ++y) {
		int packedLen;
		if (pos >= dataSize)
			return 0;
		if (pmRowBytes > 250) {
			if (pos + 2 > dataSize)
				return 0;
			packedLen = RdBE16(data + pos);
			pos += 2;
		} else {
			packedLen = data[pos++];
		}
		if (packedLen < 0 || pos + packedLen > dataSize)
			return 0;
		memset(rowBuf, 0, pmRowBytes);
		if (pixelSize == 16) {
			UnpackBitsRow16(data + pos, packedLen, rowBuf, pmRowBytes);
			for (int x = 0; x < picWidth; ++x)
				pixels[y * stride + x] =
					(UInt16)(((UInt16)rowBuf[x * 2] << 8) |
						rowBuf[x * 2 + 1]);
		} else {
			UnpackBitsRow8(data + pos, packedLen, rowBuf, pmRowBytes);
			for (int x = 0; x < picWidth; ++x)
				pixels[y * stride + x] = clut[rowBuf[x]];
		}
		pos += packedLen;
	}
	*width = picWidth;
	*height = picHeight;
	return 1;
}

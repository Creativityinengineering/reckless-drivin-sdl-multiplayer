#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packs.h"
#include "pict.h"
#include "lzrw.h"
#include "resources.h"

#define kTextureTileSize 128
#define kTextureTileBytes (kTextureTileSize * kTextureTileSize * 2)
#define kMaxImageWidth 2048
#define kMaxImageHeight 2048
#define kScreenWidth 640
#define kScreenHeight 480

UInt32 gKey = 0x1E42A71F;
int gRegistered = 1;
short gLevelResFile = 0;
short gAppResFile = 0;

static int CopyFile(FILE *source, FILE *destination, size_t bytes)
{
	UInt8 buffer[65536];
	while (bytes) {
		size_t amount = bytes < sizeof(buffer) ? bytes : sizeof(buffer);
		if (fread(buffer, 1, amount, source) != amount ||
			fwrite(buffer, 1, amount, destination) != amount)
			return -1;
		bytes -= amount;
	}
	return 0;
}

static UInt16 ReadBE16(const UInt8 *p)
{
	return (UInt16)(((UInt16)p[0] << 8) | p[1]);
}

static UInt32 ReadBE32(const UInt8 *p)
{
	return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
		((UInt32)p[2] << 8) | p[3];
}

static int WriteImage(const char *directory, const char *name,
	const UInt16 *pixels, const UInt8 *alpha, int width, int height,
	FILE *manifest, int id, int frame, int logicalWidth, int logicalHeight)
{
	char colorPath[1024], maskPath[1024];
	FILE *color, *mask;
	size_t count;
	if (width <= 0 || height <= 0 || width > kMaxImageWidth ||
		height > kMaxImageHeight)
		return 0;
	snprintf(colorPath, sizeof(colorPath), "%s/%s.ppm", directory, name);
	snprintf(maskPath, sizeof(maskPath), "%s/%s.pgm", directory, name);
	color = fopen(colorPath, "wb");
	mask = fopen(maskPath, "wb");
	if (!color || !mask) {
		fprintf(stderr, "Cannot create source image %s\n", name);
		if (color) fclose(color);
		if (mask) fclose(mask);
		return -1;
	}
	fprintf(color, "P6\n%d %d\n255\n", width, height);
	fprintf(mask, "P5\n%d %d\n255\n", width, height);
	count = (size_t)width * height;
	for (size_t i = 0; i < count; ++i) {
		UInt16 pixel = pixels[i];
		UInt8 rgb[3] = {
			(UInt8)(((pixel >> 10) & 31) * 255 / 31),
			(UInt8)(((pixel >> 5) & 31) * 255 / 31),
			(UInt8)((pixel & 31) * 255 / 31)
		};
		UInt8 opacity = alpha ? alpha[i] : 255;
		if (fwrite(rgb, 1, sizeof(rgb), color) != sizeof(rgb) ||
			fwrite(&opacity, 1, 1, mask) != 1) {
			fprintf(stderr, "Cannot write source image %s\n", name);
			fclose(color);
			fclose(mask);
			return -1;
		}
	}
	{
		int colorError = fclose(color) != 0;
		int maskError = fclose(mask) != 0;
		if (colorError || maskError) {
			fprintf(stderr, "Cannot finish source image %s\n", name);
			return -1;
		}
	}
	if (fprintf(manifest, "%d %d %d %d\n", id, frame,
		logicalWidth, logicalHeight) < 0) {
		fprintf(stderr, "Cannot write asset manifest\n");
		return -1;
	}
	return 1;
}

static int PackEntryAt(int packNum, int index, int *id, UInt8 **data, int *size)
{
	UInt8 *pack;
	int count;
	UInt32 offset, end;
	long packSize;
	if (!gPacks[packNum])
		return 0;
	pack = (UInt8 *)*gPacks[packNum];
	count = ReadBE16(pack);
	if (index < 0 || index >= count)
		return 0;
	packSize = Resources_GetSize(gPacks[packNum]);
	offset = ReadBE32(pack + 8 + index * 8 + 4);
	if (index + 1 < count)
		end = ReadBE32(pack + 8 + (index + 1) * 8 + 4);
	else
		end = (UInt32)packSize;
	if (offset > end || end > (UInt32)packSize)
		return 0;
	*id = (int)(SInt16)ReadBE16(pack + 8 + index * 8);
	*data = pack + offset;
	*size = (int)(end - offset);
	return 1;
}

static int ExportTextures(const char *sourceRoot)
{
	char directory[1024], manifestPath[1024], name[64];
	FILE *manifest;
	int exported = 0;
	snprintf(directory, sizeof(directory), "%s/textures", sourceRoot);
	snprintf(manifestPath, sizeof(manifestPath), "%s/MANIFEST", directory);
	manifest = fopen(manifestPath, "w");
	if (!manifest)
		return 0;
	LoadPack(kPackTx16);
	for (int i = 0; i < NumPackEntries(kPackTx16); ++i) {
		int id, size;
		UInt8 *data;
		if (!PackEntryAt(kPackTx16, i, &id, &data, &size))
			continue;
		int textureWidth;
		int tileBytes;
		if (size > 0 && size % kTextureTileBytes == 0) {
			textureWidth = 128;
			tileBytes = kTextureTileBytes;
		} else if (size > 0 && size % (16 * 128 * 2) == 0) {
			textureWidth = 16;
			tileBytes = 16 * 128 * 2;
		} else {
			fprintf(stderr, "Skipping malformed texture pack entry %d (%d bytes)\n",
				id, size);
			continue;
		}
		for (int tile = 0; tile < size / tileBytes; ++tile) {
			UInt16 pixels[kTextureTileSize * kTextureTileSize];
			UInt16 *tiled;
			UInt8 *source = data + tile * tileBytes;
			for (int p = 0; p < textureWidth * kTextureTileSize; ++p)
				pixels[p] = ReadBE16(source + p * 2);
			tiled=(UInt16*)malloc(3*textureWidth*3*kTextureTileSize*
				sizeof(*tiled));
			if(!tiled)
			{
				fprintf(stderr,"Out of memory tiling texture %d_%d\n",id,tile);
				fclose(manifest);
				return 0;
			}
			for(int y=0;y<3*kTextureTileSize;y++)
				for(int x=0;x<3*textureWidth;x++)
					tiled[y*3*textureWidth+x]=
						pixels[(y%kTextureTileSize)*textureWidth+(x%textureWidth)];
			snprintf(name, sizeof(name), "%d_%d", id, tile);
			int result = WriteImage(directory, name, tiled, NULL,
				3*textureWidth, 3*kTextureTileSize, manifest, id, tile,
				textureWidth,kTextureTileSize);
			free(tiled);
			if (result < 0) {
				fclose(manifest);
				return 0;
			}
			exported += result;
		}
	}
	fclose(manifest);
	return exported;
}

static int ExportRLEImage(const char *directory, FILE *manifest, int id,
	const UInt8 *data, int size)
{
	UInt16 *pixels = NULL;
	UInt8 *alpha = NULL;
	char name[64];
	int width, height, x = 0, y = 0, pos = 8, done = 0, malformed = 0, result;
	if (size < 12)
		return 0;
	height = (int)(SInt16)ReadBE16(data + 4) -
		(int)(SInt16)ReadBE16(data);
	width = (int)(SInt16)ReadBE16(data + 6) -
		(int)(SInt16)ReadBE16(data + 2);
	if (width <= 0 || height <= 0 || width > kMaxImageWidth ||
		height > kMaxImageHeight)
		return 0;
	pixels = (UInt16 *)calloc((size_t)width * height, sizeof(*pixels));
	alpha = (UInt8 *)calloc((size_t)width * height, 1);
	if (!pixels || !alpha) {
		fprintf(stderr, "Out of memory decoding RLE image %d\n", id);
		free(pixels);
		free(alpha);
		return -1;
	}
	while (!done && pos + 4 <= size) {
		UInt8 type = data[pos];
		UInt32 token = ((UInt32)data[pos + 1] << 16) |
			((UInt32)data[pos + 2] << 8) | data[pos + 3];
		pos += 4;
		switch (type) {
		case 0:
			done = 1;
			break;
		case 1:
			++y;
			x = 0;
			break;
		case 2:
			if (token > (UInt32)(width - x)) {
				fprintf(stderr, "Invalid RLE run in image %d\n", id);
				done = 1;
				malformed = 1;
				break;
			}
			if (pos + token * 2 > (UInt32)size ||
				pos + ((token * 2 + 3) & ~3u) > (UInt32)size) {
				fprintf(stderr, "Truncated RLE pixels in image %d\n", id);
				done = 1;
				malformed = 1;
				break;
			}
			for (UInt32 p = 0; p < token && y < height; ++p) {
				size_t at = (size_t)y * width + x++;
				pixels[at] = ReadBE16(data + pos + p * 2);
				alpha[at] = 255;
			}
			pos += (int)((token * 2 + 3) & ~3u);
			break;
		case 3:
			if (token > (UInt32)(width - x)) {
				fprintf(stderr, "Invalid RLE skip in image %d\n", id);
				done = 1;
				malformed = 1;
				break;
			}
			x += (int)token;
			break;
		default:
			done = 1;
			break;
		}
		if (x > width || y > height || pos > size) {
			done = 1;
			malformed = 1;
		}
	}
	if(!done)
		malformed=1;
	if(malformed)
	{
		free(pixels);
		free(alpha);
		return 0;
	}
	snprintf(name, sizeof(name), "%d_0", id);
	result = WriteImage(directory, name, pixels, alpha, width, height,
		manifest, id, 0,width,height);
	free(pixels);
	free(alpha);
	return result;
}

/* RLE images of one pack: HUD art (sR16) or the text effect font (cR16). */
static int ExportRLEPack(const char *sourceRoot, int packNum, const char *category)
{
	char directory[1024], manifestPath[1024];
	FILE *manifest;
	int exported = 0;
	snprintf(directory, sizeof(directory), "%s/%s", sourceRoot, category);
	snprintf(manifestPath, sizeof(manifestPath), "%s/MANIFEST", directory);
	manifest = fopen(manifestPath, "w");
	if (!manifest)
		return 0;
	LoadPack(packNum);
	for (int i = 0; i < NumPackEntries(packNum); ++i) {
		int id, size;
		UInt8 *data;
		if (!PackEntryAt(packNum, i, &id, &data, &size))
			continue;
		int result = ExportRLEImage(directory, manifest, id, data, size);
		if (result < 0) {
			fclose(manifest);
			return 0;
		}
		exported += result;
	}
	fclose(manifest);
	return exported;
}

static int ExportPictures(const char *sourceRoot)
{
	char directory[1024], manifestPath[1024], name[64];
	FILE *manifest;
	int exported = 0;
	snprintf(directory, sizeof(directory), "%s/pics", sourceRoot);
	snprintf(manifestPath, sizeof(manifestPath), "%s/MANIFEST", directory);
	manifest = fopen(manifestPath, "w");
	if (!manifest)
		return 0;
	for (int id = 1000; id <= 1009; ++id) {
		Handle pic = Resources_Get(MakeFourCC("PPic"), id);
		if (pic) {
			long compressedSize = Resources_GetSize(pic);
			UInt16 pixels[kScreenWidth * kScreenHeight];
			int width = 0, height = 0;
			LZRWDecodeHandle(&pic, compressedSize);
			long decodedSize = Resources_GetSize(pic);
			if (decodedSize > 0 && Pict_Decode16((const UInt8 *)*pic,
				decodedSize, pixels, kScreenWidth, &width, &height)) {
				snprintf(name, sizeof(name), "%d_0", id);
				int result = WriteImage(directory, name, pixels, NULL,
					width, height, manifest, id, 0,width,height);
				if (result < 0) {
					Resources_Release(pic);
					fclose(manifest);
					return 0;
				}
				exported += result;
			} else {
				fprintf(stderr, "Could not decode PICT resource %d for HQ assets\n", id);
			}
			Resources_Release(pic);
		}
	}
	fclose(manifest);
	return exported;
}

static int PackCategory(const char *sourceRoot, const char *rawRoot,
	const char *outputRoot, const char *category)
{
	char manifestPath[1024], rawPath[1024], outputPath[1024], name[64];
	FILE *manifest;
	int packed = 0;
	snprintf(manifestPath, sizeof(manifestPath), "%s/%s/MANIFEST",
		sourceRoot, category);
	manifest = fopen(manifestPath, "r");
	if (!manifest) {
		fprintf(stderr, "Missing asset manifest: %s\n", manifestPath);
		return 0;
	}
	{
		char line[128];
		while (fgets(line, sizeof(line), manifest)) {
		int id, frame, width, height;
		char extra;
		FILE *raw, *output;
		UInt8 header[12] = {'R','D','H','Q',0,0,0,0,0,4,0,0};
		size_t pixels, bytes;
		if (sscanf(line, "%d %d %d %d %c", &id, &frame,
			&width, &height, &extra) != 4) {
			fprintf(stderr, "Malformed asset manifest: %s\n", manifestPath);
			fclose(manifest);
			return 0;
		}
		if (width <= 0 || height <= 0 || width > UINT16_MAX / 4 ||
			height > UINT16_MAX / 4) {
			fprintf(stderr, "Invalid source dimensions for asset %d_%d\n", id, frame);
			fclose(manifest);
			return 0;
		}
		snprintf(name, sizeof(name), "%d_%d.rgba", id, frame);
		snprintf(rawPath, sizeof(rawPath), "%s/%s/%s", rawRoot, category, name);
		snprintf(name, sizeof(name), "%d_%d.hqa", id, frame);
		snprintf(outputPath, sizeof(outputPath), "%s/%s/%s",
			outputRoot, category, name);
		raw = fopen(rawPath, "rb");
		if (!raw) {
			fprintf(stderr, "Missing upscaled pixels: %s\n", rawPath);
			fclose(manifest);
			return 0;
		}
		pixels = (size_t)width * 4 * (size_t)height * 4;
		bytes = pixels * 4;
		if (bytes > 64u * 1024u * 1024u) {
			fprintf(stderr, "HQ asset dimensions are too large for %d_%d\n",
				id, frame);
			fclose(raw);
			fclose(manifest);
			return 0;
		}
		if (fseek(raw, 0, SEEK_END) != 0 || ftell(raw) != (long)bytes ||
			fseek(raw, 0, SEEK_SET) != 0) {
			fprintf(stderr, "Unexpected pixel size in %s\n", rawPath);
			fclose(raw);
			fclose(manifest);
			return 0;
		}
		header[4] = (UInt8)(width >> 8);
		header[5] = (UInt8)width;
		header[6] = (UInt8)(height >> 8);
		header[7] = (UInt8)height;
		output = fopen(outputPath, "wb");
		if (!output) {
			fprintf(stderr, "Cannot write HQ asset %s\n", outputPath);
			fclose(raw);
			fclose(manifest);
			return 0;
		}
		{
			int writeError = fwrite(header, 1, sizeof(header), output) !=
				sizeof(header) || CopyFile(raw, output, bytes) != 0;
			if (fclose(output) != 0)
				writeError = 1;
			if (writeError) {
				fprintf(stderr, "Cannot write HQ asset %s\n", outputPath);
				fclose(raw);
				fclose(manifest);
				return 0;
			}
		}
		fclose(raw);
		++packed;
		}
	}
	if (ferror(manifest)) {
		fprintf(stderr, "Cannot read asset manifest %s\n", manifestPath);
		fclose(manifest);
		return 0;
	}
	fclose(manifest);
	return packed;
}

int main(int argc, char **argv)
{
	if (argc == 4 && strcmp(argv[1], "export") == 0) {
		int textures, rle, pics, chars;
		if (!Resources_Init(argv[2]))
			return 1;
		textures = ExportTextures(argv[3]);
		rle = ExportRLEPack(argv[3], kPacksR16, "rle");
		chars = ExportRLEPack(argv[3], kPackcR16, "chars");
		pics = ExportPictures(argv[3]);
		Resources_Shutdown();
		printf("Exported %d road/track textures, %d RLE/HUD images, %d menu pictures, %d font glyphs\n",
			textures, rle, pics, chars);
		return textures > 0 && rle > 0 && pics > 0 ? 0 : 1;
	}
	if (argc == 5 && strcmp(argv[1], "pack") == 0) {
		int textures = PackCategory(argv[2], argv[3], argv[4], "textures");
		int rle = PackCategory(argv[2], argv[3], argv[4], "rle");
		int pics = PackCategory(argv[2], argv[3], argv[4], "pics");
		int chars = PackCategory(argv[2], argv[3], argv[4], "chars");
		printf("Packed %d road/track textures, %d RLE/HUD images, %d menu pictures, %d font glyphs\n",
			textures, rle, pics, chars);
		return textures > 0 && rle > 0 && pics > 0 ? 0 : 1;
	}
	fprintf(stderr,
		"Usage:\n  %s export <Data> <source-directory>\n"
		"  %s pack <source-directory> <raw-directory> <output-directory>\n",
		argv[0], argv[0]);
	return 2;
}

#include "hqassets.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "preferences.h"
#include "assetfile.h"

typedef struct tHQAsset {
	char category[16];
	int id;
	int frame;
	tHQImage image;
	UInt8 *pixels;
	tHQImage mipImages[3];
	UInt8 *mipPixels[3];
	int mipCount;
	struct tHQAsset *next;
} tHQAsset;

static tHQAsset *sAssets;

static void FreeMipLevels(tHQAsset *asset)
{
	int i;
	for (i = 0; i < 3; i++) {
		free(asset->mipPixels[i]);
		asset->mipPixels[i] = NULL;
		memset(&asset->mipImages[i], 0, sizeof(asset->mipImages[i]));
		asset->image.mipLevels[i] = NULL;
	}
	asset->mipCount = 0;
}

const tHQImage *HQAssets_GetMipLevel(const tHQImage *image, int level)
{
	tHQAsset *asset;
	if (!image || level < 1 || level > 3)
		return image;
	if (image->mipLevels[level - 1])
		return image->mipLevels[level - 1];
	for (asset = sAssets; asset; asset = asset->next)
		if (&asset->image == image)
			break;
	if (!asset)
		return image;
	while (asset->mipCount < level) {
		int nextLevel = asset->mipCount;
		const tHQImage *source = nextLevel == 0 ? &asset->image :
			&asset->mipImages[nextLevel - 1];
		tHQImage *destination = &asset->mipImages[nextLevel];
		UInt8 *pixels;
		UInt32 sourceWidth, sourceHeight;
		UInt32 x, y, channel;
		size_t pixelCount;
		sourceWidth = nextLevel == 0 ?
			(UInt32)source->width * source->scale : source->width;
		sourceHeight = nextLevel == 0 ?
			(UInt32)source->height * source->scale : source->height;
		if (sourceWidth < 2 || sourceHeight < 2)
			break;
		destination->width = (UInt16)(sourceWidth / 2);
		destination->height = (UInt16)(sourceHeight / 2);
		destination->scale = 1;
		pixelCount = (size_t)destination->width * destination->height;
		if (pixelCount > SIZE_MAX / 4)
			break;
		pixels = (UInt8 *)malloc(pixelCount * 4);
		if (!pixels)
			break;
		for (y = 0; y < destination->height; y++)
			for (x = 0; x < destination->width; x++)
				for (channel = 0; channel < 4; channel++) {
					size_t top = (size_t)(y * 2) * sourceWidth + x * 2;
					size_t bottom = top + sourceWidth;
					unsigned int sum =
						source->rgba[top * 4 + channel] +
						source->rgba[(top + 1) * 4 + channel] +
						source->rgba[bottom * 4 + channel] +
						source->rgba[(bottom + 1) * 4 + channel];
					pixels[((size_t)y * destination->width + x) * 4 +
						channel] = (UInt8)((sum + 2) / 4);
				}
		asset->mipPixels[nextLevel] = pixels;
		destination->rgba = pixels;
		asset->image.mipLevels[nextLevel] = destination;
		asset->mipCount++;
	}
	return level <= asset->mipCount ? &asset->mipImages[level - 1] : image;
}

static const tHQImage *RememberMissing(const char *category, int id, int frame)
{
	tHQAsset *asset=(tHQAsset*)calloc(1,sizeof(*asset));
	if(!asset)
		return NULL;
	snprintf(asset->category,sizeof(asset->category),"%s",category);
	asset->id=id;
	asset->frame=frame;
	asset->next=sAssets;
	sAssets=asset;
	return NULL;
}

static int AssetPath(char *path, size_t pathSize, const char *category,
	int id, int frame)
{
	char *basePath;
	int written;
	if (!category || strlen(category) >= sizeof(((tHQAsset *)0)->category))
		return 0;
	basePath = SDL_GetBasePath();
	if (!basePath)
		return 0;
	written = snprintf(path, pathSize, "%s/../Resources/AIAssets/%s/%d_%d.hqa",
		basePath, category, id, frame);
	if (written > 0 && (size_t)written < pathSize) {
		FILE *file = fopen(path, "rb");
		if (file) {
			fclose(file);
			SDL_free(basePath);
			return 1;
		}
	}
	written = snprintf(path, pathSize, "%s/AIAssets/%s/%d_%d.hqa",
		basePath, category, id, frame);
	if (written > 0 && (size_t)written < pathSize) {
		FILE *file = fopen(path, "rb");
		if (file) {
			fclose(file);
			SDL_free(basePath);
			return 1;
		}
	}
	written = snprintf(path, pathSize, "%s/../../../AIAssets/%s/%d_%d.hqa",
		basePath, category, id, frame);
	SDL_free(basePath);
	if (written <= 0 || (size_t)written >= pathSize)
		return 0;
	{
		FILE *file = fopen(path, "rb");
		if (file) {
			fclose(file);
			return 1;
		}
	}
	return 0;
}

const tHQImage *HQAssets_Load(const char *category, int id, int frame)
{
	tHQAsset *asset;
	char path[1024];
	UInt8 header[12], *data;
	size_t pixelBytes, dataSize;
	if (gPrefs.unused[6] != 1 ||
		Platform_GetRenderScale() < PLATFORM_HQ_RENDER_SCALE)
		return NULL;
	for (asset = sAssets; asset; asset = asset->next)
		if (asset->id == id && asset->frame == frame &&
			strcmp(asset->category, category) == 0)
			return asset->image.scale ? &asset->image : NULL;
	if (!AssetPath(path, sizeof(path), category, id, frame))
		return RememberMissing(category,id,frame);
	data = AssetFile_Read(path, &dataSize);
	if (!data)
		return NULL;
	if (dataSize < sizeof(header) || memcmp(data, "RDHQ", 4) != 0 ||
		((UInt16)data[8] << 8 | data[9]) != 4) {
		fprintf(stderr, "[hqassets] Invalid HQ asset header: %s\n", path);
		free(data);
		return NULL;
	}
	memcpy(header, data, sizeof(header));
	{
		UInt16 width = (UInt16)(((UInt16)header[4] << 8) | header[5]);
		UInt16 height = (UInt16)(((UInt16)header[6] << 8) | header[7]);
		if (!width || !height || width > 2048 || height > 2048) {
			fprintf(stderr, "[hqassets] Invalid HQ asset dimensions: %s\n", path);
			free(data);
			return NULL;
		}
		pixelBytes = (size_t)width * 4 * (size_t)height * 4 * 4;
		if (pixelBytes > 64u * 1024u * 1024u || dataSize != sizeof(header) + pixelBytes) {
			fprintf(stderr, "[hqassets] Invalid or unreadable HQ asset: %s\n", path);
			free(data);
			return NULL;
		}
		asset = (tHQAsset *)calloc(1, sizeof(*asset));
		if (!asset) {
			fprintf(stderr, "[hqassets] Could not allocate asset record\n");
			free(data);
			return NULL;
		}
		/* The pixels stay in the file buffer, after the header. */
		asset->pixels = data;
		asset->image.width = width;
		asset->image.height = height;
		asset->image.scale = 4;
		asset->image.rgba = data + sizeof(header);
	}
	snprintf(asset->category, sizeof(asset->category), "%s", category);
	asset->id = id;
	asset->frame = frame;
	asset->next = sAssets;
	sAssets = asset;
	return &asset->image;
}

void HQAssets_Release(const char *category, int id, int frame)
{
	tHQAsset **link=&sAssets;
	while(*link)
	{
		tHQAsset *asset=*link;
		if(asset->id==id&&asset->frame==frame&&
			strcmp(asset->category,category)==0)
		{
			*link=asset->next;
			FreeMipLevels(asset);
			free(asset->pixels);
			free(asset);
			return;
		}
		link=&asset->next;
	}
}

void HQAssets_Clear(void)
{
	while (sAssets) {
		tHQAsset *asset = sAssets;
		sAssets = asset->next;
		FreeMipLevels(asset);
		free(asset->pixels);
		free(asset);
	}
}

#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packs.h"
#include "register.h"
#include "resources.h"

#define FIRST_SPRITE_ID 128
#define SPRITE_COUNT 300
#define UPSCALE_FACTOR 4

UInt32 gKey = 0x1E42A71F;
int gRegistered = 1;
short gLevelResFile = 0;
short gAppResFile = 0;

static UInt16 ReadBE16(const UInt8 *bytes)
{
    return (UInt16)(((UInt16)bytes[0] << 8) | bytes[1]);
}

static void WriteBE16(FILE *file, UInt16 value)
{
    fputc((value >> 8) & 0xff, file);
    fputc(value & 0xff, file);
}

static int WriteSpriteSource(const char *directory, int id, const UInt8 *data,
    int size)
{
    char colorPath[1024];
    char maskPath[1024];
    FILE *colorFile;
    FILE *maskFile;
    UInt16 width, height, mask;
    size_t pixels, i;

    if (size < 8) return 0;
    width = ReadBE16(data);
    height = ReadBE16(data + 2);
    if (!width || !height) return 0;
    pixels = (size_t)width * height;
    if (pixels > (size_t)(size - 8) / 2) {
        fprintf(stderr, "Skipping malformed sprite %d\n", id);
        return 0;
    }
    mask = ReadBE16(data + 8);
    snprintf(colorPath, sizeof(colorPath), "%s/%d.ppm", directory, id);
    snprintf(maskPath, sizeof(maskPath), "%s/%d.pgm", directory, id);
    colorFile = fopen(colorPath, "wb");
    maskFile = fopen(maskPath, "wb");
    if (!colorFile || !maskFile) {
        fprintf(stderr, "Cannot create source files for sprite %d\n", id);
        if (colorFile) fclose(colorFile);
        if (maskFile) fclose(maskFile);
        return -1;
    }
    fprintf(colorFile, "P6\n%u %u\n255\n", width, height);
    fprintf(maskFile, "P5\n%u %u\n255\n", width, height);

    {
        UInt8 *rgb = (UInt8 *)malloc(pixels * 3);
        UInt8 *alpha = (UInt8 *)malloc(pixels);
        UInt8 *filled = (UInt8 *)malloc(pixels);
        int pass;
        if (!rgb || !alpha || !filled) {
            free(rgb); free(alpha); free(filled);
            fclose(colorFile); fclose(maskFile);
            return -1;
        }
        for (i = 0; i < pixels; ++i) {
            UInt16 pixel = ReadBE16(data + 8 + i * 2);
            rgb[i * 3] = (UInt8)(((pixel >> 10) & 31) * 255 / 31);
            rgb[i * 3 + 1] = (UInt8)(((pixel >> 5) & 31) * 255 / 31);
            rgb[i * 3 + 2] = (UInt8)((pixel & 31) * 255 / 31);
            alpha[i] = pixel == mask ? 0 : 255;
            filled[i] = alpha[i] != 0;
        }
        /* Give clear texels the color of their opaque neighbours, so the
           upscaler does not blend the key color into soft sprite edges. */
        for (pass = 0; pass < 8; ++pass) {
            int changed = 0;
            UInt8 *next = (UInt8 *)malloc(pixels);
            if (!next)
                break;
            memcpy(next, filled, pixels);
            for (i = 0; i < pixels; ++i) {
                int x = (int)(i % width), y = (int)(i / width), dx, dy, count = 0;
                int sum[3] = {0, 0, 0};
                if (filled[i])
                    continue;
                for (dy = -1; dy <= 1; ++dy)
                    for (dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        size_t n;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                            continue;
                        n = (size_t)ny * width + nx;
                        if (!filled[n])
                            continue;
                        sum[0] += rgb[n * 3];
                        sum[1] += rgb[n * 3 + 1];
                        sum[2] += rgb[n * 3 + 2];
                        count++;
                    }
                if (count) {
                    rgb[i * 3] = (UInt8)(sum[0] / count);
                    rgb[i * 3 + 1] = (UInt8)(sum[1] / count);
                    rgb[i * 3 + 2] = (UInt8)(sum[2] / count);
                    next[i] = 1;
                    changed = 1;
                }
            }
            memcpy(filled, next, pixels);
            free(next);
            if (!changed)
                break;
        }
        fwrite(rgb, 1, pixels * 3, colorFile);
        fwrite(alpha, 1, pixels, maskFile);
        free(rgb);
        free(alpha);
        free(filled);
    }
    {
        int colorError = fclose(colorFile) != 0;
        int maskError = fclose(maskFile) != 0;
        if (colorError || maskError) {
            fprintf(stderr, "Cannot finish source files for sprite %d\n", id);
            return -1;
        }
    }
    return 1;
}

static int ExportSprites(const char *dataPath, const char *sourceDirectory)
{
    int id, exported = 0;
    if (!Resources_Init(dataPath)) return 0;
    LoadPack(kPackSp16);
    if (!gPacks[kPackSp16]) {
        fprintf(stderr, "Could not load the 16-bit sprite pack\n");
        Resources_Shutdown();
        return 0;
    }
    for (id = FIRST_SPRITE_ID; id < FIRST_SPRITE_ID + SPRITE_COUNT; ++id) {
        int size = 0;
        Ptr data = GetUnsortedPackEntry(kPackSp16, id, &size);
        if (data) {
            int result = WriteSpriteSource(sourceDirectory, id, data, size);
            if (result < 0) {
                UnloadPack(kPackSp16);
                Resources_Shutdown();
                return 0;
            }
            exported += result;
        }
    }
    UnloadPack(kPackSp16);
    Resources_Shutdown();
    printf("Exported %d source sprites\n", exported);
    return exported > 0;
}

static int IsPowerOfTwo(UInt16 value)
{
    return value && !(value & (value - 1));
}

static int PackUpscaledSprites(const char *dataPath, const char *rawDirectory,
    const char *outputDirectory)
{
    int id, packed = 0;
    char rawPath[1024];
    char spritePath[1024];
    char manifestPath[1024];
    FILE *manifest;

    if (!Resources_Init(dataPath)) return 0;
    LoadPack(kPackSp16);
    if (!gPacks[kPackSp16]) {
        fprintf(stderr, "Could not load the 16-bit sprite pack\n");
        Resources_Shutdown();
        return 0;
    }

    snprintf(manifestPath, sizeof(manifestPath), "%s/AI_SPRITES", outputDirectory);
    manifest = fopen(manifestPath, "w");
    if (!manifest) {
        fprintf(stderr, "Cannot create AI sprite manifest in %s\n", outputDirectory);
        UnloadPack(kPackSp16);
        Resources_Shutdown();
        return 0;
    }

    for (id = FIRST_SPRITE_ID; id < FIRST_SPRITE_ID + SPRITE_COUNT; ++id) {
        int sourceSize = 0;
        Ptr source = GetUnsortedPackEntry(kPackSp16, id, &sourceSize);
        UInt16 width, height;
        UInt8 *rgba;
        size_t pixelCount, rawBytes;
        FILE *rawFile;
        FILE *spriteFile;

        if (!source || sourceSize < 8) continue;
        width = ReadBE16(source);
        height = ReadBE16(source + 2);
        if (!width || !height) continue;
        if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height) ||
            width > UINT16_MAX / UPSCALE_FACTOR ||
            height > UINT16_MAX / UPSCALE_FACTOR) {
            fprintf(stderr, "Skipping sprite %d with unsupported dimensions %ux%u\n",
                id, width, height);
            continue;
        }
        snprintf(rawPath, sizeof(rawPath), "%s/%d.rgba", rawDirectory, id);
        rawFile = fopen(rawPath, "rb");
        if (!rawFile) continue;

        pixelCount = (size_t)width * height * UPSCALE_FACTOR * UPSCALE_FACTOR;
        rawBytes = pixelCount * 4;
        rgba = (UInt8 *)malloc(rawBytes);
        if (!rgba || fread(rgba, 1, rawBytes, rawFile) != rawBytes ||
            fgetc(rawFile) != EOF) {
            fprintf(stderr, "Upscaled pixels for sprite %d have an unexpected size\n", id);
            free(rgba);
            fclose(rawFile);
            fclose(manifest);
            UnloadPack(kPackSp16);
            Resources_Shutdown();
            return 0;
        }
        fclose(rawFile);

        snprintf(spritePath, sizeof(spritePath), "%s/%d.spr", outputDirectory, id);
        spriteFile = fopen(spritePath, "wb");
        if (!spriteFile) {
            fprintf(stderr, "Cannot create upscaled sprite %d\n", id);
            free(rgba);
            fclose(manifest);
            UnloadPack(kPackSp16);
            Resources_Shutdown();
            return 0;
        }

        WriteBE16(spriteFile, width * UPSCALE_FACTOR);
        WriteBE16(spriteFile, height * UPSCALE_FACTOR);
        fputc(source[4] + 2, spriteFile);
        fputc(source[5], spriteFile);
        fputc(source[6], spriteFile);
        fputc(source[7], spriteFile);

        /* 32-bit format: RGBA texels with the soft alpha from the upscaled
           mask; the game blends the edges with the background. */
        if (fwrite(rgba, 1, rawBytes, spriteFile) != rawBytes) {
            fprintf(stderr, "Cannot write upscaled sprite %d\n", id);
            fclose(spriteFile);
            free(rgba);
            fclose(manifest);
            UnloadPack(kPackSp16);
            Resources_Shutdown();
            return 0;
        }
        if (fclose(spriteFile) != 0) {
            fprintf(stderr, "Cannot finish upscaled sprite %d\n", id);
            free(rgba);
            fclose(manifest);
            UnloadPack(kPackSp16);
            Resources_Shutdown();
            return 0;
        }
        free(rgba);
        fprintf(manifest, "%d\n", id);
        ++packed;
    }

    fprintf(manifest, "scale=%d\nformat=rgba32\n", UPSCALE_FACTOR);
    fclose(manifest);
    UnloadPack(kPackSp16);
    Resources_Shutdown();
    printf("Packed %d AI-upscaled sprites\n", packed);
    return packed > 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "export") == 0)
        return ExportSprites(argv[2], argv[3]) ? 0 : 1;
    if (argc == 5 && strcmp(argv[1], "pack") == 0)
        return PackUpscaledSprites(argv[2], argv[3], argv[4]) ? 0 : 1;
    fprintf(stderr,
        "Usage:\n  %s export DATA SOURCE_DIR\n  %s pack DATA RGBA_DIR OUTPUT_DIR\n",
        argv[0], argv[0]);
    return 2;
}

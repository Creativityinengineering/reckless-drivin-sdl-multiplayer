#include "lzrw.h"
#include "resources.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#else
#define MAKE_DIR(path) mkdir(path, 0755)
#endif

enum {
    kPackObjectType = 0,
    kPackRoad = 7,
    kPackLevel1 = 12,
    kLevelCount = 10,
    kLevelHeaderSize = 48,
    kTrackSegmentSize = 12,
    kObjectPositionSize = 16,
    kRoadSegmentSize = 8,
    kMarkSegmentSize = 16,
    kRoadInfoSize = 64
};

static const char *kLevelNames[kLevelCount] = {
    "Highway", "Coastal Road", "Dirt Track", "Highway II", "The River",
    "Coastal Road II", "Ice Road", "Night Drive", "Mountain Pass",
    "The Gauntlet"
};

static uint16_t ReadU16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t ReadS16(const uint8_t *p)
{
    return (int16_t)ReadU16(p);
}

static uint32_t ReadU32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t ReadS32(const uint8_t *p)
{
    return (int32_t)ReadU32(p);
}

static float ReadFloat(const uint8_t *p)
{
    uint32_t bits = ReadU32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int IsRangeValid(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static int WriteBytes(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Cannot create '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (size && fwrite(data, 1, size, file) != size) {
        fprintf(stderr, "Cannot write all bytes to '%s'\n", path);
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "Cannot close '%s': %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

static void DecryptLevelPack(uint8_t *data, size_t size)
{
    static const uint8_t keyBytes[] = {0x1e, 0x42, 0xa7, 0x1f};
    size_t i;
    for (i = 256; i < size; ++i)
        data[i] ^= keyBytes[(i - 256) & 3];
}

static int GetPackEntry(const uint8_t *pack, size_t packSize, int entryId,
                        const uint8_t **entryData, size_t *entrySize)
{
    uint16_t count, startId;
    int index;
    uint32_t offset, nextOffset;

    if (packSize < 16)
        return 0;
    count = ReadU16(pack);
    startId = ReadU16(pack + 8);
    index = entryId - (int)startId + 1;
    if (index < 1 || index > count)
        return 0;

    offset = ReadU32(pack + (size_t)index * 8 + 4);
    if (index == count)
        nextOffset = (uint32_t)packSize;
    else
        nextOffset = ReadU32(pack + (size_t)(index + 1) * 8 + 4);
    if (offset > nextOffset || nextOffset > packSize)
        return 0;
    *entryData = pack + offset;
    *entrySize = (size_t)(nextOffset - offset);
    return 1;
}

static int GetPackEntryById(const uint8_t *pack, size_t packSize, int entryId,
                            const uint8_t **entryData, size_t *entrySize)
{
    uint16_t count;
    size_t i;
    if (packSize < 8)
        return 0;
    count = ReadU16(pack);
    if (!IsRangeValid(packSize, 8, (size_t)count * 8))
        return 0;
    for (i = 0; i < count; ++i) {
        size_t header = 8 + i * 8;
        uint32_t offset, end;
        if (ReadS16(pack + header) != entryId)
            continue;
        offset = ReadU32(pack + header + 4);
        end = i + 1 < count ? ReadU32(pack + header + 12) : (uint32_t)packSize;
        if (offset > end || end > packSize)
            return 0;
        *entryData = pack + offset;
        *entrySize = end - offset;
        return 1;
    }
    return 0;
}

static int WriteFloat(FILE *file, float value)
{
    if (!isfinite(value)) {
        fputs("null", file);
        return 0;
    }
    fprintf(file, "%.9g", (double)value);
    return 1;
}

static void WriteRoadInfo(FILE *file, const uint8_t *info)
{
    fputs("\"roadInfo\":{", file);
    fputs("\"friction\":", file); WriteFloat(file, ReadFloat(info));
    fputs(",\"airResistance\":", file); WriteFloat(file, ReadFloat(info + 4));
    fputs(",\"backResistance\":", file); WriteFloat(file, ReadFloat(info + 8));
    fprintf(file,
            ",\"tolerance\":%u,\"marksId\":%d,\"deathOffset\":%d"
            ",\"backgroundTextureId\":%d,\"foregroundTextureId\":%d"
            ",\"leftBorderTextureId\":%d,\"rightBorderTextureId\":%d"
            ",\"tracksId\":%d,\"skidSoundId\":%d",
            (unsigned)ReadU16(info + 12), (int)ReadS16(info + 14),
            (int)ReadS16(info + 16), (int)ReadS16(info + 18),
            (int)ReadS16(info + 20), (int)ReadS16(info + 22),
            (int)ReadS16(info + 24), (int)ReadS16(info + 26),
            (int)ReadS16(info + 28));
    fputs(",\"xDrift\":", file); WriteFloat(file, ReadFloat(info + 32));
    fputs(",\"yDrift\":", file); WriteFloat(file, ReadFloat(info + 36));
    fputs(",\"xFrontDrift\":", file); WriteFloat(file, ReadFloat(info + 40));
    fputs(",\"yFrontDrift\":", file); WriteFloat(file, ReadFloat(info + 44));
    fputs(",\"trackSlide\":", file); WriteFloat(file, ReadFloat(info + 48));
    fputs(",\"dustSlide\":", file); WriteFloat(file, ReadFloat(info + 52));
    fprintf(file, ",\"dustColor\":%u,\"water\":%u",
            (unsigned)info[56], (unsigned)info[57]);
    fputs(",\"slideFriction\":", file); WriteFloat(file, ReadFloat(info + 60));
    fputc('}', file);
}

static int ExportLevel(const char *outDir, int levelNumber,
                       const uint8_t *pack, size_t packSize,
                       const uint8_t *roadPack, size_t roadPackSize,
                       const uint8_t *objectTypePack, size_t objectTypePackSize)
{
    char path[1024];
    const uint8_t *level, *marks, *roadInfo;
    size_t levelSize, marksSize, roadInfoSize;
    size_t offset, i;
    uint32_t upCount, downCount, objectCount, roadCount;
    uint16_t roadInfoId;
    FILE *json;

    if (!GetPackEntry(pack, packSize, 1, &level, &levelSize) ||
        !GetPackEntry(pack, packSize, 2, &marks, &marksSize)) {
        fprintf(stderr, "Level %d has missing or invalid pack entries\n", levelNumber);
        return 0;
    }
    if (levelSize < kLevelHeaderSize) {
        fprintf(stderr, "Level %d metadata is truncated\n", levelNumber);
        return 0;
    }

    offset = kLevelHeaderSize;
    if (!IsRangeValid(levelSize, offset, 4)) return 0;
    upCount = ReadU32(level + offset);
    if (upCount > (levelSize - offset - 4) / kTrackSegmentSize) return 0;
    offset += 4 + (size_t)upCount * kTrackSegmentSize;
    if (!IsRangeValid(levelSize, offset, 4)) return 0;
    downCount = ReadU32(level + offset);
    if (downCount > (levelSize - offset - 4) / kTrackSegmentSize) return 0;
    offset += 4 + (size_t)downCount * kTrackSegmentSize;
    if (!IsRangeValid(levelSize, offset, 4)) return 0;
    objectCount = ReadU32(level + offset);
    if (objectCount > (levelSize - offset - 4) / kObjectPositionSize) return 0;
    offset += 4 + (size_t)objectCount * kObjectPositionSize;
    if (!IsRangeValid(levelSize, offset, 4)) return 0;
    roadCount = ReadU32(level + offset);
    if (roadCount > (levelSize - offset - 4) / kRoadSegmentSize ||
        offset + 4 + (size_t)roadCount * kRoadSegmentSize != levelSize) {
        fprintf(stderr, "Level %d road data does not match its pack entry size\n",
                levelNumber);
        return 0;
    }
    if (marksSize % kMarkSegmentSize != 0) {
        fprintf(stderr, "Level %d mark data has an invalid size\n", levelNumber);
        return 0;
    }

    roadInfoId = ReadU16(level);
    if (!GetPackEntry(roadPack, roadPackSize, (int)(int16_t)roadInfoId,
                      &roadInfo, &roadInfoSize) || roadInfoSize < kRoadInfoSize) {
        fprintf(stderr, "Level %d references missing road info %d\n",
                levelNumber, (int)(int16_t)roadInfoId);
        return 0;
    }

    snprintf(path, sizeof(path), "%s/level-%02d-pack.bin", outDir, levelNumber);
    if (!WriteBytes(path, pack, packSize)) return 0;
    snprintf(path, sizeof(path), "%s/level-%02d-entry-1.bin", outDir, levelNumber);
    if (!WriteBytes(path, level, levelSize)) return 0;
    snprintf(path, sizeof(path), "%s/level-%02d-entry-2.bin", outDir, levelNumber);
    if (!WriteBytes(path, marks, marksSize)) return 0;
    snprintf(path, sizeof(path), "%s/level-%02d.json", outDir, levelNumber);
    json = fopen(path, "w");
    if (!json) {
        fprintf(stderr, "Cannot create '%s': %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(json,
            "{\n  \"format\": \"reckless-drivin-level-export-v1\",\n"
            "  \"level\": %d,\n  \"name\": \"%s\",\n"
            "  \"metadata\": {\n    \"roadInfoId\": %d,\n"
            "    \"time\": %u,\n    \"objectGroups\": [",
            levelNumber, kLevelNames[levelNumber - 1],
            (int)(int16_t)roadInfoId, (unsigned)ReadU16(level + 2));
    for (i = 0; i < 10; ++i) {
        const uint8_t *group = level + 4 + i * 4;
        if (i) fputc(',', json);
        fprintf(json, "\n      {\"resourceId\":%d,\"objectCount\":%d}",
                (int)ReadS16(group), (int)ReadS16(group + 2));
    }
    fprintf(json, "\n    ],\n    \"startX\": %d,\n    \"levelEnd\": %u\n  },\n",
            (int)ReadS16(level + 44), (unsigned)ReadU16(level + 46));
    WriteRoadInfo(json, roadInfo);
    fputs(",\n  \"trackUp\": [", json);
    offset = kLevelHeaderSize + 4;
    for (i = 0; i < upCount; ++i, offset += kTrackSegmentSize) {
        const uint8_t *seg = level + offset;
        if (i) fputc(',', json);
        fprintf(json, "\n    {\"flags\":%u,\"x\":%d,\"y\":%d,\"speed\":",
                (unsigned)ReadU16(seg), (int)ReadS16(seg + 2),
                (int)ReadS32(seg + 4));
        WriteFloat(json, ReadFloat(seg + 8));
        fputc('}', json);
    }
    fprintf(json, "\n  ],\n  \"trackDown\": [");
    offset = kLevelHeaderSize + 4 + (size_t)upCount * kTrackSegmentSize;
    offset += 4;
    for (i = 0; i < downCount; ++i, offset += kTrackSegmentSize) {
        const uint8_t *seg = level + offset;
        if (i) fputc(',', json);
        fprintf(json, "\n    {\"flags\":%u,\"x\":%d,\"y\":%d,\"speed\":",
                (unsigned)ReadU16(seg), (int)ReadS16(seg + 2),
                (int)ReadS32(seg + 4));
        WriteFloat(json, ReadFloat(seg + 8));
        fputc('}', json);
    }
    fprintf(json, "\n  ],\n  \"objects\": [");
    offset = kLevelHeaderSize + 4 + (size_t)upCount * kTrackSegmentSize;
    offset += 4 + (size_t)downCount * kTrackSegmentSize + 4;
    for (i = 0; i < objectCount; ++i, offset += kObjectPositionSize) {
        const uint8_t *obj = level + offset;
        if (i) fputc(',', json);
        fprintf(json, "\n    {\"x\":%d,\"y\":%d,\"direction\":",
                (int)ReadS32(obj), (int)ReadS32(obj + 4));
        WriteFloat(json, ReadFloat(obj + 8));
        fprintf(json, ",\"typeResourceId\":%d}", (int)ReadS16(obj + 12));
    }
    fputs("\n  ],\n  \"objectTypes\": [", json);
    {
        int16_t seen[512];
        size_t seenCount = 0;
        int first = 1;
        offset = kLevelHeaderSize + 4 + (size_t)upCount * kTrackSegmentSize;
        offset += 4 + (size_t)downCount * kTrackSegmentSize + 4;
        for (i = 0; i < objectCount; ++i, offset += kObjectPositionSize) {
            const uint8_t *obj = level + offset, *typeData;
            size_t typeSize, j;
            int16_t typeId = ReadS16(obj + 12);
            int duplicate = 0;
            for (j = 0; j < seenCount; ++j)
                if (seen[j] == typeId) duplicate = 1;
            if (duplicate) continue;
            if (seenCount >= sizeof(seen) / sizeof(seen[0])) {
                fprintf(stderr, "Level %d has too many distinct object types\n",
                        levelNumber);
                fclose(json);
                return 0;
            }
            seen[seenCount++] = typeId;
            if (!GetPackEntryById(objectTypePack, objectTypePackSize,
                                  typeId, &typeData, &typeSize) || typeSize < 64) {
                fprintf(stderr, "Level %d object type %d is missing or truncated\n",
                        levelNumber, typeId);
                fclose(json);
                return 0;
            }
            if (!first) fputc(',', json);
            first = 0;
            fprintf(json, "\n    {\"resourceId\":%d,\"spriteId\":%d,\"width\":",
                    typeId, (int)ReadS16(typeData + 20));
            WriteFloat(json, ReadFloat(typeData + 40));
            fputs(",\"length\":", json);
            WriteFloat(json, ReadFloat(typeData + 44));
            fputc('}', json);
        }
    }
    fprintf(json, "\n  ],\n  \"roadSegments\": [");
    offset += 4;
    for (i = 0; i < roadCount; ++i, offset += kRoadSegmentSize) {
        const uint8_t *seg = level + offset;
        if (i) fputc(',', json);
        fprintf(json, "\n    [%d,%d,%d,%d]", (int)ReadS16(seg),
                (int)ReadS16(seg + 2), (int)ReadS16(seg + 4),
                (int)ReadS16(seg + 6));
    }
    fprintf(json, "\n  ],\n  \"marks\": [");
    for (i = 0; i < marksSize / kMarkSegmentSize; ++i) {
        const uint8_t *mark = marks + i * kMarkSegmentSize;
        if (i) fputc(',', json);
        fputs("\n    {\"x1\":", json); WriteFloat(json, ReadFloat(mark));
        fputs(",\"y1\":", json); WriteFloat(json, ReadFloat(mark + 4));
        fputs(",\"x2\":", json); WriteFloat(json, ReadFloat(mark + 8));
        fputs(",\"y2\":", json); WriteFloat(json, ReadFloat(mark + 12));
        fputc('}', json);
    }
    fputs("\n  ],\n  \"rawFiles\": {\n", json);
    fprintf(json,
            "    \"decompressedPack\": \"level-%02d-pack.bin\",\n"
            "    \"levelEntry\": \"level-%02d-entry-1.bin\",\n"
            "    \"marksEntry\": \"level-%02d-entry-2.bin\"\n  }\n}\n",
            levelNumber, levelNumber, levelNumber);
    if (fclose(json) != 0) {
        fprintf(stderr, "Cannot finish writing '%s'\n", path);
        return 0;
    }

    printf("Level %02d %-16s: %u road sections, %u/%u AI points, "
           "%u objects, %zu marks\n",
           levelNumber, kLevelNames[levelNumber - 1], roadCount,
           upCount, downCount, objectCount, marksSize / kMarkSegmentSize);
    return 1;
}

static int LoadDecompressedPack(int packNumber, int encrypted,
                                uint8_t **packData, size_t *packSize)
{
    Handle handle = Resources_Get(MakeFourCC("Pack"), packNumber + 128);
    long compressedSize, decompressedSize = 0;
    uint8_t *compressed, *decompressed;

    if (!handle) {
        fprintf(stderr, "Pack resource %d is missing\n", packNumber + 128);
        return 0;
    }
    compressedSize = Resources_GetSize(handle);
    compressed = (uint8_t *)*handle;
    if (encrypted) {
        uint8_t *copy = (uint8_t *)malloc((size_t)compressedSize);
        if (!copy) {
            Resources_Release(handle);
            fprintf(stderr, "Out of memory decrypting pack %d\n", packNumber);
            return 0;
        }
        memcpy(copy, compressed, (size_t)compressedSize);
        DecryptLevelPack(copy, (size_t)compressedSize);
        decompressed = (uint8_t *)LZRW_Decompress(copy, compressedSize,
                                                  &decompressedSize);
        free(copy);
    } else {
        decompressed = (uint8_t *)LZRW_Decompress(compressed, compressedSize,
                                                  &decompressedSize);
    }
    Resources_Release(handle);
    if (!decompressed || decompressedSize <= 0) {
        free(decompressed);
        fprintf(stderr, "Could not decompress pack %d\n", packNumber);
        return 0;
    }
    *packData = decompressed;
    *packSize = (size_t)decompressedSize;
    return 1;
}

int main(int argc, char **argv)
{
    const char *dataPath = argc > 1 ? argv[1] : "Data";
    const char *outDir = argc > 2 ? argv[2] : "build/level-data";
    uint8_t *roadPack = NULL;
    uint8_t *objectTypePack = NULL;
    size_t roadPackSize = 0, objectTypePackSize = 0;
    int level, result = EXIT_FAILURE;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [Data-file [output-directory]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (MAKE_DIR(outDir) != 0 && errno != EEXIST) {
        fprintf(stderr, "Cannot create output directory '%s': %s\n",
                outDir, strerror(errno));
        return EXIT_FAILURE;
    }
    if (!Resources_Init(dataPath))
        return EXIT_FAILURE;
    if (!LoadDecompressedPack(kPackRoad, 0, &roadPack, &roadPackSize))
        goto cleanup;
    if (!LoadDecompressedPack(kPackObjectType, 0, &objectTypePack,
                              &objectTypePackSize))
        goto cleanup;

    for (level = 0; level < kLevelCount; ++level) {
        uint8_t *pack = NULL;
        size_t packSize = 0;
        if (!LoadDecompressedPack(kPackLevel1 + level, level >= 3,
                                  &pack, &packSize))
            goto cleanup;
        if (!ExportLevel(outDir, level + 1, pack, packSize,
                         roadPack, roadPackSize, objectTypePack,
                         objectTypePackSize)) {
            free(pack);
            goto cleanup;
        }
        free(pack);
    }
    result = EXIT_SUCCESS;

cleanup:
    free(roadPack);
    free(objectTypePack);
    Resources_Shutdown();
    return result;
}

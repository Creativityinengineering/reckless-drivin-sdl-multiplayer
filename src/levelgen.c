#include "levelgen.h"
#include "objects.h"
#include "roads.h"
#include "packs.h"
#include "route.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Settings ---- */

tLevelGen gLevelGen = {12345, 0, 1, 1, 1, 2, 1, 1, 1, 1, 0, 0};
int gLevelGenOn;
static tLevelGen sSaved[kMaxSavedLevels];
static int sSavedCount;

static const char *const kLandscapes[kGenLandscapes] = {
	"HIGHWAY", "COAST", "DIRT TRACK", "COUNTRYSIDE", "RIVER - BOATS",
	"HARBOUR", "ICE ROAD", "NIGHT DRIVE", "MOUNTAIN PASS", "GAUNTLET"
};

static const char *const kChoices[10][kGenChoices] = {
	{"SHORT", "NORMAL", "LONG", "MARATHON"},	/* length */
	{"STRAIGHT", "GENTLE", "TWISTY", "CRAZY"},	/* curves */
	{"NARROW", "NORMAL", "2 LANES", "3 LANES"},	/* road width: lanes per direction */
	{"NONE", "LIGHT", "NORMAL", "HEAVY"},	/* traffic */
	{"SLOW", "NORMAL", "FAST", "RACING"},	/* traffic speed */
	{"NONE", "FEW", "NORMAL", "MANY"},	/* police */
	{"NONE", "SOME", "MANY", "LOTS"},	/* jumps */
	{"RELAXED", "NORMAL", "TIGHT", "BRUTAL"},	/* time limit */
	{"NONE", "ONE", "TWO", "THREE"},	/* crossings: loops over a bridge */
	{"NONE", "ONE", "TWO", "THREE"}	/* forks: side roads */
};

const char *LevelGen_LandscapeName(int landscape)
{
	return landscape >= 0 && landscape < kGenLandscapes ? kLandscapes[landscape] : "?";
}

const char *LevelGen_ChoiceName(int setting, int value)
{
	if (setting < 0 || setting >= 10 || value < 0 || value >= kGenChoices)
		return "?";
	return kChoices[setting][value];
}

/* ---- Level codes: the seed and the settings in 12 letters and digits ---- */

static const char kCodeAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

UInt32 LevelGen_Pack(const tLevelGen *gen)
{
	return (UInt32)(gen->landscape & 15) | (UInt32)(gen->length & 3) << 4 |
		(UInt32)(gen->curves & 3) << 6 | (UInt32)(gen->width & 3) << 8 |
		(UInt32)(gen->traffic & 3) << 10 | (UInt32)(gen->speed & 3) << 12 |
		(UInt32)(gen->cops & 3) << 14 | (UInt32)(gen->jumps & 3) << 16 |
		(UInt32)(gen->time & 3) << 18 | (UInt32)(gen->crossings & 3) << 20 |
		(UInt32)(gen->forks & 3) << 22;
}

void LevelGen_Unpack(UInt32 packed, UInt32 seed, tLevelGen *gen)
{
	gen->seed = seed;
	gen->landscape = (UInt8)(packed & 15);
	if (gen->landscape >= kGenLandscapes)
		gen->landscape = 0;
	gen->length = (UInt8)(packed >> 4 & 3);
	gen->curves = (UInt8)(packed >> 6 & 3);
	gen->width = (UInt8)(packed >> 8 & 3);
	gen->traffic = (UInt8)(packed >> 10 & 3);
	gen->speed = (UInt8)(packed >> 12 & 3);
	gen->cops = (UInt8)(packed >> 14 & 3);
	gen->jumps = (UInt8)(packed >> 16 & 3);
	gen->time = (UInt8)(packed >> 18 & 3);
	gen->crossings = (UInt8)(packed >> 20 & 3);
	gen->forks = (UInt8)(packed >> 22 & 3);
}

static UInt8 CodeCheck(UInt64 bits)
{
	return (UInt8)((bits * 0x9E3779B97F4A7C15ull) >> 56 ^ 0x5A);
}

/* Crossings and forks came later: they change the check letter's value
   (extra = crossings + 4 * forks), so the codes from before stay exactly
   the same. */
static UInt8 CrossingsCheck(UInt64 bits, int extra)
{
	return (UInt8)(CodeCheck(bits) ^ (extra * 0x6D));
}

void LevelGen_Encode(const tLevelGen *gen, char code[kLevelCodeSize])
{
	UInt64 bits = (UInt64)(LevelGen_Pack(gen) & 0xFFFFF) << 32 | gen->seed;	/* 52 bits */
	UInt64 all = bits << 8 | CrossingsCheck(bits, (gen->crossings & 3) + 4 * (gen->forks & 3));	/* 60 bits: 12 letters */
	int i, out = 0;
	for (i = 0; i < 12; ++i) {
		if (i == 4 || i == 8)
			code[out++] = '-';
		code[out++] = kCodeAlphabet[(all >> (55 - 5 * i)) & 31];
	}
	code[out] = '\0';
}

int LevelGen_Decode(const char *code, tLevelGen *gen)
{
	UInt64 all = 0, bits;
	int count = 0;
	for (; *code; ++code) {
		const char *at;
		char c = *code;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		if (c == '-' || c == ' ')
			continue;
		at = strchr(kCodeAlphabet, c);
		if (!at || !c || count >= 12)
			return 0;
		all = all << 5 | (UInt64)(at - kCodeAlphabet);
		count++;
	}
	if (count != 12)
		return 0;
	bits = all >> 8;
	if ((bits >> 32 & 15) >= kGenLandscapes)
		return 0;
	{
		int extra;
		for (extra = 0; extra < 16; ++extra)
			if (CrossingsCheck(bits, extra) == (UInt8)(all & 255)) {
				LevelGen_Unpack((UInt32)(bits >> 32) | (UInt32)(extra & 3) << 20 |
					(UInt32)(extra >> 2) << 22, (UInt32)bits, gen);
				return 1;
			}
	}
	return 0;
}

/* ---- Settings file "Generator" in the preferences folder ---- */

static const char *SettingsPath(void)
{
	static char path[600];
	char *prefDir = SDL_GetPrefPath("DarrCoh", "RecklessDrivin");
	if (!prefDir)
		return NULL;
	snprintf(path, sizeof(path), "%sGenerator", prefDir);
	SDL_free(prefDir);
	return path;
}

void LevelGen_LoadSettings(void)
{
	const char *path = SettingsPath();
	FILE *file = path ? fopen(path, "r") : NULL;
	char line[64];
	sSavedCount = 0;
	if (!file) {
		gLevelGen.seed = (UInt32)SDL_GetTicks() * 2654435761u % 1000000000u;
		return;
	}
	while (fgets(line, sizeof(line), file)) {
		tLevelGen gen;
		if (!strncmp(line, "on=", 3))
			gLevelGenOn = atoi(line + 3) != 0;
		else if (!strncmp(line, "current=", 8) && LevelGen_Decode(line + 8, &gen))
			gLevelGen = gen;
		else if (!strncmp(line, "saved=", 6) && sSavedCount < kMaxSavedLevels &&
			LevelGen_Decode(line + 6, &gen))
			sSaved[sSavedCount++] = gen;
	}
	fclose(file);
}

void LevelGen_SaveSettings(void)
{
	const char *path = SettingsPath();
	FILE *file = path ? fopen(path, "w") : NULL;
	char code[kLevelCodeSize];
	int i;
	if (!file)
		return;
	fprintf(file, "on=%d\n", gLevelGenOn);
	LevelGen_Encode(&gLevelGen, code);
	fprintf(file, "current=%s\n", code);
	for (i = 0; i < sSavedCount; ++i) {
		LevelGen_Encode(&sSaved[i], code);
		fprintf(file, "saved=%s\n", code);
	}
	fclose(file);
}

int LevelGen_SavedCount(void)
{
	return sSavedCount;
}

int LevelGen_GetSaved(int index, tLevelGen *gen)
{
	if (index < 0 || index >= sSavedCount)
		return 0;
	*gen = sSaved[index];
	return 1;
}

void LevelGen_AddSaved(const tLevelGen *gen)
{
	int i;
	for (i = 0; i < sSavedCount; ++i)
		if (sSaved[i].seed == gen->seed && LevelGen_Pack(&sSaved[i]) == LevelGen_Pack(gen))
			return;	/* already in the list */
	if (sSavedCount == kMaxSavedLevels) {	/* full: the oldest goes */
		memmove(sSaved, sSaved + 1, sizeof(sSaved[0]) * (kMaxSavedLevels - 1));
		sSavedCount--;
	}
	sSaved[sSavedCount++] = *gen;
	LevelGen_SaveSettings();
}

void LevelGen_DeleteSaved(int index)
{
	if (index < 0 || index >= sSavedCount)
		return;
	memmove(sSaved + index, sSaved + index + 1, sizeof(sSaved[0]) * (sSavedCount - index - 1));
	sSavedCount--;
	LevelGen_SaveSettings();
}

/* ---- The generator ----
   Its own random numbers and only +, -, * and / on floats, so every
   computer builds exactly the same road from the same code. */

static UInt32 sRandom;

static UInt32 Rnd(void)
{
	sRandom ^= sRandom << 13;
	sRandom ^= sRandom >> 17;
	sRandom ^= sRandom << 5;
	return sRandom;
}

static float RndF(float low, float high)
{
	return low + (high - low) * (float)(Rnd() >> 8) / 16777216.0f;
}

static int RndI(int low, int high)
{
	return low + (int)(Rnd() % (UInt32)(high - low + 1));
}

static tLevelData *sLevel;
static tTrackInfo *sUp, *sDown;
static tRoadSeg *sRoad;
static UInt32 sRoadCount;
static tMarkSeg *sMarks;
static float *sSlope;
static int sLoaded;
int gGenLanes;
static float sBridges[16];
static int sBridgeCount;

int LevelGen_GetBridges(const float **heights)
{
	*heights = sBridges;
	return sLoaded ? sBridgeCount : 0;
}

int LevelGen_IsLoaded(void)
{
	return sLoaded;
}

void LevelGen_Dispose(void)
{
	free(sLevel);
	free(sUp);
	free(sDown);
	free(sRoad);
	free(sMarks);
	free(sSlope);
	sLevel = NULL;
	sUp = sDown = NULL;
	sRoad = NULL;
	sMarks = NULL;
	sSlope = NULL;
	sLoaded = 0;
	gGenLanes = 0;
	sBridgeCount = 0;
	Route_Dispose();
}

static int Single(const tRoadSeg seg)
{
	return seg[1] == seg[2];
}

/* Where traffic drives: up on the right, down on the left. */
static float UpLaneX(const tRoadSeg seg)
{
	return Single(seg) ? seg[0] + (seg[3] - seg[0]) * (gGenLanes > 1 ? 0.75f : 0.72f) :
		(seg[2] + seg[3]) * 0.5f;
}

static float DownLaneX(const tRoadSeg seg)
{
	return Single(seg) ? seg[0] + (seg[3] - seg[0]) * (gGenLanes > 1 ? 0.25f : 0.28f) :
		(seg[0] + seg[1]) * 0.5f;
}

static const tRoadSeg *RoadAt(float y);

/* Multi-lane roads: the traffic track runs down the middle of each
   direction's carriageway; a car keeps to its lane beside it, as many lanes
   as fit there (fewer where the road narrows). */
float LevelGen_LaneOffset(int lane, float x, float y)
{
	const tRoadSeg *seg;
	float width;
	int lanes;
	if (!sLoaded || gGenLanes < 2 || lane <= 0)
		return 0;
	if (gRouteActive) {	/* free-form road: one road, the track mid-way of each half */
		width = Route_HalfWidth();
		lanes = gGenLanes;
		return ((lane - 1) % lanes - (lanes - 1) * 0.5f) * (width / lanes);
	}
	seg = RoadAt(y);
	if (Single(*seg))
		width = ((*seg)[3] - (*seg)[0]) * 0.5f;
	else if (x >= (*seg)[2])
		width = (float)((*seg)[3] - (*seg)[2]);
	else
		width = (float)((*seg)[1] - (*seg)[0]);
	lanes = (int)(width / 80);
	if (lanes > gGenLanes)
		lanes = gGenLanes;
	if (lanes < 2)
		return 0;
	return ((lane - 1) % lanes - (lanes - 1) * 0.5f) * (width / lanes);
}

static const tRoadSeg *RoadAt(float y)
{
	int row = (int)(y / 2);
	if (row < 0)
		row = 0;
	if (row >= (int)sRoadCount)
		row = (int)sRoadCount - 1;
	return &sRoad[row];
}

static float SlopeAt(float y)
{
	int row = (int)(y / 2);
	if (row < 0)
		row = 0;
	if (row >= (int)sRoadCount)
		row = (int)sRoadCount - 1;
	return sSlope[row];
}

static float Abs(float v)
{
	return v < 0 ? -v : v;
}

/* The original level a landscape comes from: road surface, scenery and
   traffic groups, and whether its roads have lane marks. */
typedef struct {
	SInt16 roadInfo;
	UInt16 time;
	tObjectGroupReference groups[10];
	int marks;
} tBaseLevel;

static int ReadBaseLevel(int landscape, tBaseLevel *base)
{
	int pack = kPackLevel1 + landscape, loadedHere = 0, size = 0, i;
	const UInt8 *raw;
	memset(base, 0, sizeof(*base));
	if (!gPacks[pack]) {
		LoadPack(pack);
		loadedHere = 1;
	}
	if (!gPacks[pack])
		return 0;
	/* Read without touching the pack (LoadLevel swaps it in place). */
	raw = (const UInt8 *)GetSortedPackEntry(pack, 1, NULL);
	if (raw) {
		base->roadInfo = (SInt16)(raw[0] << 8 | raw[1]);
		base->time = (UInt16)(raw[2] << 8 | raw[3]);
		for (i = 0; i < 10; ++i) {
			base->groups[i].resID = (SInt16)(raw[4 + 4 * i] << 8 | raw[5 + 4 * i]);
			base->groups[i].numObjs = (SInt16)(raw[6 + 4 * i] << 8 | raw[7 + 4 * i]);
		}
	}
	GetSortedPackEntry(pack, 2, &size);
	base->marks = size > 0;
	if (loadedHere)
		UnloadPack(pack);
	return raw != NULL;
}

/* Scenery stays as in the original; traffic and police follow the
   settings; everything grows with the length. */
static SInt16 ScaleGroup(tObjectGroupReference ref, const tLevelGen *gen, float lengthScale)
{
	static const float kTraffic[4] = {0, 0.5f, 1, 1.8f};
	static const float kCops[4] = {0, 0.5f, 1, 2.2f};
	tObjectGroup *group = (tObjectGroup *)GetSortedPackEntry(kPackOgrp, ref.resID, NULL);
	int entry, cops = 0, cars = 0;
	float scale = lengthScale;
	if (!group)
		return 0;
	for (entry = 0; entry < (int)group->numEntries; ++entry) {
		tObjectType *type = (tObjectType *)GetSortedPackEntry(kPackObTy, group->data[entry].typeRes, NULL);
		if (!type || !group->data[entry].probility)
			continue;
		if (type->flags & kObjectCop)
			cops = 1;
		else if (type->flags & kObjectWheelFlag)
			cars = 1;
	}
	if (cops)
		scale *= kCops[gen->cops];
	else if (cars)	/* more lanes, more cars */
		scale *= kTraffic[gen->traffic] * ((gen->width & 3) >= 2 ? (gen->width & 3) * 0.9f : 1);
	return (SInt16)(ref.numObjs * scale + 0.5f);
}

static const UInt16 kLength[4] = {20000, 32000, 44000, 58000};
static const float kMaxCurve[4] = {0.0002f, 0.0011f, 0.0019f, 0.0028f};
static const float kMaxSlope[4] = {0.2f, 0.85f, 1.25f, 1.65f};
static const float kCurveEase[4] = {0.02f, 0.02f, 0.03f, 0.045f};
static const int kJumps[4] = {0, 9000, 5000, 2800};	/* road per ramp */

/* Only the river world is water (boats, no medians). */
static int GenIsWater(int landscape)
{
	return landscape == 4;
}

/* The road of level levelIndex: center line, width layout, slopes. Uses
   the generator's random numbers first, the ramps next. */
static int GenerateRoad(const tLevelGen *gen, int levelIndex)
{
	UInt16 levelEnd = kLength[gen->length & 3];
	int rows, row, i, water = GenIsWater(gen->landscape), segmentLeft = 0, layoutLeft = 0, blendRows = 0;
	float x = 0, slope = 0, curve = 0, targetCurve = 0, laneWidth, medianWidth;
	/* offsets from the center line: a single road is kept as a median of
	   width 0 in the middle, so a median opens and closes in the middle */
	float from[4] = {-130, 0, 0, 130}, to[4] = {-130, 0, 0, 130};
	sRandom = gen->seed * 2654435761u ^ (UInt32)(levelIndex + 1) * 0x9E3779B9u ^
		LevelGen_Pack(gen) * 0x85EBCA6Bu;
	if (!sRandom)
		sRandom = 1;
	for (i = 0; i < 8; ++i)
		Rnd();
	/* one lane width and one (narrow) median for the whole level, as on a
	   real road; the median only varies a little */
	laneWidth = gen->width == 3 ? RndF(255, 285) : gen->width == 2 ? RndF(175, 200) : RndF(150, 180);
	medianWidth = gen->width == 3 ? RndF(44, 80) : gen->width == 2 ? RndF(50, 120) : RndF(60, 200);

	/* The road: a center line that bends, and its width layout. */
	rows = (levelEnd + 2400) / 2;
	sRoadCount = (UInt32)rows;
	sRoad = (tRoadSeg *)calloc((size_t)rows, sizeof(tRoadSeg));
	sSlope = (float *)calloc((size_t)rows, sizeof(float));
	if (!sRoad || !sSlope) {
		return 0;
	}
	for (row = 0; row < rows; ++row) {
		float y = row * 2.0f, offs[4];
		int calm = y < 1400 || y > levelEnd - 1400;
		/* bends */
		if (calm)
			targetCurve = -slope * 0.004f;
		else if (--segmentLeft <= 0) {
			segmentLeft = gen->curves >= 3 ? RndI(140, 650) : gen->curves == 2 ? RndI(200, 900) : RndI(250, 1300);
			targetCurve = RndI(0, 4) ? RndF(-kMaxCurve[gen->curves], kMaxCurve[gen->curves]) : 0;
			/* stay in a sensible area */
			if ((x > 2500 && targetCurve > 0) || (x < -2500 && targetCurve < 0))
				targetCurve = -targetCurve;
		}
		curve += (targetCurve - curve) * kCurveEase[gen->curves & 3];
		slope += curve * 2;
		if (slope > kMaxSlope[gen->curves]) {
			slope = kMaxSlope[gen->curves];
			curve = 0;
		} else if (slope < -kMaxSlope[gen->curves]) {
			slope = -kMaxSlope[gen->curves];
			curve = 0;
		}
		x += slope * 2;
		sSlope[row] = slope;
		/* width: one road or two carriageways with a median */
		/* the finish is on one road: close a median well before it */
		if (y > levelEnd - 3200 && to[2] > to[1] && layoutLeft > 1)
			layoutLeft = 1;
		if (--layoutLeft <= 0) {
			int divided = 0;
			float width;
			layoutLeft = gen->width >= 2 ? RndI(1500, 4000) : RndI(800, 2600);
			blendRows = 300;
			memcpy(from, to, sizeof(from));
			switch (gen->width & 3) {
				case 0: width = RndF(180, 210); break;
				case 1: width = RndF(240, 280); divided = RndI(0, 99) < 15; break;
				case 2: width = RndF(340, 380); divided = RndI(0, 99) < 75; break;
				default: width = RndF(500, 540); divided = 1; break;	/* a motorway */
			}
			if (water || y < 1600 || y > levelEnd - 3400)
				divided = 0;
			if (divided) {
				float lane = laneWidth;
				float median = medianWidth * RndF(0.9f, 1.1f);
				to[0] = -median / 2 - lane;
				to[1] = -median / 2;
				to[2] = median / 2;
				to[3] = median / 2 + lane;
			} else {
				to[0] = -width / 2;
				to[1] = to[2] = 0;
				to[3] = width / 2;
			}
		}
		for (i = 0; i < 4; ++i) {
			float t = blendRows > 0 ? (300 - blendRows) / 300.0f : 1;
			offs[i] = from[i] + (to[i] - from[i]) * t;
		}
		if (blendRows > 0)
			blendRows--;
		for (i = 0; i < 4; ++i)
			sRoad[row][i] = (SInt16)floorf(x + offs[i] + 0.5f);
		/* a median not open yet (or just closing) is no median: one road,
		   written as the original levels do */
		if (sRoad[row][2] - sRoad[row][1] < 10)
			sRoad[row][1] = sRoad[row][2] = sRoad[row][3];
	}

	gGenLanes = water ? 1 : (gen->width & 3) >= 2 ? (gen->width & 3) : 1;
	return 1;
}

/* Ramps on long straights: their heights and lanes (up or down). */
static int PlaceRamps(const tLevelGen *gen, float *heights, UInt8 *upLane, int max);

/* Overpasses: another road crossing on a bridge, over straight stretches
   (not on the river). Random numbers after the ramps. */
static int PlaceBridges(const tLevelGen *gen, float *heights, int max)
{
	UInt16 levelEnd = kLength[gen->length & 3];
	int bridges = levelEnd / 10000 + RndI(0, 1), bridge, placed = 0;
	if (GenIsWater(gen->landscape))
		return 0;
	for (bridge = 0; bridge < bridges && placed < max; ++bridge) {
		int tries, ok = 0;
		float y = 0;
		for (tries = 0; tries < 40 && !ok; ++tries) {
			float d;
			int other;
			y = RndF(3000, levelEnd - 3500.0f);
			ok = Abs(SlopeAt(y)) < 0.35f;
			for (d = -500; d <= 500 && ok; d += 100)
				if (Abs(SlopeAt(y + d) - SlopeAt(y)) > 0.06f)
					ok = 0;
			for (other = 0; other < placed && ok; ++other)
				if (Abs(heights[other] - y) < 3000)
					ok = 0;
		}
		if (ok)
			heights[placed++] = y;
	}
	return placed;
}

static int PlaceRamps(const tLevelGen *gen, float *heights, UInt8 *upLane, int max)
{
	UInt16 levelEnd = kLength[gen->length & 3];
	int ramps, ramp, placed = 0;
	if (!kJumps[gen->jumps & 3])
		return 0;
	ramps = levelEnd / kJumps[gen->jumps & 3];
	for (ramp = 0; ramp < ramps && placed < max; ++ramp) {
		float y = 0;
		int tries, straight = 0;
		/* a long straight after the ramp: the flight and the landing */
		for (tries = 0; tries < 40 && !straight; ++tries) {
			float d;
			y = RndF(2500, levelEnd - 3000.0f);
			straight = Abs(SlopeAt(y)) < 0.3f;
			for (d = -300; d <= 1800 && straight; d += 100)
				if (Abs(SlopeAt(y + d) - SlopeAt(y)) > 0.05f)
					straight = 0;
		}
		upLane[placed] = (UInt8)(RndI(0, 2) != 0);
		if (straight)
			heights[placed++] = y;
	}
	return placed;
}

/* ---- Free-form roads with crossings (route.c) ----
   The road runs mostly north with bends; for each crossing it makes a loop
   to the right (east, south, west) whose westward stretch crosses the road
   it came on, on a bridge, and then carries on north beside it. Headings
   are turned with + - * / and sqrt only, so every computer builds the same
   road. */

typedef struct {
	tRoutePoint *points;
	UInt8 *deck;
	int count, capacity;
	float x, y, hx, hy, theta;	/* theta: heading angle for the bend limits */
} tPath;

static int PathPush(tPath *path)
{
	if (path->count == path->capacity) {
		int capacity = path->capacity ? path->capacity * 2 : 32768;
		tRoutePoint *points = (tRoutePoint *)realloc(path->points, (size_t)capacity * sizeof(tRoutePoint));
		UInt8 *deck;
		if (!points)
			return 0;
		path->points = points;
		deck = (UInt8 *)realloc(path->deck, (size_t)capacity);
		if (!deck)
			return 0;
		path->deck = deck;
		path->capacity = capacity;
	}
	path->points[path->count].x = path->x;
	path->points[path->count].y = path->y;
	path->points[path->count].hx = path->hx;
	path->points[path->count].hy = path->hy;
	path->deck[path->count] = 0;
	path->count++;
	return 1;
}

/* One step along the road, turning right by curvature (1/radius, negative:
   left). */
static int PathStep(tPath *path, float curvature)
{
	float a = curvature * kRouteStep, a2 = a * a;
	float c = 1 - a2 / 2 + a2 * a2 / 24, sn = a - a * a2 / 6;
	float nx = path->hx * c + path->hy * sn, ny = -path->hx * sn + path->hy * c;
	float length = sqrtf(nx * nx + ny * ny);
	path->hx = nx / length;
	path->hy = ny / length;
	path->theta += a;
	path->x += path->hx * kRouteStep;
	path->y += path->hy * kRouteStep;
	return PathPush(path);
}

static int PathStraight(tPath *path, float length)
{
	int steps = (int)(length / kRouteStep), i;
	for (i = 0; i < steps; ++i)
		if (!PathStep(path, 0))
			return 0;
	return 1;
}

static int PathArc(tPath *path, float angle, float radius)
{
	int steps = (int)(Abs(angle) * radius / kRouteStep + 0.5f), i;
	float curvature;
	if (steps < 1)
		return 1;
	curvature = angle / (steps * kRouteStep);
	for (i = 0; i < steps; ++i)
		if (!PathStep(path, curvature))
			return 0;
	return 1;
}

static void PathSnap(tPath *path, float hx, float hy, float theta)
{
	path->hx = hx;
	path->hy = hy;
	path->theta = theta;
}

static float RouteHalfWidth(const tLevelGen *gen)
{
	static const float kHalf[4] = {95, 130, 175, 260};
	return kHalf[gen->width & 3];
}

static void SeedRandom(const tLevelGen *gen, int levelIndex)
{
	int i;
	sRandom = gen->seed * 2654435761u ^ (UInt32)(levelIndex + 1) * 0x9E3779B9u ^
		LevelGen_Pack(gen) * 0x85EBCA6Bu;
	if (!sRandom)
		sRandom = 1;
	for (i = 0; i < 8; ++i)
		Rnd();
}

/* The loop: from heading north round to north again, to the right (east,
   south, west) or to the left (west, south, east), crossing the road it
   came on. */
static int PathLoop(tPath *path, float hw, float minRadius)
{
	float radius = hw * 2.2f > 300 ? hw * 2.2f : 300;
	float side = RndI(0, 1) ? 1.0f : -1.0f;	/* 1: right round, -1: left round */
	float quarter = side * 3.14159265f / 2;
	float east = RndF(150, 400), south = 2 * hw + RndF(200, 400), apart = 2 * hw + RndF(120, 260);
	float x0, back;
	int from, i;
	if (Abs(path->theta) > 0.01f && !PathArc(path, -path->theta, minRadius))
		return 0;
	PathSnap(path, 0, 1, 0);
	if (!PathStraight(path, south + radius + hw + 200))
		return 0;
	x0 = path->x;
	if (!PathArc(path, quarter, radius))
		return 0;
	PathSnap(path, side, 0, 0);
	if (!PathStraight(path, east) || !PathArc(path, quarter, radius))
		return 0;
	PathSnap(path, 0, -1, 0);
	if (!PathStraight(path, south) || !PathArc(path, quarter, radius))
		return 0;
	PathSnap(path, -side, 0, 0);
	back = radius + east + apart;
	from = path->count;
	if (!PathStraight(path, back))
		return 0;
	for (i = from; i < path->count; ++i)	/* the bridge over the road below */
		if (Abs(path->points[i].x - x0) < hw + 80)
			path->deck[i] = 1;
	if (!PathArc(path, quarter, radius))
		return 0;
	PathSnap(path, 0, 1, 0);
	return PathStraight(path, south + radius + hw + 400);
}

/* The whole free-form road; returns the point count (0: failed). */
static int GenerateRoute(const tLevelGen *gen, int levelIndex, tPath *path, float *finishS)
{
	static const float kMinRadius[4] = {3000, 1100, 650, 420};
	static const float kMaxTheta[4] = {0.25f, 0.8f, 1.05f, 1.2f};
	float total = kLength[gen->length & 3], hw = RouteHalfWidth(gen);
	float minRadius = kMinRadius[gen->curves & 3], maxTheta = kMaxTheta[gen->curves & 3];
	int crossings = gen->crossings & 3, done = 0;
	memset(path, 0, sizeof(*path));
	path->hy = 1;
	SeedRandom(gen, levelIndex);
	if (!PathPush(path) || !PathStraight(path, 1500))
		goto failed;
	while (path->count * kRouteStep < total - 3500) {
		float s = path->count * kRouteStep;
		if (done < crossings && s >= total * (done + 1) / (crossings + 1)) {
			if (!PathLoop(path, hw, minRadius))
				goto failed;
			done++;
			continue;
		}
		if (RndI(0, 3) == 0) {
			if (!PathStraight(path, RndF(150, 700)))
				goto failed;
		} else {
			float target = RndF(-maxTheta, maxTheta), angle;
			if ((path->x > 4000 && target > 0) || (path->x < -4000 && target < 0))
				target = -target;
			angle = target - path->theta;
			if (Abs(angle) > 0.05f && !PathArc(path, angle, RndF(minRadius, minRadius * 2.5f)))
				goto failed;
		}
	}
	/* the finish: straight north, the road goes on a little after it */
	if (Abs(path->theta) > 0.01f && !PathArc(path, -path->theta, minRadius))
		goto failed;
	PathSnap(path, 0, 1, 0);
	if (!PathStraight(path, 3200))
		goto failed;
	*finishS = (path->count - 1) * kRouteStep - 2200;
	return path->count;
failed:
	free(path->points);
	free(path->deck);
	memset(path, 0, sizeof(*path));
	return 0;
}

/* Forks: a side road leaves the road to one side and joins it again,
   on stretches without bridges and away from the rest of the road. */
typedef struct {
	tRoutePoint *points;
	float *s;
	UInt8 *id;
	int count;
} tForks;

static void FreeForks(tForks *forks)
{
	free(forks->points);
	free(forks->s);
	free(forks->id);
	memset(forks, 0, sizeof(*forks));
}

static int MakeForks(const tLevelGen *gen, const tPath *path, float finishS, float hw, tForks *forks)
{
	int fork, wanted = gen->forks & 3, made = 0, capacity = 0;
	float used[4][2];
	memset(forks, 0, sizeof(*forks));
	for (fork = 0; fork < wanted; ++fork) {
		int tries;
		for (tries = 0; tries < 30; ++tries) {
			float s0 = RndF(2500, finishS - 5500), length = RndF(2200, 3600), s1 = s0 + length;
			float side = RndI(0, 1) ? 1.0f : -1.0f, offset = 2 * hw + RndF(260, 520), s;
			int i0 = (int)(s0 / kRouteStep), i1 = (int)(s1 / kRouteStep), i, k, ok = 1, start;
			for (k = 0; k < made && ok; ++k)
				if (s1 > used[k][0] - 1500 && s0 < used[k][1] + 1500)
					ok = 0;
			for (i = i0 - 300; i <= i1 + 300 && ok; ++i)	/* no bridge near it */
				if (i >= 0 && i < path->count && path->deck[i])
					ok = 0;
			if (!ok)
				continue;
			start = forks->count;
			for (s = s0; s <= s1; s += kRouteStep) {
				int j = (int)(s / kRouteStep);
				float t = (s - s0) / length, f = (1 - cosf(6.2831853f * t)) * 0.5f, lateral;
				const tRoutePoint *p;
				if (j >= path->count)
					break;
				p = &path->points[j];
				lateral = side * offset * f;
				if (forks->count == capacity) {
					capacity = capacity ? capacity * 2 : 4096;
					forks->points = (tRoutePoint *)realloc(forks->points, (size_t)capacity * sizeof(tRoutePoint));
					forks->s = (float *)realloc(forks->s, (size_t)capacity * sizeof(float));
					forks->id = (UInt8 *)realloc(forks->id, (size_t)capacity);
					if (!forks->points || !forks->s || !forks->id) {
						FreeForks(forks);
						return 0;
					}
				}
				forks->points[forks->count].x = p->x + p->hy * lateral;
				forks->points[forks->count].y = p->y - p->hx * lateral;
				forks->s[forks->count] = s;
				forks->id[forks->count] = (UInt8)(made + 1);
				forks->count++;
			}
			/* headings, and away from the rest of the road */
			for (i = start; i < forks->count && ok; ++i) {
				int a = i > start ? i - 1 : i, b = i + 1 < forks->count ? i + 1 : i;
				float hx = forks->points[b].x - forks->points[a].x, hy = forks->points[b].y - forks->points[a].y;
				float l = sqrtf(hx * hx + hy * hy);
				forks->points[i].hx = l > 0 ? hx / l : path->points[(int)(forks->s[i] / kRouteStep)].hx;
				forks->points[i].hy = l > 0 ? hy / l : path->points[(int)(forks->s[i] / kRouteStep)].hy;
				/* (where it leaves and joins it lies on the road by design:
				   only the part away from it is checked) */
				if ((i - start) % 10 || (1 - cosf(6.2831853f * (forks->s[i] - s0) / length)) * 0.5f * offset < 2 * hw)
					continue;
				for (k = 0; k < path->count; k += 8) {
					float dx, dy;
					if (k * kRouteStep > s0 - 900 && k * kRouteStep < s1 + 900)
						continue;
					dx = path->points[k].x - forks->points[i].x;
					dy = path->points[k].y - forks->points[i].y;
					if (dx * dx + dy * dy < (2 * hw + 120) * (2 * hw + 120)) {
						ok = 0;
						break;
					}
				}
			}
			if (!ok) {
				forks->count = start;
				continue;
			}
			used[made][0] = s0;
			used[made][1] = s1;
			made++;
			break;
		}
	}
	return 1;
}

/* Map for the menu: every step-th point of the road and where it is a
   bridge. */
int LevelGen_PreviewRoute(const tLevelGen *gen, float *xs, float *ys, UInt8 *bridge, int max)
{
	tPath path;
	float finishS;
	int i, n = 0, step;
	tForks forks;
	if (!GenerateRoute(gen, 0, &path, &finishS))
		return 0;
	MakeForks(gen, &path, finishS, RouteHalfWidth(gen), &forks);
	step = path.count / (max * 3 / 4) + 1;
	for (i = 0; i < path.count && n < max; i += step) {
		xs[n] = path.points[i].x;
		ys[n] = path.points[i].y;
		bridge[n] = path.deck[i];
		n++;
	}
	for (i = 0; i < forks.count && n < max; i += step) {
		xs[n] = forks.points[i].x;
		ys[n] = forks.points[i].y;
		bridge[n] = 0;
		n++;
	}
	free(path.points);
	free(path.deck);
	FreeForks(&forks);
	return n;
}

static int BuildRoute(const tLevelGen *gen, int levelIndex, const tBaseLevel *base, tRoadInfo *roadInfo)
{
	static const float kTrafficSpeed[4] = {14, 20, 27, 34};
	static const float kPace[4] = {190, 250, 300, 360};
	tPath path;
	float finishS, hw = RouteHalfWidth(gen), lane, length, s;
	int i, count, rows, row;
	tForks forks;
	if (!GenerateRoute(gen, levelIndex, &path, &finishS))
		return 0;
	if (!MakeForks(gen, &path, finishS, hw, &forks)) {
		free(path.points);
		free(path.deck);
		return 0;
	}
	gGenLanes = (gen->width & 3) >= 2 ? (gen->width & 3) : 1;
	/* route.c takes the points over */
	if (!Route_Setup(path.points, path.count, hw, path.deck, finishS,
		forks.points, forks.s, forks.id, forks.count))
		return 0;
	Route_SetMarks(gGenLanes, base->marks);
	length = Route_Length();
	lane = hw * (gGenLanes > 1 ? 0.5f : 0.45f);

	/* the old engine's one-road-per-row view: just the outline */
	rows = Route_Rows();
	sRoadCount = (UInt32)rows;
	sRoad = (tRoadSeg *)calloc((size_t)rows, sizeof(tRoadSeg));
	sSlope = (float *)calloc((size_t)rows, sizeof(float));
	if (!sRoad || !sSlope)
		return 0;
	for (row = 0; row < rows; ++row) {
		const SInt16 *ground, *bridge;
		int g = Route_RowSpans(row, 0, &ground), b = Route_RowSpans(row, 1, &bridge);
		SInt16 left = 32000, right = -32000;
		if (g) {
			left = ground[0];
			right = ground[2 * g - 1];
		}
		if (b) {
			if (bridge[0] < left) left = bridge[0];
			if (bridge[2 * b - 1] > right) right = bridge[2 * b - 1];
		}
		if (left > right)
			left = right = -32000;
		sRoad[row][0] = left;
		sRoad[row][1] = sRoad[row][2] = sRoad[row][3] = right;
	}

	/* traffic tracks along the road, slower where it bends */
	count = (int)(length / 150) + 2;
	sUp = (tTrackInfo *)calloc(1, sizeof(UInt32) + (size_t)count * sizeof(tTrackInfoSeg));
	sDown = (tTrackInfo *)calloc(1, sizeof(UInt32) + (size_t)count * sizeof(tTrackInfoSeg));
	if (!sUp || !sDown)
		return 0;
	for (s = 0; s <= length && (int)sUp->num < count; s += 150) {	/* kRouteTrackGap */
		t2DPoint pos, h1 = Route_Heading(s), h2 = Route_Heading(s + 150);
		float bend = 1 - (h1.x * h2.x + h1.y * h2.y), slow = sqrtf(bend * 2) * 1.2f;
		Route_Point(s, lane, &pos, NULL);
		sUp->track[sUp->num].x = (SInt16)pos.x;
		sUp->track[sUp->num].y = (SInt32)pos.y;
		sUp->track[sUp->num].velo = kTrafficSpeed[gen->speed & 3] * (1 - (slow > 0.5f ? 0.5f : slow));
		sUp->num++;
	}
	for (i = (int)sUp->num - 1; i >= 0; --i) {
		t2DPoint pos;
		Route_Point(i * 150.0f, -lane, &pos, NULL);
		sDown->track[sDown->num].x = (SInt16)pos.x;
		sDown->track[sDown->num].y = (SInt32)pos.y;
		sDown->track[sDown->num].velo = sUp->track[i].velo;
		sDown->track[sDown->num].flags = kTargetOvertake;
		sDown->num++;
	}

	sLevel = (tLevelData *)calloc(1, sizeof(tLevelData));
	if (!sLevel)
		return 0;
	{
		t2DPoint start, finish;
		float seconds = finishS / kPace[gen->time & 3] * (base->time ? base->time / 150.0f : 1);
		Route_Point(500, lane, &start, NULL);
		Route_Point(finishS, 0, &finish, NULL);
		sLevel->roadInfo = base->roadInfo;
		sLevel->xStartPos = (SInt16)start.x;
		sLevel->levelEnd = (UInt16)(finish.y < 65000 ? finish.y : 65000);
		sLevel->time = (UInt16)(seconds < 30 ? 30 : seconds > 900 ? 900 : seconds);
	}
	for (i = 0; i < 10; ++i)
		if (base->groups[i].resID) {
			sLevel->objGrps[i].resID = base->groups[i].resID;
			sLevel->objGrps[i].numObjs = ScaleGroup(base->groups[i], gen, finishS / 38500.0f);
		}
	gLevelData = sLevel;
	gMarks = NULL;
	gMarkSize = 0;
	gRoadInfo = roadInfo;
	gTrackUp = sUp;
	gTrackDown = sDown;
	gRoadLenght = &sRoadCount;
	gRoadData = sRoad;
	sLoaded = 1;

	/* the finish line across the road */
	{
		int finishTypes[2] = {184, 231}, pieces = (int)(2 * hw + 189) / 190, piece;
		for (i = 0; i < 2; ++i)
			for (piece = 0; piece < pieces; ++piece) {
				tObject *line = NewObject(gFirstObj, (SInt16)finishTypes[i]);
				if (line)
					Route_Point(finishS - i, -hw + 2 * hw * (piece + 0.5f) / pieces, &line->pos, NULL);
			}
	}
	/* ramps on long straight stretches */
	if (kJumps[gen->jumps & 3]) {
		int ramps = (int)(finishS / kJumps[gen->jumps & 3]), ramp;
		for (ramp = 0; ramp < ramps; ++ramp) {
			int tries, straight = 0;
			float at = 0;
			for (tries = 0; tries < 40 && !straight; ++tries) {
				float d;
				t2DPoint h0;
				at = RndF(2500, finishS - 3000);
				h0 = Route_Heading(at);
				straight = h0.y > 0.95f;
				for (d = -300; d <= 1800 && straight; d += 100) {
					t2DPoint h = Route_Heading(at + d);
					if (h.x * h0.x + h.y * h0.y < 0.9987f)
						straight = 0;
				}
			}
			if (straight) {
				int up = RndI(0, 2) != 0;
				tObject *obj = NewObject(gFirstObj, 188);
				float dir;
				if (obj) {
					Route_Point(at, up ? lane : -lane, &obj->pos, &dir);
					obj->dir = up ? dir : dir + 3.14159265f;
				}
			}
		}
	}
	return 1;
}

int LevelGen_Build(const tLevelGen *gen, int levelIndex)
{
	static const float kTrafficSpeed[4] = {14, 20, 27, 34};
	static const float kPace[4] = {190, 250, 300, 360};	/* road per second */
	tBaseLevel base;
	tRoadInfo *roadInfo;
	UInt16 levelEnd = kLength[gen->length & 3];
	int i, count, water = GenIsWater(gen->landscape);
	float lengthScale;

	LevelGen_Dispose();
	if (!ReadBaseLevel(gen->landscape, &base))
		return 0;
	roadInfo = (tRoadInfo *)GetSortedPackEntry(kPackRoad, base.roadInfo, NULL);
	if (!roadInfo)
		return 0;
	if ((gen->crossings || gen->forks) && !water) {	/* a free-form road: crossings, forks */
		if (!BuildRoute(gen, levelIndex, &base, roadInfo)) {
			LevelGen_Dispose();
			return 0;
		}
		return 1;
	}
	if (!GenerateRoad(gen, levelIndex)) {
		LevelGen_Dispose();
		return 0;
	}
	/* Tracks for the traffic: every 300 pixels, slower in bends. */
	count = (levelEnd + 2000) / 300 + 3;
	sUp = (tTrackInfo *)calloc(1, sizeof(UInt32) + (size_t)count * sizeof(tTrackInfoSeg));
	sDown = (tTrackInfo *)calloc(1, sizeof(UInt32) + (size_t)count * sizeof(tTrackInfoSeg));
	if (!sUp || !sDown) {
		LevelGen_Dispose();
		return 0;
	}
	for (i = 0; i < count; ++i) {
		float y = -8 + i * 300.0f, bend, velo;
		if (y > levelEnd + 2000)
			break;
		bend = Abs(SlopeAt(y + 300) - SlopeAt(y));
		velo = kTrafficSpeed[gen->speed] * (1 - (bend * 1.6f > 0.45f ? 0.45f : bend * 1.6f));
		sUp->track[sUp->num].x = (SInt16)UpLaneX(*RoadAt(y));
		sUp->track[sUp->num].y = (SInt32)y;
		sUp->track[sUp->num].velo = velo;
		sUp->num++;
	}
	for (i = (int)sUp->num - 1; i >= 0; --i) {
		float y = (float)sUp->track[i].y;
		sDown->track[sDown->num].x = (SInt16)DownLaneX(*RoadAt(y));
		sDown->track[sDown->num].y = (SInt32)y;
		sDown->track[sDown->num].velo = sUp->track[i].velo;
		sDown->track[sDown->num].flags = kTargetOvertake;
		sDown->num++;
	}

	/* Lane marks, where the original world has them: dashes down the
	   middle of a road and of a wide carriageway. Sorted from the top. */
	gMarkSize = 0;
	if (base.marks && !water) {
		int dashes = (levelEnd + 2000) / 120 + 1, dash;
		sMarks = (tMarkSeg *)calloc((size_t)dashes * 6, sizeof(tMarkSeg));
		if (!sMarks) {
			LevelGen_Dispose();
			return 0;
		}
		for (dash = dashes - 1; dash >= 0; --dash) {
			float y1 = dash * 120.0f + 50, y2 = dash * 120.0f;
			const tRoadSeg *a = RoadAt(y1), *b = RoadAt(y2);
			float lines[6][2];
			int line, n = 0, side;
			if (Single(*a) != Single(*b))
				continue;	/* where a median starts or ends */
			if (Single(*a)) {
				/* the middle, and the lanes of each half */
				int lanes = (int)(((*a)[3] - (*a)[0]) * 0.5f / 80);
				if (lanes > gGenLanes)
					lanes = gGenLanes;
				if (lanes < 1)
					lanes = 1;
				lines[n][0] = ((*a)[0] + (*a)[3]) * 0.5f;
				lines[n++][1] = ((*b)[0] + (*b)[3]) * 0.5f;
				for (side = 0; side < 2; ++side) {
					int k;
					for (k = 1; k < lanes && n < 6; ++k) {
						float t = (side ? 0.5f + 0.5f * k / lanes : 0.5f * k / lanes);
						lines[n][0] = (*a)[0] + ((*a)[3] - (*a)[0]) * t;
						lines[n++][1] = (*b)[0] + ((*b)[3] - (*b)[0]) * t;
					}
				}
			} else {
				/* lane lines on each carriageway (one line on a wide
				   carriageway of an ordinary road) */
				for (side = 0; side < 2; ++side) {
					int l = side ? 2 : 0, r = side ? 3 : 1, lanes, k;
					float width = (float)((*a)[r] - (*a)[l]);
					lanes = gGenLanes > 1 ? (int)(width / 80) : (width >= 180 ? 2 : 1);
					if (gGenLanes > 1 && lanes > gGenLanes)
						lanes = gGenLanes;
					for (k = 1; k < lanes && n < 6; ++k) {
						lines[n][0] = (*a)[l] + width * k / lanes;
						lines[n++][1] = (*b)[l] + ((*b)[r] - (*b)[l]) * (float)k / lanes;
					}
				}
			}
			for (line = 0; line < n; ++line) {
				sMarks[gMarkSize].p1 = P2D(lines[line][0], y1);
				sMarks[gMarkSize].p2 = P2D(lines[line][1], y2);
				gMarkSize++;
			}
		}
	}

	/* The level: the world's surface and groups, the time from the pace. */
	sLevel = (tLevelData *)calloc(1, sizeof(tLevelData));
	if (!sLevel) {
		LevelGen_Dispose();
		return 0;
	}
	lengthScale = levelEnd / 38500.0f;
	sLevel->roadInfo = base.roadInfo;
	sLevel->levelEnd = levelEnd;
	sLevel->xStartPos = (SInt16)UpLaneX(*RoadAt(500));
	{
		float seconds = levelEnd / kPace[gen->time & 3] * (base.time ? base.time / 150.0f : 1);
		sLevel->time = (UInt16)(seconds < 30 ? 30 : seconds > 900 ? 900 : seconds);
	}
	for (i = 0; i < 10; ++i)
		if (base.groups[i].resID) {
			sLevel->objGrps[i].resID = base.groups[i].resID;
			sLevel->objGrps[i].numObjs = ScaleGroup(base.groups[i], gen, lengthScale);
		}

	gLevelData = sLevel;
	gMarks = sMarks;
	gRoadInfo = roadInfo;
	gTrackUp = sUp;
	gTrackDown = sDown;
	gRoadLenght = &sRoadCount;
	gRoadData = sRoad;
	sLoaded = 1;

	/* The finish line, and ramps on straight stretches. */
	{
		const tRoadSeg *end = RoadAt(levelEnd);
		if (water) {
			tObject *left = NewObject(gFirstObj, 239), *right = NewObject(gFirstObj, 239);
			if (left)
				left->pos = P2D((*end)[0] + 30, levelEnd);
			if (right)
				right->pos = P2D((*end)[3] - 30, levelEnd);
		} else {
			/* across the whole road: as many pieces as it is wide */
			int finish[2] = {184, 231}, pieces = ((*end)[3] - (*end)[0] + 189) / 190, piece;
			if (pieces < 1)
				pieces = 1;
			for (i = 0; i < 2; ++i)
				for (piece = 0; piece < pieces; ++piece) {
					tObject *line = NewObject(gFirstObj, (SInt16)finish[i]);
					if (line)
						line->pos = P2D((*end)[0] + ((*end)[3] - (*end)[0]) * (piece + 0.5f) / pieces,
							levelEnd - i);
				}
		}
	}
	{
		float heights[32];
		UInt8 up[32];
		int ramps = PlaceRamps(gen, heights, up, 32), ramp;
		for (ramp = 0; ramp < ramps; ++ramp) {
			const tRoadSeg *seg = RoadAt(heights[ramp]);
			tObject *obj = NewObject(gFirstObj, water ? 238 : 188);
			if (obj) {
				obj->pos = P2D(up[ramp] ? UpLaneX(*seg) : DownLaneX(*seg), heights[ramp]);
				obj->dir = up[ramp] ? 0 : 3.14159265f;
			}
		}
	}
	sBridgeCount = PlaceBridges(gen, sBridges, 16);
	return 1;
}

/* The whole road of a generator's first level, sampled for the menu's map:
   edges and medians at columns from start to finish, and the ramps. */
int LevelGen_Preview(const tLevelGen *gen, int columns, float *left, float *right,
	UInt8 *divided, float *ramps, int maxRamps, int *rampCount, float *bridges, int *bridgeCount)
{
	float heights[32], bridgeHeights[16];
	int bridgesHere;
	UInt8 up[32];
	int column, count, i;
	UInt16 levelEnd = kLength[gen->length & 3];
	if (sLoaded || columns < 2)
		return 0;	/* a generated level is being played */
	if (!GenerateRoad(gen, 0)) {
		LevelGen_Dispose();
		return 0;
	}
	count = PlaceRamps(gen, heights, up, 32);
	bridgesHere = PlaceBridges(gen, bridgeHeights, 16);
	*bridgeCount = 0;
	for (i = 0; i < bridgesHere; ++i)
		bridges[(*bridgeCount)++] = bridgeHeights[i] / levelEnd;
	for (column = 0; column < columns; ++column) {
		const tRoadSeg *seg = RoadAt(levelEnd * (float)column / (columns - 1));
		left[column] = (*seg)[0];
		right[column] = (*seg)[3];
		divided[column] = !Single(*seg);
	}
	*rampCount = 0;
	for (i = 0; i < count && i < maxRamps; ++i)
		ramps[(*rampCount)++] = heights[i] / levelEnd;
	LevelGen_Dispose();
	return 1;
}

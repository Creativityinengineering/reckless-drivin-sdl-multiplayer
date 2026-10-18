#include "route.h"
#include "objects.h"
#include "screen.h"
#include "pixel.h"
#include "roads.h"
#include "textrender.h"
#include "platform.h"
#include "random.h"
#include "netgame.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int gRouteActive;
int gRouteDrawPass = -1;

static tRoutePoint *sPoints;
static UInt8 *sDeck;
static float *sHeight;	/* the bridges' height along the road (see DeckHeights) */
/* Forks: side roads that leave the road and join it again. Their points,
   how far along the main road each one counts, and which fork it is. */
static tRoutePoint *sBranch;
static float *sBranchS;
static UInt8 *sBranchId;
static int sBranchCount;

/* Highways: extra roads (their carriageways) with their own half width
   and lanes, the stretches of the road that run on a highway (where the
   oncoming traffic is on the other carriageway, sep to the left), and road
   signs. Handed over before Route_Setup ("pending"), active after it. */
#define kMaxExtraRoads 32
#define kMaxHighways 4
#define kMaxSigns 16
typedef struct {
	tRoutePoint *points;
	UInt8 *deck;	/* where it runs on a bridge (NULL: nowhere) */
	int count, lanes;
	float half;
	float *height;	/* how high its bridges are (0 at their ends, 1 up), from deck */
} tExtraRoad;
typedef struct {
	float x, y, dir;	/* dir: the way the arrow points (heading) */
	char text[16];
} tSign;
static tExtraRoad sExtra[kMaxExtraRoads], sPendingExtra[kMaxExtraRoads];
static int sExtraCount, sPendingExtraCount;
static float sHighS0[kMaxHighways], sHighS1[kMaxHighways], sHighSep;
static int sHighCount;
static float sPendingHighS0[kMaxHighways], sPendingHighS1[kMaxHighways], sPendingHighSep;
static int sPendingHighCount;
static tSign sSigns[kMaxSigns], sPendingSigns[kMaxSigns];
static int sKeepTracks;	/* (see Route_KeepHighwayCars) */
/* Highway junctions: per highway its own two traffic tracks (northbound,
   southbound) and where on them the exit (north) and entry (south) are. */
typedef struct {
	void *north, *south;
	int exitIndex, entryIndex;
	/* the road's other direction: ramps over the north carriageway, from
	   the south one (at its point rampFrom) down to the road, and from the
	   road (at exitRampS along it) onto the south one */
	void *entryRamp, *exitRamp;
	int rampFrom;
	float exitRampS;
} tJunction;
static tJunction sJunction[kMaxHighways], sPendingJunction[kMaxHighways];
static int sSignCount, sPendingSignCount;
/* Tunnels where a highway ends: the end point (middle of the road), the
   way into the tunnel, how wide and how deep the portal is. */
typedef struct {
	float x, y, dx, dy, width, depth;
} tTunnel;
#define kMaxTunnels 8
static tTunnel sTunnels[kMaxTunnels], sPendingTunnels[kMaxTunnels];
static int sTunnelCount, sPendingTunnelCount;

void Route_PendingTunnel(float x, float y, float dx, float dy, float width, float depth)
{
	if (sPendingTunnelCount >= kMaxTunnels)
		return;
	sPendingTunnels[sPendingTunnelCount].x = x;
	sPendingTunnels[sPendingTunnelCount].y = y;
	sPendingTunnels[sPendingTunnelCount].dx = dx;
	sPendingTunnels[sPendingTunnelCount].dy = dy;
	sPendingTunnels[sPendingTunnelCount].width = width;
	sPendingTunnels[sPendingTunnelCount].depth = depth;
	sPendingTunnelCount++;
}

void Route_PendingExtraRoad(tRoutePoint *points, UInt8 *deck, int count, float half, int lanes)
{
	if (sPendingExtraCount >= kMaxExtraRoads) {
		free(points);
		free(deck);
		return;
	}
	sPendingExtra[sPendingExtraCount].points = points;
	sPendingExtra[sPendingExtraCount].deck = deck;
	sPendingExtra[sPendingExtraCount].count = count;
	sPendingExtra[sPendingExtraCount].half = half;
	sPendingExtra[sPendingExtraCount].lanes = lanes;
	sPendingExtra[sPendingExtraCount].height = NULL;
	sPendingExtraCount++;
}

void Route_PendingHighway(float s0, float s1, float sep, void *north, void *south,
	int exitIndex, int entryIndex)
{
	if (sPendingHighCount >= kMaxHighways)
		return;
	sPendingJunction[sPendingHighCount].north = north;
	sPendingJunction[sPendingHighCount].south = south;
	sPendingJunction[sPendingHighCount].exitIndex = exitIndex;
	sPendingJunction[sPendingHighCount].entryIndex = entryIndex;
	sPendingJunction[sPendingHighCount].entryRamp = sPendingJunction[sPendingHighCount].exitRamp = NULL;
	sPendingHighS0[sPendingHighCount] = s0;
	sPendingHighS1[sPendingHighCount] = s1;
	sPendingHighSep = sep;
	sPendingHighCount++;
}

void Route_PendingRamps(void *entryRamp, int rampFrom, void *exitRamp, float exitRampS)
{
	tJunction *j;
	if (sPendingHighCount < 1)
		return;
	j = &sPendingJunction[sPendingHighCount - 1];
	j->entryRamp = entryRamp;
	j->rampFrom = rampFrom;
	j->exitRamp = exitRamp;
	j->exitRampS = exitRampS;
}

void Route_PendingSign(float x, float y, float dir, const char *text)
{
	if (sPendingSignCount >= kMaxSigns)
		return;
	sPendingSigns[sPendingSignCount].x = x;
	sPendingSigns[sPendingSignCount].y = y;
	sPendingSigns[sPendingSignCount].dir = dir;
	strncpy(sPendingSigns[sPendingSignCount].text, text, sizeof(sSigns[0].text) - 1);
	sPendingSigns[sPendingSignCount].text[sizeof(sSigns[0].text) - 1] = 0;
	sPendingSignCount++;
}

static int sPendingCrossingCount;	/* (highway bridges, see below) */

void Route_DropPending(void)
{
	int i;
	for (i = 0; i < sPendingExtraCount; ++i) {
		free(sPendingExtra[i].points);
		free(sPendingExtra[i].deck);
	}
	sPendingExtraCount = sPendingHighCount = sPendingSignCount = sPendingCrossingCount = 0;
	sPendingTunnelCount = 0;
}

static float sHalfWidth;	/* (defined below) */

/* How far to the side of the road's center line traffic drives at s: up
   (the road's way) on the right, down on the left; on a highway stretch
   the oncoming traffic is on the other carriageway. */
float Route_TrafficOffset(float s, int up)
{
	float lane = sHalfWidth * 0.45f;
	int i;
	if (up)
		return lane;
	for (i = 0; i < sHighCount; ++i) {
		float into = s - sHighS0[i], left = sHighS1[i] - s, t;
		if (into < 0 || left < 0)
			continue;
		t = (into < left ? into : left) / 400;	/* blend in over the junctions */
		if (t > 1)
			t = 1;
		return -(lane + (sHighSep - lane) * t);
	}
	return -lane;
}
static int sCount, sLanes, sMarks;
static float sHalfWidth, sFinishS;

/* Road pieces per row of the world (2 pixels high), ground and bridges. */
static int sRows;
static int *sRowStart[2];
static UInt16 *sRowCount[2];
static SInt16 *sSpans[2];

/* Where the road crosses itself: a box around the crossing, the bridge's
   direction and the direction of the road below. */
typedef struct {
	float x, y, half;
	float bridgeX, bridgeY;	/* heading of the road on the bridge */
	float belowX, belowY;	/* heading of the road below */
	float deckHalf;	/* half the bridge's length */
	float acrossHalf;	/* half the bridge's width */
	UInt8 ramp;	/* a highway ramp's bridge (curved): its own traffic on top */
	float fromS, toS, belowS;	/* the road over itself: its bridge's stretch, the part below (else -1) */
	int extra;	/* a ramp's bridge: the ramp (extra road), else -1 */
} tCrossing;
#define kMaxCrossings 24
static tCrossing sCrossings[kMaxCrossings];
/* Highway bridges over the road, handed over before Route_Setup. */
static tCrossing sPendingCrossings[kMaxCrossings];
static int sPendingCrossingCount;

void Route_PendingCrossing(float x, float y, float bridgeX, float bridgeY, float belowX, float belowY,
	float deckHalf, float acrossHalf)
{
	tCrossing *c;
	if (sPendingCrossingCount >= kMaxCrossings)
		return;
	c = &sPendingCrossings[sPendingCrossingCount++];
	c->x = x;
	c->y = y;
	c->bridgeX = bridgeX;
	c->bridgeY = bridgeY;
	c->belowX = belowX;
	c->belowY = belowY;
	c->deckHalf = deckHalf;
	c->acrossHalf = acrossHalf;
	c->half = (deckHalf > acrossHalf ? deckHalf : acrossHalf) + 150;
	c->ramp = 0;
	c->fromS = c->toS = c->belowS = -1;
	c->extra = -1;
}

void Route_PendingRampCrossing(float x, float y, float bridgeX, float bridgeY, float belowX, float belowY,
	float deckHalf, float acrossHalf)
{
	Route_PendingCrossing(x, y, bridgeX, bridgeY, belowX, belowY, deckHalf, acrossHalf);
	if (sPendingCrossingCount > 0) {
		sPendingCrossings[sPendingCrossingCount - 1].ramp = 1;
		sPendingCrossings[sPendingCrossingCount - 1].extra = sPendingExtraCount;	/* (the ramp comes next) */
	}
}
static int sCrossingCount;
static int sCamKnown[16];

void Route_Dispose(void)
{
	int layer;
	free(sPoints);
	free(sDeck);
	free(sHeight);
	sHeight = NULL;
	free(sBranch);
	free(sBranchS);
	free(sBranchId);
	sPoints = NULL;
	sDeck = NULL;
	sBranch = NULL;
	sBranchS = NULL;
	sBranchId = NULL;
	sBranchCount = 0;
	{
		int k;
		for (k = 0; k < sExtraCount; ++k) {
			free(sExtra[k].points);
			free(sExtra[k].deck);
			free(sExtra[k].height);
		}
		sExtraCount = sHighCount = sSignCount = sTunnelCount = 0;
		sKeepTracks = 0;
	}
	for (layer = 0; layer < 2; ++layer) {
		free(sRowStart[layer]);
		free(sRowCount[layer]);
		free(sSpans[layer]);
		sRowStart[layer] = NULL;
		sRowCount[layer] = NULL;
		sSpans[layer] = NULL;
	}
	sCount = sRows = sCrossingCount = 0;
	gRouteActive = 0;
}

float Route_Length(void)
{
	return sCount > 1 ? (sCount - 1) * kRouteStep : 0;
}

float Route_FinishS(void)
{
	return sFinishS;
}

float Route_HalfWidth(void)
{
	return sHalfWidth;
}

int Route_Rows(void)
{
	return sRows;
}

static int IndexAt(float s)
{
	int i = (int)(s / kRouteStep + 0.5f);
	if (i < 0)
		i = 0;
	if (i >= sCount)
		i = sCount - 1;
	return i;
}

void Route_Point(float s, float lateral, t2DPoint *pos, float *dir)
{
	const tRoutePoint *p;
	if (!sCount) {
		*pos = P2D(0, 0);
		if (dir)
			*dir = 0;
		return;
	}
	p = &sPoints[IndexAt(s)];
	/* right of the heading (hx, hy) is (hy, -hx) */
	*pos = P2D(p->x + p->hy * lateral, p->y - p->hx * lateral);
	if (dir)
		*dir = atan2f(p->hx, p->hy);
}

t2DPoint Route_Heading(float s)
{
	if (!sCount)
		return P2D(0, 1);
	return P2D(sPoints[IndexAt(s)].hx, sPoints[IndexAt(s)].hy);
}

/* The nearest point of the road (main road or a fork) to pos: how far along
   the road it counts and its squared distance; with hint >= 0 only near
   the hint. */
static float NearestS(t2DPoint pos, float hint, float *distance)
{
	int i, from = 0, to = sCount - 1, step = 1, best = 0;
	float bestDistance = 1e30f, bestS;
	if (!sCount) {
		*distance = 1e30f;
		return 0;
	}
	if (hint >= 0) {
		from = IndexAt(hint) - 400;
		to = IndexAt(hint) + 400;
		if (from < 0)
			from = 0;
		if (to > sCount - 1)
			to = sCount - 1;
	} else
		step = 8;
	for (i = from; i <= to; i += step) {
		float dx = sPoints[i].x - pos.x, dy = sPoints[i].y - pos.y, d = dx * dx + dy * dy;
		if (d < bestDistance) {
			bestDistance = d;
			best = i;
		}
	}
	if (step > 1) {	/* refine around the coarse match */
		for (i = best - 8; i <= best + 8; ++i) {
			float dx, dy, d;
			if (i < 0 || i >= sCount)
				continue;
			dx = sPoints[i].x - pos.x;
			dy = sPoints[i].y - pos.y;
			d = dx * dx + dy * dy;
			if (d < bestDistance) {
				bestDistance = d;
				best = i;
			}
		}
	}
	bestS = best * kRouteStep;
	for (i = 0; i < sBranchCount; i += step) {
		float dx, dy, d;
		if (hint >= 0 && fabsf(sBranchS[i] - hint) > 800)
			continue;
		dx = sBranch[i].x - pos.x;
		dy = sBranch[i].y - pos.y;
		d = dx * dx + dy * dy;
		if (d < bestDistance) {
			bestDistance = d;
			bestS = sBranchS[i];
		}
	}
	*distance = bestDistance;
	return bestS;
}

float Route_Nearest(t2DPoint pos, float hint)
{
	float distance;
	return NearestS(pos, hint, &distance);
}

int Route_RowSpans(int row, int layer, const SInt16 **spans)
{
	if (!gRouteActive || row < 0 || row >= sRows || layer < 0 || layer > 1) {
		*spans = NULL;
		return 0;
	}
	*spans = sSpans[layer] + sRowStart[layer][row] * 2;
	return sRowCount[layer][row];
}

static int InsideRow(int row, float x, float *distance)
{
	int layer, i;
	float best = 1e9f;
	for (layer = 0; layer < 2; ++layer) {
		const SInt16 *spans;
		int count = Route_RowSpans(row, layer, &spans);
		for (i = 0; i < count; ++i) {
			float left = spans[2 * i], right = spans[2 * i + 1];
			if (x >= left && x <= right) {
				*distance = 0;
				return 1;
			}
			if (left - x < best && left > x)
				best = left - x;
			if (x - right < best && x > right)
				best = x - right;
		}
	}
	*distance = best;
	return 0;
}

int Route_BackCollision(t2DPoint pos, int tolerance)
{
	int row = (int)(pos.y / 2), dy;
	float distance, nearest;
	if (InsideRow(row, pos.x, &distance))
		return 0;
	nearest = distance;
	/* edges that run across (east or west stretches): look up and down */
	for (dy = 2; dy <= tolerance && nearest > dy; dy += 2) {
		float d;
		if (InsideRow(row + dy / 2, pos.x, &d) || InsideRow(row - dy / 2, pos.x, &d)) {
			nearest = (float)dy;
			break;
		}
	}
	return nearest > tolerance ? 2 : 1;
}

int Route_SpanAt(float x, float y, float *left, float *right)
{
	int row = (int)(y / 2), layer, i;
	for (layer = 1; layer >= 0; --layer) {
		const SInt16 *spans;
		int count = Route_RowSpans(row, layer, &spans);
		for (i = 0; i < count; ++i)
			if (x >= spans[2 * i] && x <= spans[2 * i + 1]) {
				*left = spans[2 * i];
				*right = spans[2 * i + 1];
				return 1;
			}
	}
	return 0;
}

int Route_Level(t2DPoint pos, float dir)
{
	int i;
	for (i = 0; i < sCrossingCount; ++i) {
		const tCrossing *c = &sCrossings[i];
		float hx = sinf(dir), hy = cosf(dir), alongBridge, alongBelow;
		if (fabsf(pos.x - c->x) > c->half || fabsf(pos.y - c->y) > c->half)
			continue;
		alongBridge = fabsf(hx * c->bridgeX + hy * c->bridgeY);
		alongBelow = fabsf(hx * c->belowX + hy * c->belowY);
		return alongBridge > alongBelow;
	}
	return 0;
}

int Route_UpdateLevel(void *object);

int Route_ObjectLevel(const void *object)
{
	const tObject *obj = (const tObject *)object;
	if (!gRouteActive || !obj)
		return 0;
	if (obj->shooter && obj->type->flags2 & kObjectMissile)	/* a weapon: its shooter's level */
		obj = (const tObject *)obj->shooter;
	/* the level it took when it came into the crossing (see below) */
	Route_UpdateLevel((void *)obj);
	if (obj->onBridge)
		return obj->onBridge == 1;
	return 0;
}

/* Apart only when one is on a bridge and the other below it; outside the
   crossings an object meets both (a car just before a bridge and one
   already on it are on the same road). */
static int RawLevel(const tObject *obj)
{
	if (obj->shooter && obj->type->flags2 & kObjectMissile)
		obj = (const tObject *)obj->shooter;
	Route_UpdateLevel((void *)obj);
	return obj->onBridge == 3 ? 2 : obj->onBridge;
}

int Route_SameLevel(const void *a, const void *b)
{
	int la, lb;
	if (!gRouteActive || !a || !b)
		return 1;
	la = RawLevel((const tObject *)a);
	lb = RawLevel((const tObject *)b);
	return !la || !lb || la == lb;
}

/* ---- Setup: road pieces per row from the center line ---- */

typedef struct {
	int row;
	SInt16 left, right;
} tPiece;

static int ComparePieces(const void *a, const void *b)
{
	const tPiece *p = (const tPiece *)a, *q = (const tPiece *)b;
	if (p->row != q->row)
		return p->row - q->row;
	return p->left - q->left;
}

/* The x range a quad (corners in order) covers at height y. */
static int QuadAt(const float *qx, const float *qy, float y, float *left, float *right)
{
	int i, hits = 0;
	*left = 1e9f;
	*right = -1e9f;
	for (i = 0; i < 4; ++i) {
		int j = (i + 1) & 3;
		float y0 = qy[i], y1 = qy[j], x;
		if ((y < y0 && y < y1) || (y > y0 && y > y1) || y0 == y1)
			continue;
		x = qx[i] + (qx[j] - qx[i]) * (y - y0) / (y1 - y0);
		if (x < *left)
			*left = x;
		if (x > *right)
			*right = x;
		hits++;
	}
	return hits >= 2;
}

static int AddSegment(const tRoutePoint *a, const tRoutePoint *b, float half, int rows,
	tPiece **pieces, size_t *count, size_t *capacity)
{
	float qx[4], qy[4], low, high, y;
	int row0, row1, row;
	qx[0] = a->x - a->hy * half; qy[0] = a->y + a->hx * half;
	qx[1] = a->x + a->hy * half; qy[1] = a->y - a->hx * half;
	qx[2] = b->x + b->hy * half; qy[2] = b->y - b->hx * half;
	qx[3] = b->x - b->hy * half; qy[3] = b->y + b->hx * half;
	low = high = qy[0];
	for (row = 1; row < 4; ++row) {
		if (qy[row] < low) low = qy[row];
		if (qy[row] > high) high = qy[row];
	}
	row0 = (int)floorf(low / 2);
	row1 = (int)ceilf(high / 2);
	for (row = row0; row <= row1; ++row) {
		float left, right;
		if (row < 0 || row >= rows)
			continue;
		y = row * 2.0f + 1;
		if (y < low)
			y = low;
		if (y > high)
			y = high;
		if (!QuadAt(qx, qy, y, &left, &right))
			continue;
		if (*count == *capacity) {
			tPiece *grown;
			*capacity = *capacity ? *capacity * 2 : 65536;
			grown = (tPiece *)realloc(*pieces, *capacity * sizeof(tPiece));
			if (!grown)
				return 0;
			*pieces = grown;
		}
		(*pieces)[*count].row = row;
		(*pieces)[*count].left = (SInt16)floorf(left - 1);
		(*pieces)[*count].right = (SInt16)ceilf(right + 1);
		(*count)++;
	}
	return 1;
}

static int BuildLayer(int layer, int rows)
{
	tPiece *pieces = NULL;
	size_t count = 0, capacity = 0, i;
	int k, out = 0;
	for (k = 0; k + 1 < sCount; ++k) {
		if ((sDeck[k] != 0) != (layer == 1))
			continue;
		if (!AddSegment(&sPoints[k], &sPoints[k + 1], sHalfWidth, rows, &pieces, &count, &capacity)) {
			free(pieces);
			return 0;
		}
	}
	for (k = 0; layer == 0 && k + 1 < sBranchCount; ++k) {	/* forks: on the ground */
		if (sBranchId[k] != sBranchId[k + 1])
			continue;
		if (!AddSegment(&sBranch[k], &sBranch[k + 1], sHalfWidth, rows, &pieces, &count, &capacity)) {
			free(pieces);
			return 0;
		}
	}
	{	/* highways: on the ground, or on their bridges */
		int e;
		for (e = 0; e < sExtraCount; ++e)
			for (k = 0; k + 1 < sExtra[e].count; ++k)
				if (((sExtra[e].deck && sExtra[e].deck[k]) != 0) == (layer == 1) && !AddSegment(&sExtra[e].points[k], &sExtra[e].points[k + 1], sExtra[e].half, rows,
					&pieces, &count, &capacity)) {
					free(pieces);
					return 0;
				}
	}
	qsort(pieces, count, sizeof(tPiece), ComparePieces);
	sRowStart[layer] = (int *)calloc((size_t)rows + 1, sizeof(int));
	sRowCount[layer] = (UInt16 *)calloc((size_t)rows + 1, sizeof(UInt16));
	sSpans[layer] = (SInt16 *)malloc((count ? count : 1) * 2 * sizeof(SInt16));
	if (!sRowStart[layer] || !sRowCount[layer] || !sSpans[layer]) {
		free(pieces);
		return 0;
	}
	/* merge what touches, row by row */
	for (i = 0; i < count;) {
		int row = pieces[i].row;
		sRowStart[layer][row] = out;
		while (i < count && pieces[i].row == row) {
			SInt16 left = pieces[i].left, right = pieces[i].right;
			i++;
			while (i < count && pieces[i].row == row && pieces[i].left <= right + 2) {
				if (pieces[i].right > right)
					right = pieces[i].right;
				i++;
			}
			sSpans[layer][2 * out] = left;
			sSpans[layer][2 * out + 1] = right;
			out++;
			sRowCount[layer][row]++;
		}
	}
	free(pieces);
	return 1;
}

/* A bridge rises from the road at its ends: its height along it, 0 at
   the ends to 1 a little way in (for its shadow). */
static float *DeckHeights(const UInt8 *deck, int count)
{
	float *height;
	int i = 0;
	if (!deck || count <= 0)
		return NULL;
	height = (float *)calloc((size_t)count, sizeof(float));
	if (!height)
		return NULL;
	while (i < count) {
		int from, to, k;
		if (!deck[i]) {
			i++;
			continue;
		}
		from = i;
		while (i < count && deck[i])
			i++;
		to = i - 1;
		for (k = from; k <= to; ++k) {
			float d = (k - from < to - k ? k - from : to - k) * kRouteStep / 110.0f;
			height[k] = d > 1 ? 1 : d;
		}
	}
	return height;
}

int Route_Setup(tRoutePoint *points, int count, float halfWidth, UInt8 *deck, float finishS,
	tRoutePoint *branch, float *branchS, UInt8 *branchId, int branchCount)
{
	float maxY = 0;
	int i;
	Route_Dispose();
	if (!points || !deck || count < 2) {
		free(points);
		free(deck);
		free(branch);
		free(branchS);
		free(branchId);
		return 0;
	}
	memcpy(sExtra, sPendingExtra, sizeof(sExtra));
	sExtraCount = sPendingExtraCount;
	memcpy(sHighS0, sPendingHighS0, sizeof(sHighS0));
	memcpy(sHighS1, sPendingHighS1, sizeof(sHighS1));
	sHighSep = sPendingHighSep;
	sHighCount = sPendingHighCount;
	memcpy(sJunction, sPendingJunction, sizeof(sJunction));
	memcpy(sSigns, sPendingSigns, sizeof(sSigns));
	sSignCount = sPendingSignCount;
	memcpy(sTunnels, sPendingTunnels, sizeof(sTunnels));
	sTunnelCount = sPendingTunnelCount;
	sPendingTunnelCount = 0;
	sPendingExtraCount = sPendingHighCount = sPendingSignCount = 0;
	for (i = 0; i < sExtraCount; ++i) {
		int k;
		for (k = 0; k < sExtra[i].count; ++k)
			if (sExtra[i].points[k].y > maxY)
				maxY = sExtra[i].points[k].y;
	}
	sBranch = branch;
	sBranchS = branchS;
	sBranchId = branchId;
	sBranchCount = branch && branchS && branchId ? branchCount : 0;
	for (i = 0; i < sBranchCount; ++i)
		if (branch[i].y > maxY)
			maxY = branch[i].y;
	for (i = 0; i < sExtraCount; ++i)
		sExtra[i].height = DeckHeights(sExtra[i].deck, sExtra[i].count);
	sPoints = points;
	sDeck = deck;
	sHeight = DeckHeights(deck, count);
	sCount = count;
	sHalfWidth = halfWidth;
	sFinishS = finishS;
	for (i = 0; i < count; ++i)
		if (points[i].y > maxY)
			maxY = points[i].y;
	sRows = (int)(maxY + halfWidth) / 2 + 4;
	if (!BuildLayer(0, sRows) || !BuildLayer(1, sRows)) {
		Route_Dispose();
		return 0;
	}
	/* the crossings: each run of bridge points, and the road below it */
	for (i = 0; i < count && sCrossingCount < kMaxCrossings;) {
		int from, to, mid, j, below = -1;
		float bestDistance = 1e30f;
		if (!deck[i]) {
			i++;
			continue;
		}
		from = i;
		while (i < count && deck[i])
			i++;
		to = i - 1;
		mid = (from + to) / 2;
		for (j = 0; j < count; ++j) {
			float dx, dy, d;
			if (j >= from - 200 && j <= to + 200)
				continue;
			dx = points[j].x - points[mid].x;
			dy = points[j].y - points[mid].y;
			d = dx * dx + dy * dy;
			if (d < bestDistance) {
				bestDistance = d;
				below = j;
			}
		}
		if (below < 0)
			continue;
		sCrossings[sCrossingCount].x = points[mid].x;
		sCrossings[sCrossingCount].y = points[mid].y;
		sCrossings[sCrossingCount].half = halfWidth + 120;
		sCrossings[sCrossingCount].bridgeX = points[mid].hx;
		sCrossings[sCrossingCount].bridgeY = points[mid].hy;
		sCrossings[sCrossingCount].belowX = points[below].hx;
		sCrossings[sCrossingCount].belowY = points[below].hy;
		sCrossings[sCrossingCount].deckHalf = (to - from) * kRouteStep * 0.5f;
		sCrossings[sCrossingCount].acrossHalf = halfWidth;
		sCrossings[sCrossingCount].ramp = 0;
		sCrossings[sCrossingCount].fromS = from * kRouteStep;
		sCrossings[sCrossingCount].toS = to * kRouteStep;
		sCrossings[sCrossingCount].belowS = below * kRouteStep;
		sCrossings[sCrossingCount].extra = -1;
		if (sCrossings[sCrossingCount].deckHalf + 150 > sCrossings[sCrossingCount].half)	/* the box holds the whole bridge */
			sCrossings[sCrossingCount].half = sCrossings[sCrossingCount].deckHalf + 150;
		sCrossingCount++;
	}
	for (i = 0; i < sPendingCrossingCount && sCrossingCount < kMaxCrossings; ++i)	/* highway bridges */
		sCrossings[sCrossingCount++] = sPendingCrossings[i];
	sPendingCrossingCount = 0;
	memset(sCamKnown, 0, sizeof(sCamKnown));
	gRouteActive = 1;
	return 1;
}

void Route_SetMarks(int lanes, int marks)
{
	sLanes = lanes < 1 ? 1 : lanes;
	sMarks = marks;
}

/* ---- Drawing ---- */

static void BlendRect(float x0, float y0, float x1, float y1, float xDrawStart,
	float yDrawStart, float zoom, tPixel color, int alpha)
{
	tPixel *fb = (tPixel *)gBaseAddr;
	int stride = gRowBytes / 4, x, y;
	int sx0 = (int)((x0 - xDrawStart) / zoom), sx1 = (int)((x1 - xDrawStart) / zoom);
	int sy0 = (int)((yDrawStart - y1) / zoom), sy1 = (int)((yDrawStart - y0) / zoom);
	if (sx1 <= sx0)
		sx1 = sx0 + 1;
	if (sy1 <= sy0)
		sy1 = sy0 + 1;
	if (sx0 < 0) sx0 = 0;
	if (sy0 < 0) sy0 = 0;
	if (sx1 > gXSize) sx1 = gXSize;
	if (sy1 > gYSize) sy1 = gYSize;
	for (y = sy0; y < sy1; ++y) {
		tPixel *row = fb + y * stride;
		for (x = sx0; x < sx1; ++x)
			row[x] = alpha >= 255 ? color : BlendAlpha(color, row[x], (UInt32)alpha);
	}
}

static int InView(float x, float y, float xDrawStart, float yDrawStart, float zoom, float margin)
{
	float sx = (x - xDrawStart) / zoom, sy = (yDrawStart - y) / zoom;
	return sx > -margin && sx < gXSize + margin && sy > -margin && sy < gYSize + margin;
}

/* Dashed lines down the middle and between the lanes, any direction. */
static void DrawDashes(int onDeck, float xDrawStart, float yDrawStart, float zoom)
{
	float s, size = 2.2f;
	int line;
	if (!sMarks || !sCount)
		return;
	for (s = 0; s < Route_Length(); s += 120) {
		float t;
		int i = IndexAt(s), h, onHighway = 0;
		if ((sDeck[i] != 0) != onDeck)
			continue;
		for (h = 0; h < sHighCount; ++h)	/* the highway has its own lane lines */
			if (s > sHighS0[h] - 100 && s < sHighS1[h] + 100)
				onHighway = 1;
		if (onHighway)
			continue;
		if (!InView(sPoints[i].x, sPoints[i].y, xDrawStart, yDrawStart, zoom, 200))
			continue;
		for (line = -(sLanes - 1); line <= sLanes - 1; ++line) {
			float lateral = sHalfWidth * line / sLanes;
			for (t = 0; t <= 50; t += 3) {
				t2DPoint p;
				Route_Point(s + t, lateral, &p, NULL);
				BlendRect(p.x - size, p.y - size, p.x + size, p.y + size,
					xDrawStart, yDrawStart, zoom, PixelRGB(232, 232, 222), 255);
			}
		}
	}
}

void Route_DrawMarks(float xDrawStart, float yDrawStart, float zoom)
{
	int i;
	if (!gRouteActive)
		return;
	DrawDashes(0, xDrawStart, yDrawStart, zoom);
	{	/* highways: lane lines on each carriageway */
		int e, k, line;
		for (e = 0; e < sExtraCount; ++e)
			for (k = 0; k < sExtra[e].count; k += 2) {
				const tRoutePoint *p = &sExtra[e].points[k];
				if (k % 60 >= 26 || (sExtra[e].deck && sExtra[e].deck[k]) ||
					!InView(p->x, p->y, xDrawStart, yDrawStart, zoom, 100))
					continue;
				for (line = 1; line < sExtra[e].lanes; ++line) {
					float lateral = -sExtra[e].half + 2 * sExtra[e].half * line / sExtra[e].lanes;
					float x = p->x + p->hy * lateral, y = p->y - p->hx * lateral;
					BlendRect(x - 2.2f, y - 2.2f, x + 2.2f, y + 2.2f, xDrawStart, yDrawStart, zoom,
						PixelRGB(232, 232, 222), 255);
				}
			}
	}
	for (i = 0; sMarks && i < sBranchCount; ++i)	/* the forks' middle lines */
		if (i % 60 < 25 && InView(sBranch[i].x, sBranch[i].y, xDrawStart, yDrawStart, zoom, 100))
			BlendRect(sBranch[i].x - 2.2f, sBranch[i].y - 2.2f, sBranch[i].x + 2.2f, sBranch[i].y + 2.2f,
				xDrawStart, yDrawStart, zoom, PixelRGB(232, 232, 222), 255);
}

void DrawRouteDeck16(float xDrawStart, float yDrawStart, float zoom);	/* roaddraw.c */


/* Road signs: a green board on a post beside the road, white text and an
   arrow the way to go. */
static void DrawSigns(float xDrawStart, float yDrawStart, float zoom)
{
	tPixel *fb = (tPixel *)gBaseAddr;
	int stride = gRowBytes / 4, i, scale = Platform_GetRenderScale();
	for (i = 0; i < sSignCount; ++i) {
		const tSign *sign = &sSigns[i];
		int sx = (int)((sign->x - xDrawStart) / zoom) / scale, sy = (int)((yDrawStart - sign->y) / zoom) / scale;
		int w = (int)strlen(sign->text) * 8 + 34, h = 26, x0 = sx - w / 2, y0 = sy - h;
		float ax = sinf(sign->dir), ay = cosf(sign->dir);
		int cx = x0 + w - 16, cy = y0 + h / 2, k;
		if (sx < -w || sx > 640 + w || sy < -40 || sy > 520)
			continue;
		TR_FillRect(fb, stride, sx - 1, y0 + h, 3, 10, 0x294A);	/* post */
		TR_FillRect(fb, stride, x0 - 2, y0 - 2, w + 4, h + 4, 0x7FFF);
		TR_FillRect(fb, stride, x0, y0, w, h, 0x0200);
		TR_DrawString(fb, stride, x0 + 6, y0 + 9, sign->text, 0x7FFF, 1);
		for (k = -6; k <= 6; ++k) {	/* the arrow: a shaft and a head */
			int px = cx + (int)(ax * k), py = cy - (int)(ay * k);
			TR_FillRect(fb, stride, px - 1, py - 1, 2, 2, 0x7FFF);
		}
		for (k = 0; k <= 4; ++k) {
			float hx = ax * 6, hy = -ay * 6;
			TR_FillRect(fb, stride, cx + (int)(hx - ax * k + ay * k) - 1, cy + (int)(hy + ay * k + ax * k) - 1, 2, 2, 0x7FFF);
			TR_FillRect(fb, stride, cx + (int)(hx - ax * k - ay * k) - 1, cy + (int)(hy + ay * k - ax * k) - 1, 2, 2, 0x7FFF);
		}
	}
}

/* A tunnel's box: from the portal (entrance) to the road's end, across. */
static void TunnelBox(const tTunnel *t, float depth, float extra, float *x0, float *y0, float *x1, float *y1)
{
	float ex = t->x - t->dx * depth, ey = t->y - t->dy * depth;	/* the portal */
	float px = -t->dy * (t->width * 0.5f + extra), py = t->dx * (t->width * 0.5f + extra);
	float ax[4] = {ex + px, ex - px, t->x + px, t->x - px}, ay[4] = {ey + py, ey - py, t->y + py, t->y - py};
	int k;
	*x0 = *x1 = ax[0];
	*y0 = *y1 = ay[0];
	for (k = 1; k < 4; ++k) {
		if (ax[k] < *x0) *x0 = ax[k];
		if (ax[k] > *x1) *x1 = ax[k];
		if (ay[k] < *y0) *y0 = ay[k];
		if (ay[k] > *y1) *y1 = ay[k];
	}
}

/* Highway tunnels: the hill over them, the dark inside (cars fade into
   it), and the concrete portal. */
static void DrawTunnels(float xDrawStart, float yDrawStart, float zoom)
{
	int i, slice;
	for (i = 0; i < sTunnelCount; ++i) {
		const tTunnel *t = &sTunnels[i];
		float x0, y0, x1, y1;
		if (!InView(t->x, t->y, xDrawStart, yDrawStart, zoom, 600))
			continue;
		/* the hill: from the portal on, a little wider than the road */
		{
			tTunnel hill = *t;
			hill.x += t->dx * 260;
			hill.y += t->dy * 260;
			TunnelBox(&hill, t->depth * 0.5f + 260, 70, &x0, &y0, &x1, &y1);
			BlendRect(x0, y0, x1, y1, xDrawStart, yDrawStart, zoom, PixelRGB(46, 74, 34), 255);
			TunnelBox(&hill, t->depth * 0.5f + 260, 40, &x0, &y0, &x1, &y1);
			BlendRect(x0, y0, x1, y1, xDrawStart, yDrawStart, zoom, PixelRGB(60, 94, 42), 255);
		}
		for (slice = 0; slice < 8; ++slice) {	/* darker the deeper */
			tTunnel part = *t;
			float from = t->depth * slice / 8, to = t->depth * (slice + 1) / 8;
			part.x = t->x - t->dx * (t->depth - to);
			part.y = t->y - t->dy * (t->depth - to);
			TunnelBox(&part, to - from, 0, &x0, &y0, &x1, &y1);
			BlendRect(x0, y0, x1, y1, xDrawStart, yDrawStart, zoom, PixelRGB(8, 8, 10), 70 + slice * 23);
		}
		{	/* the portal: a concrete frame across the entrance */
			tTunnel portal = *t;
			portal.x = t->x - t->dx * (t->depth - 8);
			portal.y = t->y - t->dy * (t->depth - 8);
			TunnelBox(&portal, 22, 30, &x0, &y0, &x1, &y1);
			BlendRect(x0, y0, x1, y1, xDrawStart, yDrawStart, zoom, PixelRGB(60, 60, 58), 255);
			TunnelBox(&portal, 14, 24, &x0, &y0, &x1, &y1);
			BlendRect(x0, y0, x1, y1, xDrawStart, yDrawStart, zoom, PixelRGB(190, 188, 180), 255);
		}
	}
}

/* Drivers cannot go into a highway tunnel: the portal stops them. */
int Route_TunnelWall(void *object)
{
	tObject *obj = (tObject *)object;
	int i;
	if (!gRouteActive || !obj || obj->jumpHeight)
		return 0;
	for (i = 0; i < sTunnelCount; ++i) {
		const tTunnel *t = &sTunnels[i];
		float dx = obj->pos.x - t->x, dy = obj->pos.y - t->y;
		float along = dx * t->dx + dy * t->dy, across = -dx * t->dy + dy * t->dx;	/* along < 0: before the end */
		float into = along + t->depth - 10, speed;
		if (into <= 0 || along > 400 || fabsf(across) > t->width * 0.5f + 60)
			continue;
		obj->pos.x -= t->dx * into;
		obj->pos.y -= t->dy * into;
		speed = obj->velo.x * t->dx + obj->velo.y * t->dy;
		if (speed > 0) {
			obj->velo.x -= t->dx * speed * 1.4f;
			obj->velo.y -= t->dy * speed * 1.4f;
		}
		return 1;
	}
	return 0;
}

/* Bridges' shadows: each piece of deck a little to the south, the
   further the higher it is (none at its ends, where it meets the road).
   Pieces are marked in a mask first, so overlapping ones do not darken
   twice. */
static UInt8 *sShadowMask;
static int sShadowMaskSize;

static void ShadowPiece(const tRoutePoint *a, const tRoutePoint *b, float half, float ha, float hb,
	float xDrawStart, float yDrawStart, float zoom)
{
	float qx[4], qy[4], low, high, y;
	int k, row0, row1, sy;
	float oa = 60 * ha, ob = 60 * hb;
	qx[0] = a->x - a->hy * half; qy[0] = a->y + a->hx * half - oa;
	qx[1] = a->x + a->hy * half; qy[1] = a->y - a->hx * half - oa;
	qx[2] = b->x + b->hy * half; qy[2] = b->y - b->hx * half - ob;
	qx[3] = b->x - b->hy * half; qy[3] = b->y + b->hx * half - ob;
	for (k = 0; k < 4; ++k) {	/* to the screen */
		qx[k] = (qx[k] - xDrawStart) / zoom;
		qy[k] = (yDrawStart - qy[k]) / zoom;
	}
	low = high = qy[0];
	for (k = 1; k < 4; ++k) {
		if (qy[k] < low) low = qy[k];
		if (qy[k] > high) high = qy[k];
	}
	row0 = (int)low;
	row1 = (int)high + 1;
	if (row0 < 0) row0 = 0;
	if (row1 > gYSize) row1 = gYSize;
	for (sy = row0; sy < row1; ++sy) {
		float left, right;
		int x0, x1, x;
		y = sy + 0.5f;
		if (!QuadAt(qx, qy, y, &left, &right))
			continue;
		x0 = (int)left;
		x1 = (int)right + 1;
		if (x0 < 0) x0 = 0;
		if (x1 > gXSize) x1 = gXSize;
		for (x = x0; x < x1; ++x)
			sShadowMask[sy * gXSize + x] = 1;
	}
}

static void ShadowRoad(const tRoutePoint *points, const UInt8 *deck, const float *height, int count, float half,
	float xDrawStart, float yDrawStart, float zoom)
{
	int k;
	if (!deck || !height)
		return;
	for (k = 0; k + 1 < count; k += 3) {
		int next = k + 3 < count ? k + 3 : count - 1;
		if (!deck[k] || !deck[next] || (height[k] <= 0 && height[next] <= 0))
			continue;
		if (!InView(points[k].x, points[k].y, xDrawStart, yDrawStart, zoom, half + 120))
			continue;
		ShadowPiece(&points[k], &points[next], half, height[k], height[next], xDrawStart, yDrawStart, zoom);
	}
}

static void DrawShadows(float xDrawStart, float yDrawStart, float zoom)
{
	tPixel *fb = (tPixel *)gBaseAddr;
	int stride = gRowBytes / 4, e, sy, x, size = gXSize * gYSize;
	if (size > sShadowMaskSize) {
		UInt8 *mask = (UInt8 *)realloc(sShadowMask, (size_t)size);
		if (!mask)
			return;
		sShadowMask = mask;
		sShadowMaskSize = size;
	}
	memset(sShadowMask, 0, (size_t)size);
	ShadowRoad(sPoints, sDeck, sHeight, sCount, sHalfWidth, xDrawStart, yDrawStart, zoom);
	for (e = 0; e < sExtraCount; ++e)
		ShadowRoad(sExtra[e].points, sExtra[e].deck, sExtra[e].height, sExtra[e].count, sExtra[e].half,
			xDrawStart, yDrawStart, zoom);
	for (sy = 0; sy < gYSize; ++sy) {
		tPixel *row = fb + sy * stride;
		const UInt8 *mask = sShadowMask + sy * gXSize;
		for (x = 0; x < gXSize; ++x)
			if (mask[x])
				row[x] = BlendAlpha(PixelRGB(0, 0, 0), row[x], 120);
	}
}

void Route_DrawBridges(float xDrawStart, float yDrawStart, float zoom)
{
	tPixel *fb = (tPixel *)gBaseAddr;
	int stride = gRowBytes / 4, sy, i;
	if (!gRouteActive)
		return;
	DrawTunnels(xDrawStart, yDrawStart, zoom);
	DrawSigns(xDrawStart, yDrawStart, zoom);
	if (!sCrossingCount)
		return;
	DrawShadows(xDrawStart, yDrawStart, zoom);
	DrawRouteDeck16(xDrawStart, yDrawStart, zoom);
	/* concrete: the deck a little lighter than the road, fading in from
	   the bridge's ends so the surface does not change at a line */
	for (sy = 0; sy < gYSize; ++sy) {
		float worldY = yDrawStart - sy * zoom;
		const SInt16 *spans;
		int count = Route_RowSpans((int)(worldY / 2), 1, &spans), k, c;
		const tCrossing *crossing = NULL;
		for (c = 0; c < sCrossingCount; ++c)
			if (fabsf(worldY - sCrossings[c].y) <= sCrossings[c].half + 200)
				crossing = &sCrossings[c];
		if (!crossing)
			continue;
		for (k = 0; k < count; ++k) {
			int x0 = (int)((spans[2 * k] - xDrawStart) / zoom), x1 = (int)((spans[2 * k + 1] - xDrawStart) / zoom), x;
			tPixel *row = fb + sy * stride;
			if (x0 < 0) x0 = 0;
			if (x1 > gXSize) x1 = gXSize;
			for (x = x0; x < x1; ++x) {
				float worldX = xDrawStart + x * zoom;
				float along = (worldX - crossing->x) * crossing->bridgeX + (worldY - crossing->y) * crossing->bridgeY;
				float fade = (crossing->deckHalf - fabsf(along)) / 80;
				if (fade <= 0)
					continue;
				row[x] = BlendAlpha(PixelRGB(235, 228, 210), row[x], (UInt32)(55 * (fade > 1 ? 1 : fade)));
			}
		}
	}
	DrawDashes(1, xDrawStart, yDrawStart, zoom);
	/* railings along both sides: a dark edge and a light top */
	for (i = 0; i < sCount; ++i) {
		int side;
		if (!sDeck[i] || !InView(sPoints[i].x, sPoints[i].y, xDrawStart, yDrawStart, zoom, 300))
			continue;
		for (side = -1; side <= 1; side += 2) {
			float outer = sHalfWidth + 3, inner = sHalfWidth - 3;
			float ox = sPoints[i].x + sPoints[i].hy * outer * side, oy = sPoints[i].y - sPoints[i].hx * outer * side;
			float ix = sPoints[i].x + sPoints[i].hy * inner * side, iy = sPoints[i].y - sPoints[i].hx * inner * side;
			BlendRect(ox - 5, oy - 5, ox + 5, oy + 5, xDrawStart, yDrawStart, zoom, PixelRGB(70, 70, 68), 255);
			BlendRect(ix - 3, iy - 3, ix + 3, iy + 3, xDrawStart, yDrawStart, zoom, PixelRGB(220, 220, 214), 255);
		}
	}
	{	/* highway bridges: their lane lines and railings */
		int e, k, line, side;
		for (e = 0; e < sExtraCount; ++e)
			for (k = 0; sExtra[e].deck && k < sExtra[e].count; ++k) {
				const tRoutePoint *p = &sExtra[e].points[k];
				if (!sExtra[e].deck[k] || !InView(p->x, p->y, xDrawStart, yDrawStart, zoom, 200))
					continue;
				if (!(k & 1) && k % 60 < 26)
					for (line = 1; line < sExtra[e].lanes; ++line) {
						float lateral = -sExtra[e].half + 2 * sExtra[e].half * line / sExtra[e].lanes;
						float x = p->x + p->hy * lateral, y = p->y - p->hx * lateral;
						BlendRect(x - 2.2f, y - 2.2f, x + 2.2f, y + 2.2f, xDrawStart, yDrawStart, zoom,
							PixelRGB(232, 232, 222), 255);
					}
				for (side = -1; side <= 1; side += 2) {
					float outer = sExtra[e].half + 3, inner = sExtra[e].half - 3;
					float ox = p->x + p->hy * outer * side, oy = p->y - p->hx * outer * side;
					float ix = p->x + p->hy * inner * side, iy = p->y - p->hx * inner * side;
					BlendRect(ox - 5, oy - 5, ox + 5, oy + 5, xDrawStart, yDrawStart, zoom, PixelRGB(70, 70, 68), 255);
					BlendRect(ix - 3, iy - 3, ix + 3, iy + 3, xDrawStart, yDrawStart, zoom, PixelRGB(220, 220, 214), 255);
				}
			}
	}
}

/* ---- Camera ---- */

static float sCamHeadingX[16], sCamHeadingY[16], sCamHint[16];

void Route_CameraPos(int view, t2DPoint carPos, float *screenX, float *screenY)
{
	float *headingX = sCamHeadingX, *headingY = sCamHeadingY, *hint = sCamHint;
	int *known = sCamKnown;
	t2DPoint heading;
	if (view < 0 || view >= 16)
		view = 0;
	hint[view] = Route_Nearest(carPos, known[view] ? hint[view] : -1);
	heading = Route_Heading(hint[view] + 150);	/* where the road goes next */
	if (!known[view]) {
		headingX[view] = heading.x;
		headingY[view] = heading.y;
		known[view] = 1;
	}
	headingX[view] += (heading.x - headingX[view]) * 0.04f;
	headingY[view] += (heading.y - headingY[view]) * 0.04f;
	/* north: the car a little below the middle, as in the original */
	*screenX = 0.5f - headingX[view] * 0.18f;
	*screenY = 0.5f + headingY[view] * 0.05f + (headingY[view] < 0 ? headingY[view] * 0.13f : 0);
}

/* ---- Following the road ---- */

void Route_TrackObject(void *object)
{
	tObject *obj = (tObject *)object;
	if (!gRouteActive || !obj)
		return;
	{
		float localDistance, globalDistance, local, global;
		local = NearestS(obj->pos, obj->routeS > 0 ? obj->routeS : -1, &localDistance);
		/* cut across (offroad tyres, a jump): clearly on another stretch
		   of the road now, not just beside the own one */
		if (obj->routeS > 0 && localDistance > sHalfWidth * sHalfWidth * 2.25f) {
			global = NearestS(obj->pos, -1, &globalDistance);
			if (globalDistance * 2 < localDistance)
				local = global;
		}
		obj->routeS = local > 0 ? local : 0.01f;
	}
}

/* Traffic tracks of a free-form road have a point every kRouteTrackGap of
   road: up track point i at i * gap, the down track the other way round. */
#define kRouteTrackGap 150.0f

static float TrackPointS(const tTrackInfo *track, int i)
{
	return track == gTrackUp ? i * kRouteTrackGap : ((int)track->num - 1 - i) * kRouteTrackGap;
}

/* The next point of a traffic track to drive to from pos. */
int Route_TrackTarget(const void *trackInfo, t2DPoint pos)
{
	const tTrackInfo *track = (const tTrackInfo *)trackInfo;
	float s;
	int i;
	if (!track || track->num < 2)
		return 0;
	s = Route_Nearest(pos, -1);
	if (track == gTrackUp)
		i = (int)(s / kRouteTrackGap) + 1;
	else
		i = (int)track->num - 1 - (int)(s / kRouteTrackGap);
	if (i < 1)
		i = 1;
	if (i > (int)track->num - 1)
		i = (int)track->num - 1;
	return i;
}

/* Has an object reached its track point? By how far along the road it is,
   whichever way it points. */
int Route_TargetPassed(const void *trackInfo, void *object)
{
	const tTrackInfo *track = (const tTrackInfo *)trackInfo;
	tObject *obj = (tObject *)object;
	float target;
	if (!track || !obj || obj->target < 0 || obj->target >= (int)track->num)
		return 0;
	Route_TrackObject(obj);
	target = TrackPointS(track, obj->target);
	return track == gTrackUp ? obj->routeS > target - 40 : obj->routeS < target + 40;
}

/* Where the i-th bridge starts along the road (-1: none). */
float Route_BridgeS(int index)
{
	int i, run = -1;
	for (i = 1; i < sCount; ++i)
		if (sDeck[i] && !sDeck[i - 1] && ++run == index)
			return i * kRouteStep;
	return -1;
}

/* Which level an object is on where the road crosses itself: decided once
   when it comes into the crossing (by which road it points along) and kept
   until it leaves, so a car skidding or turning below the bridge stays
   below. onBridge: 0 outside every crossing, 1 on the bridge, 2 below,
   3 just went over the bridge's side (below; the car's physics then drops
   it onto the road, see Route_FellOffBridge). */
/* Coming into a crossing: on the bridge or below. Where the road crosses
   itself, by which stretch of it the object is on (a car coming onto the
   bridge in a curve or at an angle is still on the way up); at a highway
   bridge its own traffic is on top and the road's below; otherwise by the
   way it points. */
static int DecideLevel(tObject *obj, const tCrossing *c)
{
	float hx = sinf(obj->dir), hy = cosf(obj->dir);
	if (c->toS > 0) {
		Route_TrackObject(obj);
		if (obj->routeS > 0.02f) {
			if (obj->routeS > c->fromS - 400 && obj->routeS < c->toS + 400)
				return 1;
			if (fabsf(obj->routeS - c->belowS) < c->deckHalf + 400)
				return 2;
		}
	} else if (obj->ownTrack)
		return 1;
	else {
		t2DPoint at;
		float dir;
		Route_TrackObject(obj);
		Route_Point(obj->routeS, 0, &at, &dir);
		if (VEC2D_Value(VEC2D_Difference(at, obj->pos)) < sHalfWidth + 80)
			return 2;	/* on the road, under the highway */
	}
	return fabsf(hx * c->bridgeX + hy * c->bridgeY) > fabsf(hx * c->belowX + hy * c->belowY) ? 1 : 2;
}

int Route_UpdateLevel(void *object)
{
	tObject *obj = (tObject *)object;
	int i;
	if (!gRouteActive || !obj)
		return 0;
	for (i = 0; i < sCrossingCount; ++i) {
		const tCrossing *c = &sCrossings[i];
		float dx = obj->pos.x - c->x, dy = obj->pos.y - c->y, along, across;
		if (fabsf(dx) > c->half || fabsf(dy) > c->half)
			continue;
		if (c->ramp) {
			/* a ramp bridge (curved, with railings, nobody falls off):
			   the ramp's traffic is on it, the highway's below; anyone
			   else by the way it comes in */
			if (obj->ownTrack)
				obj->onBridge = Route_IsRamp(obj->ownTrack) ? 1 : 2;
			else if (c->extra >= 0 && c->extra < sExtraCount) {
				/* anyone else (a driver, either way up the ramp): on the
				   ramp where it is on its road, below elsewhere; onto it
				   only where it is not yet a bridge (not from below) */
				const tExtraRoad *r = &sExtra[c->extra];
				float best = 1e18f;
				int k, at = -1;
				for (k = 0; k < r->count; k += 4) {
					float ex = r->points[k].x - obj->pos.x, ey = r->points[k].y - obj->pos.y, d = ex * ex + ey * ey;
					if (d < best) {
						best = d;
						at = k;
					}
				}
				if (at < 0 || best > (r->half + 12) * (r->half + 12))
					obj->onBridge = 2;
				else if (!r->deck || !r->deck[at])
					obj->onBridge = 1;
				else if (!obj->onBridge)
					obj->onBridge = 2;
			} else if (!obj->onBridge) {
				float hx = sinf(obj->dir), hy = cosf(obj->dir);
				across = dx * c->bridgeY - dy * c->bridgeX;
				obj->onBridge = fabsf(across) < c->acrossHalf + 20 &&
					fabsf(hx * c->bridgeX + hy * c->bridgeY) > fabsf(hx * c->belowX + hy * c->belowY) ? 1 : 2;
			}
			return 0;
		}
		if (!obj->onBridge) {
			obj->onBridge = DecideLevel(obj, c);
			return 0;
		}
		if (obj->onBridge != 1 || obj->jumpHeight)
			return 0;
		along = dx * c->bridgeX + dy * c->bridgeY;
		across = dx * c->bridgeY - dy * c->bridgeX;
		if (fabsf(along) <= c->deckHalf - 10 && fabsf(across) > c->acrossHalf + 6) {
			obj->onBridge = 3;
			return 1;
		}
		return 0;
	}
	obj->onBridge = 0;
	return 0;
}

int Route_FellOffBridge(void *object)
{
	tObject *obj = (tObject *)object;
	Route_UpdateLevel(object);
	if (obj && obj->onBridge == 3) {
		obj->onBridge = 2;
		return 1;
	}
	return 0;
}

/* A jump ends: on the bridge's deck it is on the bridge, elsewhere in the
   crossing on the road below. */
void Route_Landed(void *object)
{
	tObject *obj = (tObject *)object;
	int i;
	if (!gRouteActive || !obj)
		return;
	for (i = 0; i < sCrossingCount; ++i) {
		const tCrossing *c = &sCrossings[i];
		float dx = obj->pos.x - c->x, dy = obj->pos.y - c->y;
		float along = dx * c->bridgeX + dy * c->bridgeY;
		float across = dx * c->bridgeY - dy * c->bridgeX;
		if (fabsf(dx) > c->half || fabsf(dy) > c->half)
			continue;
		obj->onBridge = fabsf(along) <= c->deckHalf && fabsf(across) <= c->acrossHalf ? 1 : 2;
		return;
	}
}

/* Below a bridge the road is walled in: a car down there cannot leave its
   road under the deck (and so not get up to the bridge's road). Pushes it
   back and bounces it; 1 if it hit the wall. */
int Route_WallBelow(void *object)
{
	tObject *obj = (tObject *)object;
	int i;
	if (!gRouteActive || !obj || obj->onBridge != 2 || obj->jumpHeight)
		return 0;
	for (i = 0; i < sCrossingCount; ++i) {
		const tCrossing *c = &sCrossings[i];
		float dx = obj->pos.x - c->x, dy = obj->pos.y - c->y;
		float along = dx * c->bridgeX + dy * c->bridgeY;
		float across = dx * c->bridgeY - dy * c->bridgeX;
		const SInt16 *spans;
		int count, k, best = -1;
		float bestDistance = 1e9f, margin = 14;
		if (fabsf(dx) > c->half || fabsf(dy) > c->half)
			continue;
		if (fabsf(along) > c->deckHalf + margin || fabsf(across) > c->acrossHalf + margin)
			return 0;	/* not under the deck */
		count = Route_RowSpans((int)(obj->pos.y / 2), 0, &spans);
		for (k = 0; k < count; ++k) {
			float left = spans[2 * k] + margin, right = spans[2 * k + 1] - margin, d;
			if (obj->pos.x >= left && obj->pos.x <= right)
				return 0;	/* on its road */
			d = obj->pos.x < left ? left - obj->pos.x : obj->pos.x - right;
			if (d < bestDistance) {
				bestDistance = d;
				best = k;
			}
		}
		if (best < 0 || bestDistance > 120)
			return 0;
		if (obj->pos.x < spans[2 * best] + margin) {
			obj->pos.x = spans[2 * best] + margin;
			if (obj->velo.x < 0)
				obj->velo.x = -obj->velo.x * 0.3f;
		} else {
			obj->pos.x = spans[2 * best + 1] - margin;
			if (obj->velo.x > 0)
				obj->velo.x = -obj->velo.x * 0.3f;
		}
		obj->velo = VEC2D_Scale(obj->velo, 0.85f);
		return 1;
	}
	return 0;
}

/* Where the i-th fork leaves the road, along it (-1: none). */
float Route_ForkS(int index)
{
	int i, fork = -1;
	for (i = 0; i < sBranchCount; ++i)
		if ((i == 0 || sBranchId[i] != sBranchId[i - 1]) && ++fork == index)
			return sBranchS[i];
	return -1;
}

/* Traffic at the highway junctions: some cars stay on the highway, some
   turn off. Highway cars (ownTrack) decide when they reach the exit or
   entry point of their track; cars of the road when they pass the
   junction. */
#define kTurnOffChance 0.2f

float Network_NearestDriverDistance(float y);
static int HighwayNeedsCars(const void *track);

/* A highway car that turns off: a new one of its kind joins the highway
   at the start of its track, out of sight, so the highway stays busy. */
static void Replace(tObject *obj)
{
	const tTrackInfo *track = (const tTrackInfo *)obj->ownTrack;
	tObject *car;
	float dir;
	if (!track || track->num < 2 || Network_NearestDriverDistance((float)track->track[0].y) < kVisDist * 1.5f ||
		!HighwayNeedsCars(track))
		return;
	car = NewObject(gFirstObj, obj->typeRes);
	if (!car)
		return;
	dir = atan2f((float)(track->track[1].x - track->track[0].x), (float)(track->track[1].y - track->track[0].y));
	car->pos = P2D(track->track[0].x, track->track[0].y);
	car->dir = dir;
	car->velo = VEC2D_Scale(P2D(sinf(dir), cosf(dir)), track->track[0].velo * 0.8f);
	car->control = kObjectDriveUp;
	car->ownTrack = obj->ownTrack;
	car->lane = (SInt8)RanInt(0, 3);
	car->target = 1;
}

static int NearestTrackIndex(const tTrackInfo *track, t2DPoint pos);

/* Where on a highway track a position is: the next point, and how far
   it is to the right of the track. */
static int TrackPlace(const tTrackInfo *track, t2DPoint pos, float *across)
{
	int i = NearestTrackIndex(track, pos) + 1;
	float tx, ty, length;
	if (i >= (int)track->num)
		i = track->num - 1;
	tx = (float)(track->track[i].x - track->track[i - 1].x);
	ty = (float)(track->track[i].y - track->track[i - 1].y);
	length = sqrtf(tx * tx + ty * ty);
	if (length < 1)
		length = 1;
	*across = ((pos.x - track->track[i].x) * ty - (pos.y - track->track[i].y) * tx) / length;
	return i;
}

#define kHighLane 84.0f
/* A car goes onto a highway track: in the lane it is nearest to (lane < 0)
   or, put there (teleport), in the given lane. */
static void JoinHighway(tObject *obj, const tTrackInfo *track, int lane, int teleport)
{
	float across;
	int i = TrackPlace(track, obj->pos, &across);
	if (lane < 0) {
		lane = (int)floorf(across / kHighLane + 1.5f);
		if (lane < 0)
			lane = 0;
		if (lane > 2)
			lane = 2;
	}
	if (teleport) {
		float tx = (float)(track->track[i].x - track->track[i - 1].x), ty = (float)(track->track[i].y - track->track[i - 1].y);
		float length = sqrtf(tx * tx + ty * ty), hx, hy, lateral = (lane - 1) * kHighLane;
		if (length < 1)
			length = 1;
		hx = tx / length;
		hy = ty / length;
		obj->pos = P2D(track->track[i - 1].x + hy * lateral, track->track[i - 1].y - hx * lateral);
		obj->dir = atan2f(hx, hy);
		obj->rotVelo = 0;
		obj->velo = VEC2D_Scale(P2D(hx, hy), track->track[i].velo * 0.8f);
	}
	obj->ownTrack = (void *)track;
	obj->control = kObjectDriveUp;
	obj->target = i;
	obj->lane = (SInt8)lane;
	obj->stuckSince = 0;
	obj->laneLatSet = 0;
}

/* Nothing near a point (for putting a car there). */
static int PlaceFree(const tObject *self, t2DPoint at)
{
	tObject *other = (tObject *)gFirstObj->next;
	for (; other != gFirstObj; other = (tObject *)other->next)
		if (other != self && other->type->flags & (kObjectWheelFlag | kObjectBounce) &&
			fabsf(other->pos.x - at.x) < 80 && fabsf(other->pos.y - at.y) < 80)
			return 0;
	return 1;
}

void Route_HighwayCarAt(void *object, int index)
{
	tObject *obj = (tObject *)object;
	int h;
	for (h = 0; h < sHighCount; ++h) {
		/* the exit is on the right: only from the right lane */
		if (obj->ownTrack == sJunction[h].north && index == sJunction[h].exitIndex &&
			(obj->lane & 3) == 2 && RanProb(kTurnOffChance * 2)) {
			Replace(obj);
			obj->ownTrack = NULL;	/* off with the road */
			obj->control = kObjectDriveUp;
			obj->routeS = 0;
			obj->target = Route_TrackTarget(gTrackUp, obj->pos);
			return;
		}
		/* the other way: from the right lane off and over the highway */
		if (obj->ownTrack == sJunction[h].south && sJunction[h].entryRamp && index == sJunction[h].rampFrom &&
			(obj->lane & 3) == 2 && RanProb(kTurnOffChance * 2)) {
			Replace(obj);
			obj->ownTrack = sJunction[h].entryRamp;
			obj->target = 1;
			obj->lane = 1;
			obj->laneLatSet = 0;
			obj->stuckSince = 0;
			return;
		}
		/* (no ramp: only where nobody sees it, onto the road beyond) */
		if (obj->ownTrack == sJunction[h].south && !sJunction[h].entryRamp && index == sJunction[h].entryIndex &&
			RanProb(kTurnOffChance) && Network_NearestDriverDistance2D(obj->pos) > 900) {
			t2DPoint at;
			float dir, s = sHighS0[h] - 120;
			Route_Point(s, Route_TrafficOffset(s, 0), &at, &dir);
			if (!PlaceFree(obj, at) || Network_NearestDriverDistance2D(at) < 900)
				return;
			Replace(obj);
			obj->ownTrack = NULL;
			obj->pos = at;
			obj->dir = dir + PI;
			obj->rotVelo = 0;
			obj->velo = VEC2D_Scale(P2D(sinf(obj->dir), cosf(obj->dir)), VEC2D_Value(obj->velo));
			obj->control = kObjectDriveDown;
			obj->routeS = s;
			obj->target = Route_TrackTarget(gTrackDown, obj->pos);
			return;
		}
	}
}

static int NearestTrackIndex(const tTrackInfo *track, t2DPoint pos)
{
	int i, best = 0;
	float bestDistance = 1e30f;
	for (i = 0; i < (int)track->num; ++i) {
		float dx = track->track[i].x - pos.x, dy = track->track[i].y - pos.y, d = dx * dx + dy * dy;
		if (d < bestDistance) {
			bestDistance = d;
			best = i;
		}
	}
	return best;
}

/* The road's traffic on a highway stretch drives as highway traffic (in
   lanes): it goes onto the highway's track as soon as it is on it. Cars
   coming the other way cross the highway to the oncoming carriageway;
   they wait at the junction until it is clear. 1: wait. */
int Route_RoadCarAt(void *object, float before)
{
	tObject *obj = (tObject *)object;
	int h;
	(void)before;
	for (h = 0; h < sHighCount; ++h) {
		float s = obj->routeS;
		const tTrackInfo *north = (const tTrackInfo *)sJunction[h].north, *south = (const tTrackInfo *)sJunction[h].south;
		if (!north || !south)
			continue;
		if (obj->control == kObjectDriveUp) {
			if (s > sHighS0[h] + 150 && s < sHighS1[h] - 300) {
				JoinHighway(obj, north, -1, 0);
				return 0;
			}
		} else if (obj->control == kObjectDriveDown) {
			if (sJunction[h].exitRamp && before > sJunction[h].exitRampS && s <= sJunction[h].exitRampS) {
				obj->ownTrack = sJunction[h].exitRamp;	/* up the ramp over the highway */
				obj->control = kObjectDriveUp;
				obj->target = 1;
				obj->lane = 1;
				obj->laneLatSet = 0;
				obj->stuckSince = 0;
				return 0;
			}
			if (s >= sHighS0[h] && s <= sHighS1[h]) {
				float across;
				TrackPlace(south, obj->pos, &across);
				if (fabsf(across) < 150 || sJunction[h].exitRamp)
					JoinHighway(obj, south, -1, fabsf(across) >= 150);	/* on the carriageway (or put there) */
				else if (Network_NearestDriverDistance2D(obj->pos) > 800)
					JoinHighway(obj, south, 0, 1);
				return 0;
			}
			if (!sJunction[h].exitRamp && s > sHighS1[h] && s < sHighS1[h] + 260) {
				/* no ramp here: it never crosses the highway where it can be
				   seen; it waits and goes on out of sight */
				if (Network_NearestDriverDistance2D(obj->pos) > 800) {
					JoinHighway(obj, south, 2, 1);
					return 0;
				}
				return 1;
			}
		}
	}
	return 0;
}

/* A car at the end of its own track: off a ramp onto the road or the
   highway. 0: not a ramp. */
int Route_OwnTrackEnd(void *object)
{
	tObject *obj = (tObject *)object;
	int h;
	for (h = 0; h < sHighCount; ++h) {
		if (obj->ownTrack && obj->ownTrack == sJunction[h].entryRamp) {
			obj->ownTrack = NULL;	/* down onto the road */
			obj->control = kObjectDriveDown;
			obj->routeS = 0;
			obj->lane = 0;
			obj->target = Route_TrackTarget(gTrackDown, obj->pos);
			return 1;
		}
		if (obj->ownTrack && obj->ownTrack == sJunction[h].exitRamp) {
			JoinHighway(obj, (const tTrackInfo *)sJunction[h].south, 2, 0);	/* in from the right */
			return 1;
		}
	}
	return 0;
}

int Route_IsRamp(const void *track)
{
	int h;
	for (h = 0; h < sHighCount; ++h)
		if (track && (track == sJunction[h].entryRamp || track == sJunction[h].exitRamp))
			return 1;
	return 0;
}

/* Is this car on a highway (its own traffic, or the road's on a highway
   stretch)? There it does not turn round. */
int Route_OnHighway(const void *object)
{
	const tObject *obj = (const tObject *)object;
	int h;
	if (!gRouteActive || !obj)
		return 0;
	if (obj->ownTrack)
		return 1;
	for (h = 0; h < sHighCount; ++h)
		if (obj->routeS > sHighS0[h] - 300 && obj->routeS < sHighS1[h] + 300)
			return 1;
	return 0;
}

/* How many cars a highway track should keep (set when the level is built). */
static const void *sKeepTrack[8];
static int sKeepCount[8], sKeepTracks;

void Route_KeepHighwayCars(const void *track, int count)
{
	if (sKeepTracks < 8) {
		sKeepTrack[sKeepTracks] = track;
		sKeepCount[sKeepTracks++] = count;
	}
}

static int HighwayNeedsCars(const void *track)
{
	int i, wanted = 0, have = 0;
	tObject *obj;
	for (i = 0; i < sKeepTracks; ++i)
		if (sKeepTrack[i] == track)
			wanted = sKeepCount[i];
	for (obj = (tObject *)gFirstObj->next; obj != gFirstObj; obj = (tObject *)obj->next)
		if (obj->ownTrack == track && obj->control == kObjectDriveUp)
			have++;
	return have <= wanted;	/* (the one turning off still counts) */
}

/* Too many cars on a highway track (the road's traffic joined it): one
   that reaches the tunnel at its end stays in there. */
int Route_HighwayFull(const void *track)
{
	int i, wanted = 0, have = 0;
	tObject *obj;
	for (i = 0; i < sKeepTracks; ++i)
		if (sKeepTrack[i] == track)
			wanted = sKeepCount[i];
	if (!wanted)
		return 0;
	for (obj = (tObject *)gFirstObj->next; obj != gFirstObj; obj = (tObject *)obj->next)
		if (obj->ownTrack == track && obj->control == kObjectDriveUp)
			have++;
	return have > wanted + wanted / 10;
}

/* The i-th crossing's center (a bridge over the road); 0 if there is none. */
int Route_CrossingCenter(int index, t2DPoint *at)
{
	if (index < 0 || index >= sCrossingCount)
		return 0;
	*at = P2D(sCrossings[index].x, sCrossings[index].y);
	return 1;
}

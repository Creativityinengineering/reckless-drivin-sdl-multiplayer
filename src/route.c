#include "route.h"
#include "objects.h"
#include "screen.h"
#include "pixel.h"
#include "roads.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int gRouteActive;
int gRouteDrawPass = -1;

static tRoutePoint *sPoints;
static UInt8 *sDeck;
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
} tCrossing;
#define kMaxCrossings 8
static tCrossing sCrossings[kMaxCrossings];
static int sCrossingCount;
static int sCamKnown[16];

void Route_Dispose(void)
{
	int layer;
	free(sPoints);
	free(sDeck);
	sPoints = NULL;
	sDeck = NULL;
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

float Route_Nearest(t2DPoint pos, float hint)
{
	int i, from = 0, to = sCount - 1, step = 1, best = 0;
	float bestDistance = 1e30f;
	if (!sCount)
		return 0;
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
	return best * kRouteStep;
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

int Route_ObjectLevel(const void *object)
{
	const tObject *obj = (const tObject *)object;
	if (!gRouteActive || !obj)
		return 0;
	if (obj->shooter && obj->type->flags2 & kObjectMissile)	/* a weapon: its shooter's level */
		obj = (const tObject *)obj->shooter;
	/* cars that drive here know (they may have fallen off); copies from
	   the host and everything else: from where they are and point */
	if (obj->type->flags & kObjectWheelFlag && !obj->replica && obj->onBridge)
		return obj->onBridge == 1;
	if (obj->onBridge == 2)
		return 0;
	return Route_Level(obj->pos, obj->dir);
}

int Route_SameLevel(const void *a, const void *b)
{
	return Route_ObjectLevel(a) == Route_ObjectLevel(b);
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

static int BuildLayer(int layer, int rows)
{
	tPiece *pieces = NULL;
	size_t count = 0, capacity = 0, i;
	int k, out = 0;
	for (k = 0; k + 1 < sCount; ++k) {
		const tRoutePoint *a = &sPoints[k], *b = &sPoints[k + 1];
		float qx[4], qy[4], low, high, y;
		int row0, row1, row;
		if ((sDeck[k] != 0) != (layer == 1))
			continue;
		qx[0] = a->x - a->hy * sHalfWidth; qy[0] = a->y + a->hx * sHalfWidth;
		qx[1] = a->x + a->hy * sHalfWidth; qy[1] = a->y - a->hx * sHalfWidth;
		qx[2] = b->x + b->hy * sHalfWidth; qy[2] = b->y - b->hx * sHalfWidth;
		qx[3] = b->x - b->hy * sHalfWidth; qy[3] = b->y + b->hx * sHalfWidth;
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
			if (count == capacity) {
				tPiece *grown;
				capacity = capacity ? capacity * 2 : 65536;
				grown = (tPiece *)realloc(pieces, capacity * sizeof(tPiece));
				if (!grown) {
					free(pieces);
					return 0;
				}
				pieces = grown;
			}
			pieces[count].row = row;
			pieces[count].left = (SInt16)floorf(left - 1);
			pieces[count].right = (SInt16)ceilf(right + 1);
			count++;
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

int Route_Setup(tRoutePoint *points, int count, float halfWidth, UInt8 *deck, float finishS)
{
	float maxY = 0;
	int i;
	Route_Dispose();
	if (!points || !deck || count < 2) {
		free(points);
		free(deck);
		return 0;
	}
	sPoints = points;
	sDeck = deck;
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
		sCrossingCount++;
	}
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
		int i = IndexAt(s);
		if ((sDeck[i] != 0) != onDeck)
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
	if (gRouteActive)
		DrawDashes(0, xDrawStart, yDrawStart, zoom);
}

void DrawRouteDeck16(float xDrawStart, float yDrawStart, float zoom);	/* roaddraw.c */

void Route_DrawBridges(float xDrawStart, float yDrawStart, float zoom)
{
	tPixel *fb = (tPixel *)gBaseAddr;
	int stride = gRowBytes / 4, sy, i;
	if (!gRouteActive || !sCrossingCount)
		return;
	/* the shadow: the bridge's shape, a little to the south */
	for (sy = 0; sy < gYSize; ++sy) {
		float worldY = yDrawStart - sy * zoom + 44;
		const SInt16 *spans;
		int count = Route_RowSpans((int)(worldY / 2), 1, &spans), k;
		for (k = 0; k < count; ++k) {
			int x0 = (int)((spans[2 * k] - xDrawStart) / zoom), x1 = (int)((spans[2 * k + 1] - xDrawStart) / zoom), x;
			tPixel *row = fb + sy * stride;
			if (x0 < 0) x0 = 0;
			if (x1 > gXSize) x1 = gXSize;
			for (x = x0; x < x1; ++x)
				row[x] = BlendAlpha(PixelRGB(0, 0, 0), row[x], 95);
		}
	}
	DrawRouteDeck16(xDrawStart, yDrawStart, zoom);
	DrawDashes(1, xDrawStart, yDrawStart, zoom);
	/* railings along both sides of the deck */
	for (i = 0; i < sCount; i += 2) {
		int side;
		if (!sDeck[i] || !InView(sPoints[i].x, sPoints[i].y, xDrawStart, yDrawStart, zoom, 300))
			continue;
		for (side = -1; side <= 1; side += 2) {
			float x = sPoints[i].x + sPoints[i].hy * sHalfWidth * side;
			float y = sPoints[i].y - sPoints[i].hx * sHalfWidth * side;
			BlendRect(x - 4, y - 4, x + 4, y + 4, xDrawStart, yDrawStart, zoom,
				side < 0 ? PixelRGB(96, 96, 92) : PixelRGB(200, 200, 194), 255);
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
	obj->routeS = Route_Nearest(obj->pos, obj->routeS > 0 ? obj->routeS : -1);
	if (obj->routeS <= 0)
		obj->routeS = 0.01f;
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

/* Cars on a free-form road's crossings: onBridge 1 while on a bridge, 2
   after falling off it onto the road below (until out of the crossing).
   Returns 1 at the moment a car falls off a bridge's side. */
int Route_FellOffBridge(void *object)
{
	tObject *obj = (tObject *)object;
	int i;
	if (!gRouteActive || !obj)
		return 0;
	for (i = 0; i < sCrossingCount; ++i) {
		const tCrossing *c = &sCrossings[i];
		float dx = obj->pos.x - c->x, dy = obj->pos.y - c->y;
		float along = dx * c->bridgeX + dy * c->bridgeY;
		float across = dx * c->bridgeY - dy * c->bridgeX;
		if (fabsf(dx) > c->half + 200 || fabsf(dy) > c->half + 200)
			continue;
		if (obj->onBridge == 2)
			return 0;	/* fell: stays below while in the crossing */
		if (fabsf(across) <= sHalfWidth && fabsf(along) <= c->deckHalf) {
			if (!obj->onBridge && fabsf(along) > c->deckHalf - 40)
				obj->onBridge = Route_Level(obj->pos, obj->dir);	/* drove onto it */
			else if (!obj->onBridge && Route_Level(obj->pos, obj->dir) &&
				Route_BackCollision(obj->pos, 0) == 0 && fabsf(across) > sHalfWidth - 20)
				obj->onBridge = 0;	/* beside it below: stays below */
			return 0;
		}
		if (obj->onBridge == 1 && fabsf(along) <= c->deckHalf - 10 && fabsf(across) > sHalfWidth + 6 &&
			!obj->jumpHeight) {
			obj->onBridge = 2;
			return 1;
		}
		if (fabsf(along) > c->deckHalf && obj->onBridge == 1)
			obj->onBridge = 0;	/* off the bridge's end: on the road again */
		return 0;
	}
	obj->onBridge = 0;
	return 0;
}

#ifndef __ROUTE
#define __ROUTE

#include "compat.h"
#include "vec2d.h"

/* Free-form roads (generated multiplayer levels with crossings): the road
   is a center line that can run in any direction and cross itself on a
   bridge. The original engine knows only roads that run upwards, one per
   height; everything here is used only while such a level is loaded, so
   the original levels and generated levels without crossings behave
   exactly as before. */

typedef struct {
	float x, y;	/* center line */
	float hx, hy;	/* heading (unit vector), forward = (sin dir, cos dir) */
} tRoutePoint;

extern int gRouteActive;	/* a free-form road is loaded */
extern int gRouteDrawPass;	/* sprites: -1 all, 0 below the bridges, 1 on them */

/* Takes over the center line (one point every kRouteStep of road), the
   half width, and which points are on a bridge; builds what the engine
   asks for. finishS: road length at the finish line. */
#define kRouteStep 2.0f
int Route_Setup(tRoutePoint *points, int count, float halfWidth, UInt8 *deck, float finishS);
void Route_Dispose(void);

float Route_Length(void);
float Route_FinishS(void);
float Route_HalfWidth(void);
/* A point on the road: s along it, lateral to the right of the heading. */
void Route_Point(float s, float lateral, t2DPoint *pos, float *dir);
t2DPoint Route_Heading(float s);
/* Where along the road a position is: the nearest point; with hint >= 0
   only near the hint (keeps the right piece where the road crosses). */
float Route_Nearest(t2DPoint pos, float hint);

/* 0 on the road, 1 just off it, 2 off it (as CalcBackCollision). */
int Route_BackCollision(t2DPoint pos, int tolerance);
/* The road piece at x in the row of height y: its edges; 0 if none. */
int Route_SpanAt(float x, float y, float *left, float *right);
/* Road pieces of a row of the world (row = y / 2): layer 0 the ground,
   layer 1 the bridges. Pairs of x (left, right), sorted. */
int Route_RowSpans(int row, int layer, const SInt16 **spans);
int Route_Rows(void);

/* On a bridge (1) or below/elsewhere (0): from where an object is and
   which way it points. */
int Route_Level(t2DPoint pos, float dir);
int Route_ObjectLevel(const void *object);
int Route_SameLevel(const void *a, const void *b);

/* Drawing: lane marks on the ground, then (after the cars below) the
   bridges with their marks, railings and shadows. */
void Route_DrawMarks(float xDrawStart, float yDrawStart, float zoom);
void Route_DrawBridges(float xDrawStart, float yDrawStart, float zoom);
/* Camera: the screen position of the car, looking ahead along the road. */
void Route_CameraPos(int view, t2DPoint carPos, float *screenX, float *screenY);

/* Lane marks: lanes per direction, and whether the world has marks. */
void Route_SetMarks(int lanes, int marks);

/* Keeping objects and traffic on the road. */
void Route_TrackObject(void *object);	/* updates its routeS */
int Route_TrackTarget(const void *track, t2DPoint pos);
int Route_TargetPassed(const void *track, void *object);
float Route_BridgeS(int index);
int Route_FellOffBridge(void *object);
void Route_Landed(void *object);	/* a jump ended: bridge or below */
int Route_WallBelow(void *object);	/* below a bridge: walls beside the road */	/* thrown off a bridge's side: falls */	/* where the i-th bridge starts, -1: none */

#endif

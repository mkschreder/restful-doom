//
// Building a DOOM level in memory, including its BSP.
//
// Scenarios need a fresh map every episode, and a map is not just geometry:
// the engine reads ten lumps, four of which (SEGS, SSECTORS, NODES, BLOCKMAP)
// are compiled FROM the geometry by a level editor's node builder. Shipping
// pre-built maps would mean committing WADs and would give every episode the
// same world, which is the thing the real levels already fail at. So the node
// builder lives here.
//
// The output is an ordinary PWAD image: whatever comes out of this is the
// same bytes the engine would have read off disk, so nothing downstream -
// rendering, collision, the route field, the observation - can tell the
// difference between a generated level and an authored one.
//

#ifndef __API_MAPGEN__
#define __API_MAPGEN__

#include <stddef.h>

#include "doomtype.h"

typedef struct mapgen_s mapgen_t;

// A map under construction. Free with MapGen_Free.
mapgen_t *MapGen_New(void);
void MapGen_Free(mapgen_t *m);

// Returns the new sector's index. `special` is a DOOM sector special (4, 5, 7
// and 16 hurt; 11 hurts and then ends the level).
int MapGen_AddSector(mapgen_t *m, int floorheight, int ceilingheight,
                     const char *floorpic, const char *ceilingpic,
                     int light, int special, int tag);

// A wall: one-sided, with `sector` on its front. The front of a line is its
// RIGHT as you walk v1 to v2, so a room's walls run clockwise.
int MapGen_AddWall(mapgen_t *m, int x1, int y1, int x2, int y2,
                   int sector, const char *texture);

// A two-sided line, front sector on the right. `midtexture` may be NULL for
// an open portal. Textures named for the upper and lower steps may be NULL
// when the two floors and ceilings meet.
int MapGen_AddPortal(mapgen_t *m, int x1, int y1, int x2, int y2,
                     int frontsector, int backsector,
                     const char *uppertexture, const char *lowertexture,
                     const char *midtexture);

// Applies to the line most recently added.
void MapGen_SetLineFlags(mapgen_t *m, int flags);
void MapGen_SetLineSpecial(mapgen_t *m, int special, int tag);

// The four walls of an axis-aligned rectangle, wound so `sector` is inside.
void MapGen_AddRoom(mapgen_t *m, int x1, int y1, int x2, int y2,
                    int sector, const char *texture);

// `type` is a DOOM "thing" number (1 is the player start, 2012 a medikit).
void MapGen_AddThing(mapgen_t *m, int x, int y, int angle, int type,
                     int options);

int MapGen_NumSectors(const mapgen_t *m);
int MapGen_NumLines(const mapgen_t *m);
int MapGen_NumThings(const mapgen_t *m);

// Compile to a PWAD image holding one map called `lumpname`. The caller owns
// the returned buffer and frees it with free(). NULL on failure, with *why
// pointing at a static description of what was wrong.
byte *MapGen_Build(mapgen_t *m, const char *lumpname, size_t *len,
                   const char **why);

// How many seg splits the last MapGen_Build had to round to whole map units.
// Axis-aligned geometry splits exactly and this is zero; a diagonal wall cut
// by a partition can land between two units, and rounding it moves the wall
// by less than a unit. Non-zero is a smell, not an error.
int MapGen_InexactSplits(const mapgen_t *m);

#endif

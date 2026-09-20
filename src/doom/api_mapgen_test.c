//
// What a generated map has to be true of before the engine will believe it.
//
// A node builder fails quietly. A wrong partition does not crash: the level
// loads, the walls draw in almost the right order, and things end up in the
// wrong sector - so a monster stands in lava it is not taking damage from and
// the agent's observation describes a room the player is not in. These checks
// are here because "it looked right" is not a test.
//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_mapgen.h"
#include "doomdata.h"

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond)
    {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

// ---------------------------------------------------------------- the map
//
// Parsed back out of the PWAD image, exactly as p_setup.c would read it.

typedef struct
{
    byte *image;
    mapvertex_t *vertexes;      int numvertexes;
    maplinedef_t *lines;        int numlines;
    mapsidedef_t *sides;        int numsides;
    mapsector_t *sectors;       int numsectors;
    mapseg_t *segs;             int numsegs;
    mapsubsector_t *subsectors; int numsubsectors;
    mapnode_t *nodes;           int numnodes;
    short *blockmap;            int blockmaplen;
} parsed_t;

static const char *const kLumpOrder[] = {
    "", "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES",
    "SEGS", "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP"
};

static int parse(byte *image, size_t len, const char *lumpname, parsed_t *out)
{
    int numlumps, infotableofs, i;
    byte *dir;

    memset(out, 0, sizeof(*out));
    out->image = image;
    if (len < 12 || memcmp(image, "PWAD", 4) != 0)
    {
        printf("  FAIL not a PWAD\n");
        failures++;
        return 0;
    }
    memcpy(&numlumps, image + 4, 4);
    memcpy(&infotableofs, image + 8, 4);
    if (numlumps != 11)
    {
        printf("  FAIL %d lumps, wanted 11\n", numlumps);
        failures++;
        return 0;
    }
    dir = image + infotableofs;
    for (i = 0; i < numlumps; i++)
    {
        int filepos, size;
        char name[9];
        byte *p = dir + i * 16;
        memcpy(&filepos, p, 4);
        memcpy(&size, p + 4, 4);
        memset(name, 0, sizeof(name));
        memcpy(name, p + 8, 8);

        if (i == 0)
        {
            ok(strcmp(name, lumpname) == 0, "the first lump names the map");
            continue;
        }
        if (strcmp(name, kLumpOrder[i]) != 0)
        {
            printf("  FAIL lump %d is %s, wanted %s\n", i, name, kLumpOrder[i]);
            failures++;
            return 0;
        }
        switch (i)
        {
          case ML_LINEDEFS:
            out->lines = (maplinedef_t *)(image + filepos);
            out->numlines = size / (int)sizeof(maplinedef_t);
            break;
          case ML_SIDEDEFS:
            out->sides = (mapsidedef_t *)(image + filepos);
            out->numsides = size / (int)sizeof(mapsidedef_t);
            break;
          case ML_VERTEXES:
            out->vertexes = (mapvertex_t *)(image + filepos);
            out->numvertexes = size / (int)sizeof(mapvertex_t);
            break;
          case ML_SEGS:
            out->segs = (mapseg_t *)(image + filepos);
            out->numsegs = size / (int)sizeof(mapseg_t);
            break;
          case ML_SSECTORS:
            out->subsectors = (mapsubsector_t *)(image + filepos);
            out->numsubsectors = size / (int)sizeof(mapsubsector_t);
            break;
          case ML_NODES:
            out->nodes = (mapnode_t *)(image + filepos);
            out->numnodes = size / (int)sizeof(mapnode_t);
            break;
          case ML_SECTORS:
            out->sectors = (mapsector_t *)(image + filepos);
            out->numsectors = size / (int)sizeof(mapsector_t);
            break;
          case ML_BLOCKMAP:
            out->blockmap = (short *)(image + filepos);
            out->blockmaplen = size / 2;
            break;
          default:
            break;
        }
    }
    return 1;
}

// r_main.c's R_PointOnSide, in map units.
static int point_on_side(int x, int y, const mapnode_t *node)
{
    long long dx = x - node->x;
    long long dy = y - node->y;
    long long left = (long long)node->dy * dx;
    long long right = dy * (long long)node->dx;
    return right < left ? 0 : 1;
}

// r_main.c's R_PointInSubsector.
static int point_in_subsector(const parsed_t *m, int x, int y)
{
    int nodenum;

    if (m->numnodes == 0)
    {
        return 0;
    }
    nodenum = m->numnodes - 1;
    while (!(nodenum & NF_SUBSECTOR))
    {
        nodenum = m->nodes[nodenum].children[point_on_side(x, y, &m->nodes[nodenum])];
    }
    return nodenum & ~NF_SUBSECTOR;
}

// p_maputl.c's P_PointOnLineSide, for a seg.
static int point_on_seg_side(const parsed_t *m, int x, int y, const mapseg_t *s)
{
    int ax = m->vertexes[s->v1].x, ay = m->vertexes[s->v1].y;
    int bx = m->vertexes[s->v2].x, by = m->vertexes[s->v2].y;
    long long cross = (long long)(by - ay) * (x - ax) - (long long)(bx - ax) * (y - ay);
    return cross > 0 ? 0 : (cross < 0 ? 1 : 2);
}

static int seg_sector(const parsed_t *m, const mapseg_t *s)
{
    int sidenum = m->lines[s->linedef].sidenum[s->side];
    if (sidenum < 0 || sidenum >= m->numsides)
    {
        return -1;
    }
    return m->sides[sidenum].sector;
}

// ------------------------------------------------------------- the checks

static void a_room_is_one_subsector_and_needs_no_nodes(void)
{
    mapgen_t *m = MapGen_New();
    const char *why = NULL;
    size_t len = 0;
    byte *image;
    parsed_t p;
    int s;

    printf("a room is one subsector and needs no nodes\n");
    s = MapGen_AddSector(m, 0, 128, "FLOOR4_8", "CEIL3_5", 192, 0, 0);
    MapGen_AddRoom(m, 0, 0, 512, 512, s, "STARTAN3");
    MapGen_AddThing(m, 256, 256, 90, 1, 7);

    image = MapGen_Build(m, "E2M1", &len, &why);
    ok(image != NULL, why ? why : "built");
    if (image != NULL && parse(image, len, "E2M1", &p))
    {
        ok(p.numvertexes == 4, "four corners");
        ok(p.numlines == 4, "four walls");
        ok(p.numsides == 4, "four sidedefs");
        ok(p.numsectors == 1, "one sector");
        ok(p.numsegs == 4, "four segs");
        ok(p.numsubsectors == 1, "one subsector");
        ok(p.numnodes == 0, "no BSP nodes at all");
        ok(MapGen_InexactSplits(m) == 0, "nothing was split");
    }
    free(image);
    MapGen_Free(m);
}

// Two rooms sharing a doorway. The boundary between them is a line with a seg
// on each side, so the builder has no choice but to partition there, and a
// point in one room must never resolve to the other room's sector.
static void a_point_resolves_to_the_sector_it_stands_in(void)
{
    mapgen_t *m = MapGen_New();
    const char *why = NULL;
    size_t len = 0;
    byte *image;
    parsed_t p;
    int a, b, x, y, bad = 0, wrongsector = 0, mixed = 0;

    printf("a point resolves to the sector it stands in\n");
    a = MapGen_AddSector(m, 0, 128, "FLOOR4_8", "CEIL3_5", 192, 0, 0);
    b = MapGen_AddSector(m, 8, 128, "FLOOR0_1", "CEIL3_5", 128, 0, 0);

    // Room A, 0..256 in x, with its east wall opened between y 64 and 192.
    MapGen_AddWall(m, 0, 0, 0, 256, a, "STARTAN3");
    MapGen_AddWall(m, 0, 256, 256, 256, a, "STARTAN3");
    MapGen_AddWall(m, 256, 256, 256, 192, a, "STARTAN3");
    MapGen_AddPortal(m, 256, 192, 256, 64, a, b, "BROWN1", "BROWN1", NULL);
    MapGen_AddWall(m, 256, 64, 256, 0, a, "STARTAN3");
    MapGen_AddWall(m, 256, 0, 0, 0, a, "STARTAN3");

    // Room B, 256..512 in x.
    MapGen_AddWall(m, 256, 0, 256, 64, b, "BROWN1");
    MapGen_AddWall(m, 256, 192, 256, 256, b, "BROWN1");
    MapGen_AddWall(m, 256, 256, 512, 256, b, "BROWN1");
    MapGen_AddWall(m, 512, 256, 512, 0, b, "BROWN1");
    MapGen_AddWall(m, 512, 0, 256, 0, b, "BROWN1");

    image = MapGen_Build(m, "E2M2", &len, &why);
    ok(image != NULL, why ? why : "built");
    if (image == NULL || !parse(image, len, "E2M2", &p))
    {
        free(image);
        MapGen_Free(m);
        return;
    }
    ok(p.numnodes > 0, "two rooms need at least one partition");

    for (x = 8; x < 512; x += 8)
    {
        for (y = 8; y < 256; y += 8)
        {
            int ss = point_in_subsector(&p, x, y);
            int want = x < 256 ? a : b;
            int i, got = -1;

            if (ss < 0 || ss >= p.numsubsectors)
            {
                bad++;
                continue;
            }
            for (i = 0; i < p.subsectors[ss].numsegs; i++)
            {
                const mapseg_t *s = &p.segs[p.subsectors[ss].firstseg + i];
                int sec = seg_sector(&p, s);
                if (got == -1)
                {
                    got = sec;
                }
                else if (got != sec)
                {
                    mixed++;
                }
                // The point must lie in front of every wall of its own cell.
                if (point_on_seg_side(&p, x, y, s) == 1)
                {
                    bad++;
                }
            }
            if (got != want)
            {
                wrongsector++;
            }
        }
    }
    ok(bad == 0, "every point is inside the cell it resolved to");
    ok(mixed == 0, "a subsector belongs to exactly one sector");
    ok(wrongsector == 0, "every point resolved to the room it is in");
    ok(MapGen_InexactSplits(m) == 0, "axis-aligned walls split exactly");

    free(image);
    MapGen_Free(m);
}

// A comb: one corridor with three alcoves off it. Deeply non-convex, one
// sector, so nothing but the BSP can get a point to the right cell.
static void a_non_convex_room_is_cut_into_convex_cells(void)
{
    mapgen_t *m = MapGen_New();
    const char *why = NULL;
    size_t len = 0;
    byte *image;
    parsed_t p;
    int s, i, bad = 0, outside = 0;

    printf("a non-convex room is cut into convex cells\n");
    s = MapGen_AddSector(m, 0, 128, "FLOOR4_8", "CEIL3_5", 160, 0, 0);

    // Corridor along the bottom, y 0..64, x 0..640, with alcoves rising from
    // it at x 64..128, 256..320 and 448..512.
    MapGen_AddWall(m, 0, 0, 0, 64, s, "GRAY4");
    MapGen_AddWall(m, 0, 64, 64, 64, s, "GRAY4");
    MapGen_AddWall(m, 64, 64, 64, 256, s, "GRAY4");
    MapGen_AddWall(m, 64, 256, 128, 256, s, "GRAY4");
    MapGen_AddWall(m, 128, 256, 128, 64, s, "GRAY4");
    MapGen_AddWall(m, 128, 64, 256, 64, s, "GRAY4");
    MapGen_AddWall(m, 256, 64, 256, 256, s, "GRAY4");
    MapGen_AddWall(m, 256, 256, 320, 256, s, "GRAY4");
    MapGen_AddWall(m, 320, 256, 320, 64, s, "GRAY4");
    MapGen_AddWall(m, 320, 64, 448, 64, s, "GRAY4");
    MapGen_AddWall(m, 448, 64, 448, 256, s, "GRAY4");
    MapGen_AddWall(m, 448, 256, 512, 256, s, "GRAY4");
    MapGen_AddWall(m, 512, 256, 512, 64, s, "GRAY4");
    MapGen_AddWall(m, 512, 64, 640, 64, s, "GRAY4");
    MapGen_AddWall(m, 640, 64, 640, 0, s, "GRAY4");
    MapGen_AddWall(m, 640, 0, 0, 0, s, "GRAY4");

    image = MapGen_Build(m, "E2M3", &len, &why);
    ok(image != NULL, why ? why : "built");
    if (image == NULL || !parse(image, len, "E2M3", &p))
    {
        free(image);
        MapGen_Free(m);
        return;
    }
    ok(p.numnodes >= 3, "three alcoves need partitioning");
    ok(p.numsubsectors >= 4, "and leave at least four convex cells");

    for (i = 0; i < 16; i++)
    {
        const struct { int x, y; } probes[] = {
            {8, 8}, {632, 8}, {96, 200}, {288, 200}, {480, 200},
            {96, 32}, {288, 32}, {480, 32}, {200, 32}, {380, 32},
            {96, 128}, {288, 128}, {480, 128}, {320 - 8, 250},
            {64 + 8, 250}, {512 - 8, 8},
        };
        int x = probes[i].x, y = probes[i].y;
        int ss = point_in_subsector(&p, x, y);
        int j;

        if (ss < 0 || ss >= p.numsubsectors)
        {
            outside++;
            continue;
        }
        for (j = 0; j < p.subsectors[ss].numsegs; j++)
        {
            if (point_on_seg_side(&p, x, y, &p.segs[p.subsectors[ss].firstseg + j]) == 1)
            {
                bad++;
            }
        }
    }
    ok(outside == 0, "every probe resolved to a real subsector");
    ok(bad == 0, "every probe is inside the cell it resolved to");
    ok(MapGen_InexactSplits(m) == 0, "axis-aligned walls split exactly");

    free(image);
    MapGen_Free(m);
}

// Every seg must lie on its linedef, face the way its side does, and carry
// the distance along that linedef the texture is drawn from.
static void a_seg_stays_on_the_line_it_came_from(void)
{
    mapgen_t *m = MapGen_New();
    const char *why = NULL;
    size_t len = 0;
    byte *image;
    parsed_t p;
    int i, off = 0, stray = 0, backwards = 0;

    printf("a seg stays on the line it came from\n");
    {
        int s = MapGen_AddSector(m, 0, 128, "FLOOR4_8", "CEIL3_5", 160, 0, 0);
        MapGen_AddWall(m, 0, 0, 0, 64, s, "GRAY4");
        MapGen_AddWall(m, 0, 64, 64, 64, s, "GRAY4");
        MapGen_AddWall(m, 64, 64, 64, 256, s, "GRAY4");
        MapGen_AddWall(m, 64, 256, 128, 256, s, "GRAY4");
        MapGen_AddWall(m, 128, 256, 128, 0, s, "GRAY4");
        MapGen_AddWall(m, 128, 0, 0, 0, s, "GRAY4");
    }
    image = MapGen_Build(m, "E2M4", &len, &why);
    ok(image != NULL, why ? why : "built");
    if (image == NULL || !parse(image, len, "E2M4", &p))
    {
        free(image);
        MapGen_Free(m);
        return;
    }
    for (i = 0; i < p.numsegs; i++)
    {
        const mapseg_t *s = &p.segs[i];
        const maplinedef_t *ld = &p.lines[s->linedef];
        int lx1 = p.vertexes[ld->v1].x, ly1 = p.vertexes[ld->v1].y;
        int lx2 = p.vertexes[ld->v2].x, ly2 = p.vertexes[ld->v2].y;
        int sx1 = p.vertexes[s->v1].x, sy1 = p.vertexes[s->v1].y;
        int sx2 = p.vertexes[s->v2].x, sy2 = p.vertexes[s->v2].y;
        int rx = s->side == 0 ? lx1 : lx2, ry = s->side == 0 ? ly1 : ly2;
        double want;

        // Both endpoints on the linedef's infinite line.
        if ((long long)(ly2 - ly1) * (sx1 - lx1) - (long long)(lx2 - lx1) * (sy1 - ly1) != 0
            || (long long)(ly2 - ly1) * (sx2 - lx1) - (long long)(lx2 - lx1) * (sy2 - ly1) != 0)
        {
            stray++;
        }
        // Side 0 runs with the linedef, side 1 against it.
        if (s->side == 0)
        {
            if ((long long)(sx2 - sx1) * (lx2 - lx1) + (long long)(sy2 - sy1) * (ly2 - ly1) <= 0)
            {
                backwards++;
            }
        }
        else if ((long long)(sx2 - sx1) * (lx2 - lx1) + (long long)(sy2 - sy1) * (ly2 - ly1) >= 0)
        {
            backwards++;
        }
        want = sqrt((double)(sx1 - rx) * (sx1 - rx) + (double)(sy1 - ry) * (sy1 - ry));
        if (fabs(want - s->offset) > 1.0)
        {
            off++;
        }
    }
    ok(stray == 0, "every seg lies on its linedef");
    ok(backwards == 0, "every seg runs the way its side does");
    ok(off == 0, "every seg knows how far along the line it starts");

    free(image);
    MapGen_Free(m);
}

// p_maputl.c walks the blockmap to find what a moving thing might hit. A line
// missing from a block it crosses is a wall the player walks through.
static void the_blockmap_lists_every_line_that_crosses_a_block(void)
{
    mapgen_t *m = MapGen_New();
    const char *why = NULL;
    size_t len = 0;
    byte *image;
    parsed_t p;
    int s, missing = 0, bx, by;
    int originx, originy, cols, rows;

    printf("the blockmap lists every line that crosses a block\n");
    s = MapGen_AddSector(m, 0, 128, "FLOOR4_8", "CEIL3_5", 160, 0, 0);
    MapGen_AddWall(m, -64, -64, -64, 700, s, "GRAY4");
    MapGen_AddWall(m, -64, 700, 700, 700, s, "GRAY4");
    MapGen_AddWall(m, 700, 700, 700, -64, s, "GRAY4");
    MapGen_AddWall(m, 700, -64, -64, -64, s, "GRAY4");

    image = MapGen_Build(m, "E2M5", &len, &why);
    ok(image != NULL, why ? why : "built");
    if (image == NULL || !parse(image, len, "E2M5", &p))
    {
        free(image);
        MapGen_Free(m);
        return;
    }
    originx = p.blockmap[0];
    originy = p.blockmap[1];
    cols = (unsigned short)p.blockmap[2];
    rows = (unsigned short)p.blockmap[3];
    ok(cols > 0 && rows > 0, "the blockmap covers some ground");

    for (by = 0; by < rows; by++)
    {
        for (bx = 0; bx < cols; bx++)
        {
            int x0 = originx + bx * 128, y0 = originy + by * 128;
            int x1 = x0 + 128, y1 = y0 + 128;
            int offset = (unsigned short)p.blockmap[4 + by * cols + bx];
            int i;

            for (i = 0; i < p.numlines; i++)
            {
                int ax = p.vertexes[p.lines[i].v1].x;
                int ay = p.vertexes[p.lines[i].v1].y;
                int cx = p.vertexes[p.lines[i].v2].x;
                int cy = p.vertexes[p.lines[i].v2].y;
                int listed = 0, k;

                // Only the axis-aligned lines of this map, so a cheap
                // overlap test is exact.
                if (ax == cx)
                {
                    if (ax < x0 || ax > x1) continue;
                    if (ay > cy) { int t = ay; ay = cy; cy = t; }
                    if (cy < y0 || ay > y1) continue;
                }
                else
                {
                    if (ay < y0 || ay > y1) continue;
                    if (ax > cx) { int t = ax; ax = cx; cx = t; }
                    if (cx < x0 || ax > x1) continue;
                }
                for (k = offset + 1; k < p.blockmaplen; k++)
                {
                    if ((unsigned short)p.blockmap[k] == 0xffff) break;
                    if (p.blockmap[k] == i) { listed = 1; break; }
                }
                if (!listed)
                {
                    missing++;
                }
            }
        }
    }
    ok(missing == 0, "no line is missing from a block it crosses");

    free(image);
    MapGen_Free(m);
}

int main(void)
{
    a_room_is_one_subsector_and_needs_no_nodes();
    a_point_resolves_to_the_sector_it_stands_in();
    a_non_convex_room_is_cut_into_convex_cells();
    a_seg_stays_on_the_line_it_came_from();
    the_blockmap_lists_every_line_that_crosses_a_block();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

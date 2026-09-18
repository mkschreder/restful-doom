//
// Which way to go: a walkable distance field to the level's exit.
//
// The observation already told an agent where the exit was, as a straight-line
// bearing. That is worse than useless in a building - the straight line goes
// through walls, and an agent following it presses against the wall the exit
// is behind. Measured on E1M1: a navigator following it for 500 decisions
// closed 231 units of 2432 and visited 14 distinct places.
//
// This replaces it with the direction a player would actually walk.
//
// SECTORS WERE TRIED FIRST and are the wrong graph. A DOOM sector is not a
// room: a single room is many sectors - steps, platforms, light levels - and
// two of them can share a doorway that is a sliver. Breadth-first over sector
// adjacency therefore gives a hop count that does not order positions by how
// far away they are, and a follower walking to the shared linedef's midpoint
// oscillated between two sectors for 400 decisions without leaving the room.
//
// So: a uniform grid over the level, a cell being walkable if the player could
// stand in it, an edge between neighbours if a player could cross between
// them, and a breadth-first search from the exit. That gives every walkable
// point a real distance, and the way on is the downhill direction.
//
// A CLOSED DOOR IS PASSABLE here, deliberately. A door is a two-sided linedef
// with no opening right now and a player opens it; a field that routed around
// closed doors would route around most of DOOM.
//
// This is map knowledge, and it is worth being explicit about that: it is what
// a player who has walked the level before has. It is also strictly more
// honest than the straight line it replaces, which claimed to know where the
// exit was and pointed at a wall.
//

#include <stdlib.h>
#include <string.h>

#include "api_route.h"
#include "doomstat.h"
#include "p_local.h"
#include "r_state.h"

/* Grid resolution. The player is 32 units wide, so a 64-unit cell is about one
 * of them and a corridor is one or two cells across. */
#define ROUTE_CELL 32
#define ROUTE_MAX_CELLS 262144
#define ROUTE_UNREACHED 0xffff
/* How far down the gradient to aim. One cell is jittery - the target flips
 * between neighbours as the player crosses a boundary - and too far cuts
 * corners through walls. Four cells is about one decision of walking. */
#define ROUTE_LOOKAHEAD 4

static unsigned short *field;
/* Which cells the player can reach at all, and which edges between them a
 * player can cross. Computed once with the distance field and reused, so the
 * per-observation searches below are array work and no engine queries. */
static unsigned short *reachable;
static unsigned char *edges;
/* Where the player has been this episode. */
static unsigned char *visited;
static int grid_w, grid_h;
static fixed_t grid_x0, grid_y0;
static void *built_for;
static int built_count;

static boolean route_blocked;

/* Blocked only by things a player can never cross: a one-sided wall, or a line
 * the author flagged as blocking. A shut door is two-sided and is not blocked. */

static boolean PTR_RouteTraverse(intercept_t *in)
{
    line_t *ld = in->d.line;

    if (ld->backsector == NULL || ld->frontsector == NULL || (ld->flags & ML_BLOCKING))
    {
        route_blocked = true;
        return false;
    }
    return true;
}

/* P_CheckPosition leaves the engine's "thing being moved" scratch state
 * pointing at the probe, and repeated calls drift the probe's own position.
 * Every caller here restores it - without this the flood fill tested seven
 * edges on a level with 3268 walkable cells, because the probe had wandered
 * somewhere it could not stand. */
static boolean walkable(mobj_t *probe, fixed_t x, fixed_t y)
{
    fixed_t ox = probe->x, oy = probe->y, oz = probe->z;
    boolean ok = P_CheckPosition(probe, x, y);

    probe->x = ox;
    probe->y = oy;
    probe->z = oz;
    return ok;
}

static boolean clear_line(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    route_blocked = false;
    P_PathTraverse(x1, y1, x2, y2, PT_ADDLINES, PTR_RouteTraverse);
    return !route_blocked;
}

/* Can a player get from one cell to its neighbour?
 *
 * Sampled ACROSS the shared boundary rather than down the centre line. A
 * doorway is bounded by the door's own two-sided linedef and by one-sided side
 * tracks, and a single centre-to-centre segment on a 64-unit grid clips a
 * track as often as it threads the gap - which sealed every door on E1M1 and
 * left the exit unreachable from the spawn. Three parallel samples find the
 * opening if there is one.
 *
 * `walkable` is not enough on its own: P_CheckPosition passes at any point
 * with no linedef within the player's radius, which includes the solid void
 * between rooms. The line test is what distinguishes floor from void. */
static boolean can_cross(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    /* Perpendicular to the crossing, which is axis-aligned on this grid. */
    fixed_t px = (y2 - y1) == 0 ? 0 : 1;
    fixed_t py = (x2 - x1) == 0 ? 0 : 1;
    int i;
    static const int offs[3] = { 0, 20, -20 };

    for (i = 0; i < 3; i++)
    {
        fixed_t ox = px * offs[i] * FRACUNIT;
        fixed_t oy = py * offs[i] * FRACUNIT;

        if (clear_line(x1 + ox, y1 + oy, x2 + ox, y2 + oy))
        {
            return true;
        }
    }
    return false;
}

static fixed_t cell_x(int cx)
{
    return grid_x0 + (cx * ROUTE_CELL + ROUTE_CELL / 2) * FRACUNIT;
}

static fixed_t cell_y(int cy)
{
    return grid_y0 + (cy * ROUTE_CELL + ROUTE_CELL / 2) * FRACUNIT;
}

static int cell_of(fixed_t x, fixed_t y, int *cx, int *cy)
{
    *cx = (x - grid_x0) / FRACUNIT / ROUTE_CELL;
    *cy = (y - grid_y0) / FRACUNIT / ROUTE_CELL;
    return *cx >= 0 && *cy >= 0 && *cx < grid_w && *cy < grid_h;
}

/* Every walkable place a player might stand to use the exit, nearest offsets
 * first.
 *
 * NOT the exit line's midpoint, which is inside the wall the switch is mounted
 * on. And deliberately ALL of them rather than the first: a line has two
 * sides, and the far side of an exit switch can be a void pocket that passes a
 * position check while being sealed off from the level. Which of these is the
 * real one is a question about reachability, answered by the caller. */
int API_ExitSpots(mobj_t *probe, api_point_t *out, int max)
{
    static const int offsets[] = { 32, 56, 80, 112, 144, 192 };
    static const int alongs[] = { 2, 1, 3 };
    int i, n = 0;

    for (i = 0; i < numlines && n < max; i++)
    {
        line_t *ld = &lines[i];
        fixed_t dx, dy, len;
        unsigned int oi, ai;
        int side;

        if (ld->special != 11 && ld->special != 51 && ld->special != 52
            && ld->special != 124)
        {
            continue;
        }
        dx = ld->v2->x - ld->v1->x;
        dy = ld->v2->y - ld->v1->y;
        len = P_AproxDistance(dx, dy);
        if (len <= 0)
        {
            continue;
        }
        for (oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]) && n < max; oi++)
        {
            for (ai = 0; ai < sizeof(alongs) / sizeof(alongs[0]) && n < max; ai++)
            {
                fixed_t px = ld->v1->x + dx * alongs[ai] / 4;
                fixed_t py = ld->v1->y + dy * alongs[ai] / 4;

                for (side = 0; side < 2 && n < max; side++)
                {
                    fixed_t sign = side == 0 ? FRACUNIT : -FRACUNIT;
                    fixed_t nx = FixedDiv(FixedMul(dy, sign), len);
                    fixed_t ny = FixedDiv(FixedMul(-dx, sign), len);
                    fixed_t sx = px + FixedMul(offsets[oi] * FRACUNIT, nx);
                    fixed_t sy = py + FixedMul(offsets[oi] * FRACUNIT, ny);

                    if (walkable(probe, sx, sy))
                    {
                        out[n].x = sx;
                        out[n].y = sy;
                        n++;
                    }
                }
            }
        }
    }
    return n;
}

/* Flood fill `out` from `seed`, 4-connected, through walkable cells a player
 * could cross between. Returns how many cells were reached. */
static int flood(mobj_t *probe, unsigned short *out, int seed, int *queue)
{
    int head = 0, tail = 0, reached = 1, i;

    for (i = 0; i < grid_w * grid_h; i++)
    {
        out[i] = ROUTE_UNREACHED;
    }
    out[seed] = 0;
    queue[tail++] = seed;
    while (head < tail)
    {
        int at = queue[head++];
        int ax = at % grid_w;
        int ay = at / grid_w;
        static const int dx[4] = { 1, -1, 0, 0 };
        static const int dy[4] = { 0, 0, 1, -1 };
        int d;

        for (d = 0; d < 4; d++)
        {
            int nx = ax + dx[d];
            int ny = ay + dy[d];
            int ni;

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (out[ni] != ROUTE_UNREACHED)
            {
                continue;
            }
            if (!walkable(probe, cell_x(nx), cell_y(ny))
                || !can_cross(cell_x(ax), cell_y(ay), cell_x(nx), cell_y(ny)))
            {
                continue;
            }
            out[ni] = out[at] + 1;
            queue[tail++] = ni;
            reached++;
        }
    }
    return reached;
}

/* Rebuild when the level changed. Keyed on the `lines` array, which a level
 * reload reallocates - a stale field would route through a map that is gone. */
static void ensure_built(mobj_t *probe)
{
    fixed_t minx = 0, miny = 0, maxx = 0, maxy = 0;
    api_point_t spots[API_MAX_EXIT_SPOTS];
    int n_spots;
    int i, cx, cy, seed;
    int *queue;
    unsigned short *reach;

    if (field != NULL && built_for == (void *)lines && built_count == numlines)
    {
        return;
    }
    free(field);
    field = NULL;
    built_for = (void *)lines;
    built_count = numlines;

    n_spots = API_ExitSpots(probe, spots, API_MAX_EXIT_SPOTS);
    if (numvertexes <= 0 || n_spots == 0)
    {
        printf("API_Route: no exit line, or nowhere to stand at it\n");
        return;
    }
    minx = maxx = vertexes[0].x;
    miny = maxy = vertexes[0].y;
    for (i = 1; i < numvertexes; i++)
    {
        if (vertexes[i].x < minx) minx = vertexes[i].x;
        if (vertexes[i].x > maxx) maxx = vertexes[i].x;
        if (vertexes[i].y < miny) miny = vertexes[i].y;
        if (vertexes[i].y > maxy) maxy = vertexes[i].y;
    }
    grid_x0 = minx - ROUTE_CELL * FRACUNIT;
    grid_y0 = miny - ROUTE_CELL * FRACUNIT;
    grid_w = (maxx - minx) / FRACUNIT / ROUTE_CELL + 3;
    grid_h = (maxy - miny) / FRACUNIT / ROUTE_CELL + 3;
    if (grid_w <= 0 || grid_h <= 0 || grid_w * grid_h > ROUTE_MAX_CELLS)
    {
        printf("API_Route: %dx%d grid is too large for this level\n", grid_w, grid_h);
        return;
    }

    field = malloc(sizeof(unsigned short) * grid_w * grid_h);
    queue = malloc(sizeof(int) * grid_w * grid_h);
    if (field == NULL || queue == NULL)
    {
        printf("API_Route: out of memory for a %dx%d grid\n", grid_w, grid_h);
        free(queue);
        free(field);
        field = NULL;
        return;
    }
    for (i = 0; i < grid_w * grid_h; i++)
    {
        field[i] = ROUTE_UNREACHED;
    }

    /* WHICH walkable cells the player can actually get to.
     *
     * Needed because "walkable" and "reachable" are different questions, and
     * the exit line has candidate standing spots on BOTH of its sides. One of
     * them is the room the player walks into; the other can be a void pocket
     * behind the switch that passes a position check and is sealed off from
     * the level. Seeding the distance field there produced a field covering
     * six cells out of 3268 walkable ones, and an agent that was told the exit
     * was unreachable from everywhere. */
    if (!cell_of(probe->x, probe->y, &cx, &cy) || !walkable(probe, probe->x, probe->y))
    {
        printf("API_Route: the player is not on the grid\n");
        free(queue);
        free(field);
        field = NULL;
        return;
    }
    reach = malloc(sizeof(unsigned short) * grid_w * grid_h);
    if (reach == NULL)
    {
        free(queue);
        free(field);
        field = NULL;
        return;
    }
    flood(probe, reach, cy * grid_w + cx, queue);

    /* The first candidate spot at the exit that the player can reach. */
    seed = -1;
    for (i = 0; i < n_spots; i++)
    {
        int sx, sy;

        if (!cell_of(spots[i].x, spots[i].y, &sx, &sy))
        {
            continue;
        }
        if (reach[sy * grid_w + sx] != ROUTE_UNREACHED)
        {
            seed = sy * grid_w + sx;
            break;
        }
    }
    /* Kept: the frontier search walks this component every observation. */
    free(reachable);
    reachable = reach;
    reach = NULL;
    if (seed < 0)
    {
        printf("API_Route: none of the %d standing spots at the exit is reachable\n", n_spots);
        free(queue);
        free(field);
        field = NULL;
        return;
    }
    free(edges);
    edges = calloc(1, grid_w * grid_h);
    free(visited);
    visited = calloc(1, grid_w * grid_h);
    if (edges != NULL)
    {
        int k, d;
        static const int dx[4] = { 1, -1, 0, 0 };
        static const int dy[4] = { 0, 0, 1, -1 };

        for (k = 0; k < grid_w * grid_h; k++)
        {
            if (reachable[k] == ROUTE_UNREACHED)
            {
                continue;
            }
            for (d = 0; d < 4; d++)
            {
                int nx = k % grid_w + dx[d];
                int ny = k / grid_w + dy[d];

                if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
                {
                    continue;
                }
                if (reachable[ny * grid_w + nx] == ROUTE_UNREACHED)
                {
                    continue;
                }
                if (can_cross(cell_x(k % grid_w), cell_y(k / grid_w), cell_x(nx), cell_y(ny)))
                {
                    edges[k] |= 1 << d;
                }
            }
        }
    }
    printf("API_Route: %dx%d cells at %d units, %d reachable, exit %d cells from the spawn\n",
           grid_w, grid_h, ROUTE_CELL, flood(probe, field, seed, queue),
           field[cy * grid_w + cx] == ROUTE_UNREACHED ? -1 : field[cy * grid_w + cx]);

    free(queue);
}

boolean API_Route(mobj_t *player, api_route_t *out)
{
    int cx, cy, at, steps;
    int bx, by;

    if (player == NULL)
    {
        return false;
    }
    ensure_built(player);
    if (field == NULL || !cell_of(player->x, player->y, &cx, &cy))
    {
        return false;
    }
    at = cy * grid_w + cx;
    if (field[at] == ROUTE_UNREACHED)
    {
        return false;
    }

    out->cells = field[at];
    out->have_step = false;
    if (field[at] == 0)
    {
        return true;
    }

    /* Walk downhill a few cells and aim at where that lands. */
    bx = cx;
    by = cy;
    for (steps = 0; steps < ROUTE_LOOKAHEAD; steps++)
    {
        static const int dx[4] = { 1, -1, 0, 0 };
        static const int dy[4] = { 0, 0, 1, -1 };
        int best = -1, bn = 0, d;

        for (d = 0; d < 4; d++)
        {
            int nx = bx + dx[d];
            int ny = by + dy[d];
            int ni;

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (field[ni] == ROUTE_UNREACHED)
            {
                continue;
            }
            if (best < 0 || field[ni] < field[bn])
            {
                best = ni;
                bn = ni;
            }
        }
        if (best < 0 || field[bn] >= field[by * grid_w + bx])
        {
            break;
        }
        bx = bn % grid_w;
        by = bn / grid_w;
        if (field[bn] == 0)
        {
            break;
        }
    }
    if (bx == cx && by == cy)
    {
        return true;
    }
    out->have_step = true;
    out->x = cell_x(bx);
    out->y = cell_y(by);
    return true;
}

void API_RouteForgetVisited(void)
{
    if (visited != NULL)
    {
        memset(visited, 0, grid_w * grid_h);
    }
}

void API_RouteMarkVisited(mobj_t *player)
{
    int cx, cy;

    if (visited == NULL || player == NULL || !cell_of(player->x, player->y, &cx, &cy))
    {
        return;
    }
    visited[cy * grid_w + cx] = 1;
}

/* The nearest place the player has not been, and the way to it.
 *
 * A breadth-first search from the player ACROSS ground already walked. That
 * crossing is the whole point: the naive alternative - penalise revisiting -
 * traps an agent on the near side of a strip it has already crossed, with
 * unexplored space beyond it. Here already-walked ground costs the same as
 * any other, and only the DESTINATION has to be new.
 */
boolean API_Frontier(mobj_t *player, api_route_t *out)
{
    int cx, cy, head = 0, tail = 0, found = -1;
    int *queue;
    int *parent;
    int i;

    if (player == NULL || reachable == NULL || edges == NULL || visited == NULL)
    {
        return false;
    }
    if (!cell_of(player->x, player->y, &cx, &cy)
        || reachable[cy * grid_w + cx] == ROUTE_UNREACHED)
    {
        return false;
    }
    queue = malloc(sizeof(int) * grid_w * grid_h);
    parent = malloc(sizeof(int) * grid_w * grid_h);
    if (queue == NULL || parent == NULL)
    {
        free(queue);
        free(parent);
        return false;
    }
    for (i = 0; i < grid_w * grid_h; i++)
    {
        parent[i] = -2;
    }
    parent[cy * grid_w + cx] = -1;
    queue[tail++] = cy * grid_w + cx;

    while (head < tail && found < 0)
    {
        int at = queue[head++];
        static const int dx[4] = { 1, -1, 0, 0 };
        static const int dy[4] = { 0, 0, 1, -1 };
        int d;

        for (d = 0; d < 4; d++)
        {
            int nx, ny, ni;

            if (!(edges[at] & (1 << d)))
            {
                continue;
            }
            nx = at % grid_w + dx[d];
            ny = at / grid_w + dy[d];
            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (parent[ni] != -2)
            {
                continue;
            }
            parent[ni] = at;
            if (!visited[ni])
            {
                found = ni;
                break;
            }
            queue[tail++] = ni;
        }
    }

    if (found < 0)
    {
        free(queue);
        free(parent);
        return false;
    }

    /* Path length, and a waypoint a few steps along it rather than the far
     * end - aiming at the frontier itself would cut corners through walls. */
    {
        int steps = 0;
        int at = found;
        int path[4096];
        int n = 0;

        while (at >= 0 && n < 4096)
        {
            path[n++] = at;
            at = parent[at];
        }
        out->cells = n - 1;
        steps = n - 1 - ROUTE_LOOKAHEAD;
        if (steps < 0)
        {
            steps = 0;
        }
        out->have_step = true;
        out->x = cell_x(path[steps] % grid_w);
        out->y = cell_y(path[steps] / grid_w);
    }
    free(queue);
    free(parent);
    return true;
}

/* The explored map, one byte a cell, for a viewer to draw.
 *
 * 0 nowhere the player can be, 1 reachable and not yet walked, 2 walked.
 * Exactly what the frontier search reads, so what a viewer shows and what the
 * agent decides on cannot drift apart.
 */
boolean API_RouteMap(api_map_t *out)
{
    int i;

    if (reachable == NULL || visited == NULL)
    {
        return false;
    }
    out->w = grid_w;
    out->h = grid_h;
    out->cell = ROUTE_CELL;
    out->x0 = grid_x0;
    out->y0 = grid_y0;
    out->cells = malloc(grid_w * grid_h);
    if (out->cells == NULL)
    {
        return false;
    }
    for (i = 0; i < grid_w * grid_h; i++)
    {
        out->cells[i] = reachable[i] == ROUTE_UNREACHED ? 0 : (visited[i] ? 2 : 1);
    }
    return true;
}

boolean API_RouteCellOf(fixed_t x, fixed_t y, int *cx, int *cy)
{
    if (reachable == NULL)
    {
        return false;
    }
    return cell_of(x, y, cx, cy) ? true : false;
}

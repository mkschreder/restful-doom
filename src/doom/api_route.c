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
#include "r_main.h"
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
/* Which cells a player can stand in, and which of the eight steps out of each
 * one they can actually take. These two are the ONE definition of what the
 * player may do on this grid: every flood fill and every route bearing reads
 * them and nothing re-derives them, so a field and the path followed down it
 * can no longer disagree. */
static unsigned char *walk;
static unsigned char *edges;  /* 8 bits a cell */
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
/* Whether the route is allowed through floor that hurts. Set only when
 * avoiding it leaves the exit unreachable - some levels have no dry path. */
static boolean allow_damage;

/* Sector specials that damage a player standing on them: nukage, lava and the
 * two "end level" ones. A route through these is a route that arrives dead -
 * measured on E1M1, health draining from 106 to 0 over five hundred decisions
 * with almost none of it from monsters, because the straight way across the
 * big room is through the slime. */
static boolean hurts(int special)
{
    return special == 4 || special == 5 || special == 7 || special == 16
        || special == 11;
}

static boolean walkable(mobj_t *probe, fixed_t x, fixed_t y)
{
    fixed_t ox = probe->x, oy = probe->y, oz = probe->z;
    boolean ok = P_CheckPosition(probe, x, y);

    probe->x = ox;
    probe->y = oy;
    probe->z = oz;
    if (ok && !allow_damage)
    {
        subsector_t *ss = R_PointInSubsector(x, y);

        if (ss != NULL && ss->sector != NULL && hurts(ss->sector->special))
        {
            return false;
        }
    }
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
 * The straight segment between the two centres, and nothing else. Sampling
 * parallel offsets as well was tried, to thread doorways on a coarse grid, and
 * it is WRONG: an offset sample finds a gap the centre path cannot use, so the
 * field acquires edges through walls. The symptom is the worst kind - a route
 * that looks authoritative and points at a wall sixteen units ahead. At 32
 * units a cell is half the player's width and doorways thread on the centre
 * line, so the offsets bought nothing and cost correctness.
 *
 * `walkable` is not enough on its own: P_CheckPosition passes at any point
 * with no linedef within the player's radius, which includes the solid void
 * between rooms. The line test is what distinguishes floor from void. */
static boolean can_cross(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    return clear_line(x1, y1, x2, y2);
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
/* Eight directions, orthogonals first so a tie prefers a straight move.
 *
 * FOUR was tried and is not enough. A 4-connected field can only descend in
 * right angles, so its gradient staircases and ties flip between neighbours as
 * the player crosses a cell boundary - measured, a route bearing reading -3,
 * 3, 8, 168, 87, 2, 5 on consecutive decisions, which is not something anything
 * can follow. Eight directions give a gradient that turns smoothly. */
static const int ROUTE_DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int ROUTE_DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

static int flood(unsigned short *out, int seed, int *queue)
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
        int d;

        for (d = 0; d < 8; d++)
        {
            int ni;

            if (!(edges[at] & (1 << d)))
            {
                continue;
            }
            ni = (ay + ROUTE_DY[d]) * grid_w + ax + ROUTE_DX[d];
            if (out[ni] != ROUTE_UNREACHED)
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

/* Every step a player can take on this grid, computed once.
 *
 * A cell pair is joined when a sight ray between the centres crosses no solid
 * line AND a 32-unit-wide body fits at the midpoint. The ray alone is what a
 * route used to be built from, and it is not enough: a ray is a point and
 * threads gaps a body wedges in. A diagonal additionally needs both of its
 * orthogonals, or the path squeezes through the corner where two walls meet. */
static void build_edges(mobj_t *probe)
{
    int k, d;

    for (k = 0; k < grid_w * grid_h; k++)
    {
        int ax = k % grid_w, ay = k / grid_w;

        edges[k] = 0;
        if (!walk[k])
        {
            continue;
        }
        for (d = 0; d < 8; d++)
        {
            int nx = ax + ROUTE_DX[d];
            int ny = ay + ROUTE_DY[d];

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            if (!walk[ny * grid_w + nx])
            {
                continue;
            }
            if (d >= 4 && (!walk[ay * grid_w + nx] || !walk[ny * grid_w + ax]))
            {
                continue;
            }
            if (!can_cross(cell_x(ax), cell_y(ay), cell_x(nx), cell_y(ny)))
            {
                continue;
            }
            if (!walkable(probe, (cell_x(ax) + cell_x(nx)) / 2,
                                 (cell_y(ay) + cell_y(ny)) / 2))
            {
                continue;
            }
            edges[k] |= 1 << d;
        }
    }
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
    if (built_for != (void *)lines)
    {
        /* A new level: try the dry route again. */
        allow_damage = false;
    }
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
    free(walk);
    free(edges);
    free(visited);
    walk = calloc(1, grid_w * grid_h);
    edges = calloc(1, grid_w * grid_h);
    visited = calloc(1, grid_w * grid_h);
    if (walk == NULL || edges == NULL || visited == NULL)
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
        walk[i] = walkable(probe, cell_x(i % grid_w), cell_y(i / grid_w)) ? 1 : 0;
    }
    build_edges(probe);

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
    flood(reach, cy * grid_w + cx, queue);

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
        if (!allow_damage)
        {
            /* Some levels have no dry path. Rather than report no route at
             * all, walk through the slime and let the agent deal with it. */
            printf("API_Route: no dry path to the exit; allowing damaging floor\n");
            allow_damage = true;
            built_for = NULL;
            free(queue);
            free(field);
            field = NULL;
            ensure_built(probe);
            return;
        }
        printf("API_Route: none of the %d standing spots at the exit is reachable\n", n_spots);
        free(queue);
        free(field);
        field = NULL;
        return;
    }
    {
        /* Sequenced deliberately: C does not order function arguments, and
         * reading the field in the same printf that fills it reported the
         * spawn as unreachable on a field that had not been built yet. */
        int got = flood(field, seed, queue);
        int at_spawn = field[cy * grid_w + cx];

        printf("API_Route: %dx%d cells at %d units, %d reachable, exit %d cells from the spawn%s\n",
               grid_w, grid_h, ROUTE_CELL, got,
               at_spawn == ROUTE_UNREACHED ? -1 : at_spawn,
               allow_damage ? " (through damaging floor)" : "");
    }

    free(queue);
}

/* Could the player WALK to (x,y) in a straight line from where they are?
 *
 * Stricter than clear_line, and it has to be: clear_line asks whether a sight
 * ray gets through, and a gap a ray threads can be far too narrow for a body
 * 32 units wide, or on the other side of a step too high to climb. Choosing a
 * waypoint the player can see but cannot reach produces a route bearing that
 * is perfectly stable and points at a wall - measured, eighteen consecutive
 * decisions walking forward at a bearing of -1 without moving. */
static fixed_t walk_probe_z;
static boolean walk_blocked;
static line_t *walk_block_line;
static fixed_t walk_block_frac;

static boolean PTR_WalkTraverse(intercept_t *in)
{
    line_t *ld = in->d.line;

    if (ld->backsector == NULL || ld->frontsector == NULL || (ld->flags & ML_BLOCKING))
    {
        walk_blocked = true;
        walk_block_line = ld;
        walk_block_frac = in->frac;
        return false;
    }
    P_LineOpening(ld);
    /* 56 is the player's height and 24 the tallest step they can climb, both
     * from p_map.c. A shut door has an openrange of zero and is deliberately
     * still refused here: the route may go through it, but the player cannot
     * walk to a point beyond it until it is open. */
    if (openrange < 56 * FRACUNIT || openbottom - walk_probe_z > 24 * FRACUNIT)
    {
        walk_blocked = true;
        walk_block_line = ld;
        walk_block_frac = in->frac;
        return false;
    }
    return true;
}

static boolean can_walk_to(mobj_t *player, fixed_t x, fixed_t y)
{
    walk_blocked = false;
    walk_block_line = NULL;
    walk_probe_z = player->z;
    P_PathTraverse(player->x, player->y, x, y, PT_ADDLINES, PTR_WalkTraverse);
    return !walk_blocked;
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
    out->blocked = false;
    out->can_open = false;
    if (field[at] == 0)
    {
        return true;
    }

    /* Walk downhill and aim at the furthest point still in a straight line
     * from the player.
     *
     * Only steps the player can actually take are considered. A cell's
     * distance being one less than this one's does NOT mean the two are
     * joined: two cells either side of a thin wall differ by one all the way
     * along it, and a descent that ignores `edges` walks straight into it. The
     * symptom was a route bearing pointing at a wall twenty units ahead, held
     * perfectly steady while the player stood still.
     *
     * The lookahead is what makes following smooth - aiming at the very next
     * cell flips the target every time a boundary is crossed - but a fixed
     * lookahead cuts corners: where the path bends, the straight line to a
     * point four cells along goes through the wall it bends around. So the
     * candidate is only accepted while the player could walk straight at it. */
    bx = cx;
    by = cy;
    {
        int px = cx, py = cy;
        int first = -1;

        for (steps = 0; steps < ROUTE_LOOKAHEAD; steps++)
        {
            int bn = -1, d;

            for (d = 0; d < 8; d++)
            {
                int ni;

                if (!(edges[py * grid_w + px] & (1 << d)))
                {
                    continue;
                }
                ni = (py + ROUTE_DY[d]) * grid_w + px + ROUTE_DX[d];
                if (field[ni] == ROUTE_UNREACHED)
                {
                    continue;
                }
                if (bn < 0 || field[ni] < field[bn])
                {
                    bn = ni;
                }
            }
            if (bn < 0 || field[bn] >= field[py * grid_w + px])
            {
                break;
            }
            px = bn % grid_w;
            py = bn / grid_w;
            if (first < 0)
            {
                first = bn;
            }
            if (!can_walk_to(player, cell_x(px), cell_y(py)))
            {
                break;
            }
            bx = px;
            by = py;
            if (field[bn] == 0)
            {
                break;
            }
        }

        /* Not even the next cell was straight ahead. Say WHAT is in the way.
         *
         * The field deliberately runs through shut doors, because a player
         * opens them; so "the route says go east and the player cannot" is the
         * normal state of affairs at every door in the game, and an agent that
         * is only told a bearing can do nothing but walk into it. Measured on
         * E1M1: the route was right, the door at (1519,-2448) was shut, and the
         * follower pressed forward against it until the episode ran out.
         *
         * Aiming at the next cell anyway was tried instead, on the theory that
         * the engine slides a player along what they brush, and it is how a
         * follower came to spend two hundred and seventy-five decisions pressed
         * into a corner at a bearing of zero. */
        if (bx == cx && by == cy && first >= 0)
        {
            fixed_t tx = cell_x(first % grid_w);
            fixed_t ty = cell_y(first / grid_w);

            if (!can_walk_to(player, tx, ty) && walk_block_line != NULL)
            {
                line_t *ld = walk_block_line;

                out->blocked = true;
                out->can_open = ld->special != 0;
                out->block_x = player->x + FixedMul(tx - player->x, walk_block_frac);
                out->block_y = player->y + FixedMul(ty - player->y, walk_block_frac);
            }
            /* Off the middle of their own cell and hugging something: steer
             * back to that middle, which is at most half a cell away and is a
             * place the player is known to fit. Only worth saying when it is
             * far enough to be a direction rather than noise. */
            if (P_AproxDistance(cell_x(cx) - player->x, cell_y(cy) - player->y)
                    > 12 * FRACUNIT
                && can_walk_to(player, cell_x(cx), cell_y(cy)))
            {
                out->have_step = true;
                out->x = cell_x(cx);
                out->y = cell_y(cy);
                return true;
            }
            bx = first % grid_w;
            by = first / grid_w;
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

cJSON *API_RouteDebug(mobj_t *player)
{
    cJSON *root, *arr;
    api_route_t r;
    int cx, cy, at, d;

    if (player == NULL)
    {
        return NULL;
    }
    ensure_built(player);
    root = cJSON_CreateObject();
    if (field == NULL || !cell_of(player->x, player->y, &cx, &cy))
    {
        cJSON_AddStringToObject(root, "error", "no field, or the player is off the grid");
        return root;
    }
    at = cy * grid_w + cx;
    cJSON_AddNumberToObject(root, "cell", ROUTE_CELL);
    cJSON_AddNumberToObject(root, "cx", cx);
    cJSON_AddNumberToObject(root, "cy", cy);
    cJSON_AddNumberToObject(root, "centerX", cell_x(cx) >> FRACBITS);
    cJSON_AddNumberToObject(root, "centerY", cell_y(cy) >> FRACBITS);
    cJSON_AddBoolToObject(root, "walkable", walk[at] != 0);
    cJSON_AddNumberToObject(root, "distance",
                            field[at] == ROUTE_UNREACHED ? -1 : field[at]);

    arr = cJSON_CreateArray();
    for (d = 0; d < 8; d++)
    {
        int nx = cx + ROUTE_DX[d];
        int ny = cy + ROUTE_DY[d];
        cJSON *o;

        if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
        {
            continue;
        }
        o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "dx", ROUTE_DX[d]);
        cJSON_AddNumberToObject(o, "dy", ROUTE_DY[d]);
        cJSON_AddBoolToObject(o, "walkable", walk[ny * grid_w + nx] != 0);
        cJSON_AddBoolToObject(o, "edge", (edges[at] & (1 << d)) != 0);
        cJSON_AddNumberToObject(o, "distance",
                                field[ny * grid_w + nx] == ROUTE_UNREACHED
                                    ? -1 : field[ny * grid_w + nx]);
        cJSON_AddBoolToObject(o, "canWalkTo",
                              can_walk_to(player, cell_x(nx), cell_y(ny)));
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "steps", arr);

    if (API_Route(player, &r) && r.have_step)
    {
        cJSON_AddNumberToObject(root, "aimX", r.x >> FRACBITS);
        cJSON_AddNumberToObject(root, "aimY", r.y >> FRACBITS);
    }
    return root;
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
        int d;

        for (d = 0; d < 8; d++)
        {
            int nx, ny, ni;

            if (!(edges[at] & (1 << d)))
            {
                continue;
            }
            nx = at % grid_w + ROUTE_DX[d];
            ny = at / grid_w + ROUTE_DY[d];
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

/* A reachable place whose route distance to the exit is closest to `units`.
 *
 * This is what a curriculum wants: not "N decisions of random walk from the
 * goal", which wanders and whose difficulty nobody can state, but "start this
 * far along the path", which is a number that means the same thing every time
 * and can be grown to the whole level. `seed` picks among the cells at that
 * distance so episodes differ.
 */
boolean API_RouteSpotAt(int units, unsigned int seed, fixed_t *x, fixed_t *y)
{
    int target = units / ROUTE_CELL;
    int best = -1;
    int best_err = 0;
    int count = 0;
    int i;

    if (field == NULL)
    {
        return false;
    }
    /* Two passes: find how close anything gets, then choose among the cells
     * that tie, so a curriculum stage does not always start in one spot. */
    for (i = 0; i < grid_w * grid_h; i++)
    {
        int err;

        if (field[i] == ROUTE_UNREACHED)
        {
            continue;
        }
        err = abs((int)field[i] - target);
        if (best < 0 || err < best_err)
        {
            best_err = err;
            best = i;
        }
    }
    if (best < 0)
    {
        return false;
    }
    for (i = 0; i < grid_w * grid_h; i++)
    {
        if (field[i] != ROUTE_UNREACHED && abs((int)field[i] - target) == best_err)
        {
            count++;
        }
    }
    count = count > 0 ? (int)(seed % (unsigned int)count) : 0;
    for (i = 0; i < grid_w * grid_h; i++)
    {
        if (field[i] != ROUTE_UNREACHED && abs((int)field[i] - target) == best_err)
        {
            if (count-- == 0)
            {
                *x = cell_x(i % grid_w);
                *y = cell_y(i / grid_w);
                return true;
            }
        }
    }
    return false;
}

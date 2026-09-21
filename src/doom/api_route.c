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
#include "api_object_controller.h"
#include "doomstat.h"
#include "p_local.h"
#include "r_main.h"
#include "i_timer.h"
#include "r_state.h"
#include "m_misc.h"

/* Grid resolution. The player is 32 units wide, so a 64-unit cell is about one
 * of them and a corridor is one or two cells across. */
#define ROUTE_CELL 32
#define ROUTE_MAX_CELLS 262144
#define ROUTE_UNREACHED 0xffff
/* How far down the gradient to aim. One cell is jittery - the target flips
 * between neighbours as the player crosses a boundary - and too far cuts
 * corners through walls. Four cells is about one decision of walking. */
#define ROUTE_LOOKAHEAD 4
/* How far off a teleport line the cell that steps across it is sampled. A
 * player is 32 units wide, so anything less samples the line itself and the
 * side test becomes a coin flip. */
#define PORTAL_STANDOFF 24
/* How far to look for the route when the player is standing off it, in cells.
 * Six is about two of the player's own decisions of walking. */
#define ROUTE_RECOVER 6
/* The player's own height, and the tallest step they can walk up, matching
 * P_TryMove's own rules. A crossing with less room than this is not one. */
#define ROUTE_HEIGHT (56 * FRACUNIT)

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
/* The keys that refused a step out of each cell, as a mask of API_KEY_*.
 *
 * Kept per cell rather than as one running total, because which keys matter
 * is a question about the EDGE of what the player can reach: a red door on
 * the far side of the level is not what is stopping them, and heading for its
 * key is a worse answer than heading for nothing. The flood is what says
 * which cells those are, and it has not run when the steps are tested.
 *
 * Recorded for both cells of a pair, because a pair is tested once and the
 * step written in both directions. Approximate in exactly the way the step
 * itself is, and it stops being approximate when the step does. */
static unsigned char *locked_at;

/* A one-way jump between two cells that is not a step.
 *
 * A teleport linedef moves the player instantly, and the grid is geometry: the
 * two cells either side of one are as far apart as the level is wide. Without
 * these the route can never AIM at a teleporter as a way of getting anywhere,
 * which from E1M5 onward is how a good deal of the shareware episode is meant
 * to be crossed.
 *
 * `portal_first[cell]` indexes `portal_to`, and `portal_next` chains the rest,
 * because the flood asks "what can I reach from here" once per cell and
 * scanning a list every time is the flood's inner loop. */
static int *portal_first;
static int *portal_to;
static int *portal_next;
static int portal_count;
/* Where the player has been this episode. */
static unsigned char *visited;
static int grid_w, grid_h;
static fixed_t grid_x0, grid_y0;
static void *built_for;
static int built_count;
static int built_keys = -1;
/* What the field is flooded from. Normally the exit; when the exit is behind a
 * door the player has no key for, the key itself - the level's own next
 * objective, which is what a player would head for. */
static int route_goal_key;

static boolean route_blocked;

/* Which keys the player had when this grid was built, and which ones the grid
 * found itself needing. A locked door is a wall to a player without its key
 * and an open doorway to one with it, so the two are different maps and the
 * build is keyed on which of them applies. */
static int held_keys;
static int keys_wanted;
/* Shut sectors on the way to the exit that only a switch elsewhere opens, and
 * the one the route has settled on leading to. A level's lifts and its
 * switch-operated doors are these, and they are as much a part of the way
 * through as a keycard is. */
#define ROUTE_MAX_WANTED 16
static line_t *wanted_lines[ROUTE_MAX_WANTED];
static int wanted_count;
static boolean route_goal_switch;
/* Set when the level has changed under the field and it must be rebuilt. */
static boolean field_stale;
/* Whether the route may only cross ground the player has actually laid eyes
 * on.
 *
 * This is the difference between an agent that has been handed the level's
 * solution and one that has to find it. A distance field flooded over the
 * whole map answers "which way to the exit" from the first tic of a level
 * nobody has walked a step of - through rooms behind doors that have not been
 * opened, past a key whose existence is not yet known. Following it is not
 * playing DOOM; it is following a line painted on the floor, and a policy
 * trained against it learns one level's line rather than how to look for a
 * way out.
 *
 * The engine already knows exactly what the player has seen, because the
 * renderer marks every wall it draws with ML_MAPPED for the automap. That is
 * the honest map, and it is what this floods over. */
static boolean fair_play = true;
/* Per cell: whether the player has seen the sector it is in, and which sector
 * that is. The second is a lookup built with the grid, so re-deciding the
 * first as the player explores costs a pass over the cells rather than a pass
 * over the level's geometry. */
static unsigned char *seen;
static int *cell_sector;
/* How many of the level's lines were on the automap when `seen` was filled. */
static int seen_from;
/* The cell the player was in when the field was last flooded. The frontier is
 * chosen relative to it, so it is half of what the answer depends on. */
static int flooded_from = -1;
/* Set when the route leads to unexplored ground rather than to the exit, a
 * key or a switch: the player has not found the way out yet, and looking for
 * it is the job. */
static boolean route_goal_frontier;
/* The unseen cell just beyond the frontier the route is heading for. Walking
 * into it is what turns it into somewhere seen. */
static int frontier_step = -1;
/* The switch the route is leading to, when it is leading to one. */
static fixed_t route_switch_x, route_switch_y;

/* The tagged sector either side of a line that something else opens. */
static sector_t *shut_sector_of(line_t *ld)
{
    if (ld->backsector != NULL && ld->backsector->tag != 0)
    {
        return ld->backsector;
    }
    if (ld->frontsector != NULL && ld->frontsector->tag != 0)
    {
        return ld->frontsector;
    }
    return NULL;
}

static void want_line(line_t *ld)
{
    int i;

    if (shut_sector_of(ld) == NULL)
    {
        return;
    }
    for (i = 0; i < wanted_count; i++)
    {
        if (wanted_lines[i] == ld)
        {
            return;
        }
    }
    if (wanted_count < ROUTE_MAX_WANTED)
    {
        wanted_lines[wanted_count++] = ld;
    }
}

/* The colour of key a line demands, or 0. Both the doors a player opens by
 * walking into them and the ones a switch opens remotely; the numbers are
 * p_doors.c's own. */
/* Steps refused because a locked door stood in them, this build. Counted
 * because "the route crossed a door it has no key for" and "the route found
 * another way round" are indistinguishable from the outside, and the first is
 * a bug that reads as a working route right up until the player is standing
 * at the door. */
static int dropped_locked;

static int lock_of(line_t *ld)
{
    switch (ld->special)
    {
        case 26: case 32: case 99:  case 133: return API_KEY_BLUE;
        case 27: case 34: case 136: case 137: return API_KEY_YELLOW;
        case 28: case 33: case 134: case 135: return API_KEY_RED;
        default: return 0;
    }
}

/* Blocked only by things a player can never cross: a one-sided wall, a line
 * the author flagged as blocking, or a door locked against the keys they are
 * carrying. A shut door they CAN open is two-sided and is not blocked - they
 * open it - which is why the route runs through doors and the agent is told
 * about the one in front of it. */
/* Whether anything in the level can ever move this line's opening.
 *
 * The distinction this draws is the one between a shut door and a wall, and
 * both look identical from the geometry: a two-sided linedef with no gap
 * between floor and ceiling. A door has a special on the line itself, or a
 * tagged back sector that some switch or trigger elsewhere operates. A wall
 * has neither - and DOOM is full of walls built that way, because a sector of
 * zero height is how a mapper of 1993 draws a diagonal with its own texture. */
static boolean line_can_open(line_t *ld)
{
    return ld->special != 0 || (ld->backsector != NULL && ld->backsector->tag != 0);
}

/* Whether the player could ever get through this line.
 *
 * A closed door counts as passable: the field is built on a player opening
 * the doors in front of them. A locked one does not, until its key is held -
 * and the colour is remembered, because a key the route needs is the level's
 * own next objective. */
static boolean line_passable(line_t *ld)
{
    int lock;

    if (ld->backsector == NULL || ld->frontsector == NULL || (ld->flags & ML_BLOCKING))
    {
        return false;
    }
    /* Two-sided and no way through. A SHUT DOOR looks exactly like this and
     * must stay crossable, so the test is not "is there room now" but "can
     * there ever be".
     *
     * Without this the grid ran an edge straight through E1M2's diagonal
     * walls, which are zero-height sectors rather than one-sided lines: the
     * route crossed them, the player could not, and a follower oscillated
     * across three cells for six hundred decisions with the bearing pointing
     * into the wall twenty-five units ahead. */
    P_LineOpening(ld);
    if (openrange < ROUTE_HEIGHT)
    {
        if (!line_can_open(ld))
        {
            return false;
        }
        if (ld->special == 0)
        {
            /* Shut, and nothing about THIS line opens it: a switch somewhere
             * else does. That is a wall as far as walking is concerned, in
             * exactly the way a locked door is - the player has to go and do
             * something first - so it is treated the same way, and the sector
             * is remembered so the route can lead to its switch. */
            want_line(ld);
            return false;
        }
    }
    lock = lock_of(ld);
    if (lock != 0 && !(held_keys & (1 << lock)))
    {
        dropped_locked++;
        return false;
    }
    return true;
}

/* The keys a player is carrying, as a mask of API_KEY_*. A card and a skull of
 * the same colour open the same doors, so they are one thing here. */
static int keys_of(mobj_t *probe)
{
    player_t *p = probe->player;
    int mask = 0;

    if (p == NULL)
    {
        return 0;
    }
    if (p->cards[it_bluecard] || p->cards[it_blueskull])     mask |= 1 << API_KEY_BLUE;
    if (p->cards[it_yellowcard] || p->cards[it_yellowskull]) mask |= 1 << API_KEY_YELLOW;
    if (p->cards[it_redcard] || p->cards[it_redskull])       mask |= 1 << API_KEY_RED;
    return mask;
}

/* Where a key of this colour is lying, if one is. Keys are identified by their
 * sprite, which is how p_inter.c decides what picking one up gives you. */
static mobj_t *find_key(int colour)
{
    thinker_t *th;

    for (th = thinkercap.next; th != &thinkercap && th != NULL; th = th->next)
    {
        mobj_t *mo;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
        {
            continue;
        }
        mo = (mobj_t *)th;
        switch (mo->sprite)
        {
            case SPR_BKEY: case SPR_BSKU:
                if (colour == API_KEY_BLUE) return mo;
                break;
            case SPR_YKEY: case SPR_YSKU:
                if (colour == API_KEY_YELLOW) return mo;
                break;
            case SPR_RKEY: case SPR_RSKU:
                if (colour == API_KEY_RED) return mo;
                break;
            default:
                break;
        }
    }
    return NULL;
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

/* Where the LEVEL lets a player stand - not where one could stand right now.
 *
 * Things are deliberately ignored. The grid is built once, and a monster
 * asleep in a doorway when the level loads is not a wall; recording it as one
 * cuts the map into pieces for the rest of the episode, long after the monster
 * has wandered off or died. E1M3 has 74 of them and its exit came out
 * unreachable from its own spawn; with -nomonsters the same code found it 76
 * cells away. What is standing in the way NOW is a question for the
 * observation, which answers it every step. */
static boolean walkable(mobj_t *probe, fixed_t x, fixed_t y)
{
    fixed_t ox = probe->x, oy = probe->y, oz = probe->z;
    boolean ok = P_CheckPositionLines(probe, x, y);

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

/* The segment being tested, and whether anything in the way stopped it. */
static divline_t cross_trace;
static boolean cross_blocked;

/* The line that refused the last crossing test, and the key it wanted.
 *
 * "A wall in the way" and "a door you have no key for" look identical from a
 * grid of heights, and they want completely different things done. */
static line_t *cross_blocker;
static int cross_lock;

static boolean PIT_CrossLine(line_t *ld)
{
    divline_t dl;

    /* Both ends of the line on the same side of the segment, or both ends of
     * the segment on the same side of the line: they do not cross. */
    if (P_PointOnDivlineSide(ld->v1->x, ld->v1->y, &cross_trace)
        == P_PointOnDivlineSide(ld->v2->x, ld->v2->y, &cross_trace))
    {
        return true;
    }
    dl.x = ld->v1->x;
    dl.y = ld->v1->y;
    dl.dx = ld->v2->x - ld->v1->x;
    dl.dy = ld->v2->y - ld->v1->y;
    if (P_PointOnDivlineSide(cross_trace.x, cross_trace.y, &dl)
        == P_PointOnDivlineSide(cross_trace.x + cross_trace.dx,
                                cross_trace.y + cross_trace.dy, &dl))
    {
        return true;
    }
    if (!line_passable(ld))
    {
        cross_blocked = true;
        cross_blocker = ld;
        cross_lock = lock_of(ld);
        return false;
    }
    return true;
}

/* Whether a player could walk the straight line between two points, as far as
 * the LINES are concerned.
 *
 * Every line in the segment's bounding box, rather than P_PathTraverse along
 * it. The engine's own traverse walks the blockmap with a DDA that steps in
 * one axis at a time, and a trace running at exactly 45 degrees - which is
 * every diagonal step on a square grid - can pass through a block corner and
 * skip the block beyond it. Measured on E1M3: the step north out of cell
 * (-1264,-2400) sees the blue door and is refused, and the step north-east
 * out of the same cell, crossing the same door sixteen units along it, sees
 * nothing at all. The route then ran through a locked door, never asked for
 * the blue key, and the player stood at that door for thirteen hundred
 * decisions. A box test cannot have that failure: it looks at every line that
 * could possibly cross, and asks each one directly. */
static boolean clear_line(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    int xl, xh, yl, yh, bx, by;
    fixed_t lox = x1 < x2 ? x1 : x2;
    fixed_t hix = x1 < x2 ? x2 : x1;
    fixed_t loy = y1 < y2 ? y1 : y2;
    fixed_t hiy = y1 < y2 ? y2 : y1;

    cross_trace.x = x1;
    cross_trace.y = y1;
    cross_trace.dx = x2 - x1;
    cross_trace.dy = y2 - y1;
    cross_blocked = false;
    cross_blocker = NULL;
    cross_lock = 0;

    xl = (lox - bmaporgx) >> MAPBLOCKSHIFT;
    xh = (hix - bmaporgx) >> MAPBLOCKSHIFT;
    yl = (loy - bmaporgy) >> MAPBLOCKSHIFT;
    yh = (hiy - bmaporgy) >> MAPBLOCKSHIFT;
    if (xl < 0) xl = 0;
    if (yl < 0) yl = 0;
    if (xh >= bmapwidth) xh = bmapwidth - 1;
    if (yh >= bmapheight) yh = bmapheight - 1;

    validcount++;
    for (bx = xl; bx <= xh && !cross_blocked; bx++)
    {
        for (by = yl; by <= yh && !cross_blocked; by++)
        {
            P_BlockLinesIterator(bx, by, PIT_CrossLine);
        }
    }
    return !cross_blocked;
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
    /* The normal exit first, and the secret one only if the level has no
     * other. They are different objectives: a secret exit ends the level
     * somewhere else entirely, and the level is not built to lead you to it -
     * it is built to hide it. E1M3 has both, the secret one at a lower line
     * number, and taking the first match sent every route to it: across a
     * hellslime pit the player has no business in, with the blue key the
     * NORMAL exit needs never asked for, and the scripted player dead in the
     * slime at decision 58. */
    boolean secret;

    for (secret = false; secret <= true && n == 0; secret++)
    {
    for (i = 0; i < numlines && n < max; i++)
    {
        line_t *ld = &lines[i];
        fixed_t dx, dy, len;
        unsigned int oi, ai;
        int side;
        boolean is_exit = ld->special == 11 || ld->special == 52;
        boolean is_secret = ld->special == 51 || ld->special == 124;

        if (secret ? !is_secret : !is_exit)
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
    }
    return n;
}

/* How many of the level's lines the player has laid eyes on. The renderer
 * sets ML_MAPPED on every wall segment it draws, so this is the game's own
 * account of what has been seen, not a second one kept alongside it. */
static int mapped_lines(void)
{
    int i, n = 0;

    for (i = 0; i < numlines; i++)
    {
        if (lines[i].flags & ML_MAPPED)
        {
            n++;
        }
    }
    return n;
}

/* Fill `seen` from the automap: a cell counts as seen when the sector it sits
 * in has any wall the player has looked at. Sector-grained rather than
 * cell-grained because that is how a person remembers a building - you see a
 * room, not a square foot of it - and because a sector is the unit the engine
 * already tracks. */
static void mark_seen(mobj_t *probe)
{
    unsigned char *sector_seen;
    int i;

    if (seen == NULL || cell_sector == NULL)
    {
        return;
    }
    seen_from = mapped_lines();
    sector_seen = calloc(1, numsectors);
    if (sector_seen == NULL)
    {
        return;
    }
    for (i = 0; i < numlines; i++)
    {
        if (!(lines[i].flags & ML_MAPPED))
        {
            continue;
        }
        if (lines[i].frontsector != NULL)
        {
            sector_seen[lines[i].frontsector - sectors] = 1;
        }
        /* The far side only if there is a gap to see through it. Looking at
         * a shut door does not show you the room behind it, and marking it
         * seen anyway spread the explored region through every door in the
         * level in a few dozen decisions - after which there was no frontier
         * anywhere, no goal of any kind, and the agent span on the spot with
         * four fifths of the level still unwalked. */
        if (lines[i].backsector != NULL)
        {
            P_LineOpening(&lines[i]);
            if (openrange > 0)
            {
                sector_seen[lines[i].backsector - sectors] = 1;
            }
        }
    }
    /* And the room the player is standing in, whatever the automap says. A
     * level's first observation happens before a single frame has been drawn,
     * so ML_MAPPED is empty and the player could route nowhere at all - not
     * even out of the room they can plainly see around them. */
    if (probe != NULL && probe->subsector != NULL && probe->subsector->sector != NULL)
    {
        sector_seen[probe->subsector->sector - sectors] = 1;
    }
    for (i = 0; i < grid_w * grid_h; i++)
    {
        seen[i] = cell_sector[i] >= 0 && sector_seen[cell_sector[i]];
    }
    /* And the cell the player is standing in, whichever sector its CENTRE
     * happens to fall in. Near a boundary those are different sectors, and
     * when the neighbour is one nobody has seen the player's own cell came
     * out unseen - so the flood started nowhere, there was no route at all,
     * and the agent span on the spot for the rest of the episode. */
    if (probe != NULL)
    {
        int px, py;

        if (cell_of(probe->x, probe->y, &px, &py))
        {
            seen[py * grid_w + px] = 1;
        }
    }
    free(sector_seen);
}

void API_RouteInvalidate(void)
{
    built_for = NULL;
    built_count = 0;
    built_keys = -1;
    seen_from = -1;
    field_stale = true;
}

/* Whether the route is allowed to cross this cell at all. */
static boolean open_to_route(int cell)
{
    return walk[cell] && (!fair_play || seen == NULL || seen[cell]);
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
    if (!open_to_route(seed))
    {
        return 0;
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
            if (out[ni] != ROUTE_UNREACHED || !open_to_route(ni))
            {
                continue;
            }
            out[ni] = out[at] + 1;
            queue[tail++] = ni;
            reached++;
        }
        /* And whatever this cell teleports to. The destination is entered
         * without being crossed to, so it is admitted whether or not the
         * player has seen it: stepping on a pad is something a player can do
         * knowing only that it is a pad. What lies PAST it still has to have
         * been seen, because the ordinary test applies from there on, so
         * under fair play a route through an unused teleporter reaches the
         * landing spot and stops - which is exactly what is known. */
        for (d = portal_first[at]; d >= 0; d = portal_next[d])
        {
            int ni = portal_to[d];

            if (out[ni] != ROUTE_UNREACHED || !walk[ni])
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

/* Is there room for a 32-unit body to pass between two cells?
 *
 * On the line between the centres, or a little to either side of it. The
 * sideways search is what makes this a test of the GAP rather than of one
 * arbitrary point: a doorway 32 units wide is exactly the player's width, and
 * whether its middle lines up with a cell boundary is an accident of where the
 * level sits on the grid. Requiring the midpoint itself severed real passages
 * and left E1M2's exit unreachable from its own spawn.
 *
 * Half a cell either side and no further. Sampling a whole cell out finds gaps
 * the crossing cannot use and the field acquires steps through walls - the
 * worst symptom there is, a route that looks authoritative and points at a
 * wall sixteen units ahead. */
static boolean body_fits(mobj_t *probe, fixed_t x, fixed_t y, int d)
{
    /* (-dy, dx): across the direction of travel, which is where the room to
     * pass has to be. (dy, dx) is not perpendicular to a diagonal - it is the
     * diagonal again - so that spelling tested the same line twice. */
    fixed_t px = -ROUTE_DY[d] * FRACUNIT;
    fixed_t py = ROUTE_DX[d] * FRACUNIT;
    int off;

    for (off = 0; off <= ROUTE_CELL / 2; off += ROUTE_CELL / 8)
    {
        if (walkable(probe, x + px * off, y + py * off)
            || walkable(probe, x - px * off, y - py * off))
        {
            return true;
        }
    }
    return false;
}

/* The direction that undoes each of the eight, so a pair of cells is tested
 * once and the answer written into both. */
static const int ROUTE_OPPOSITE[8] = { 1, 0, 3, 2, 7, 6, 5, 4 };

/* How many steps survived each test, for the line the build prints. A level
 * whose rooms come out disconnected shows up here as a suspicious number
 * rather than as an agent that mysteriously never finds the exit. */
static int kept_straight, kept_diagonal, dropped_wall, dropped_body;

/* Every step a player can take on this grid, computed once.
 *
 * Straight steps first. Both cells must hold a 32-unit body, a sight line
 * between the centres must cross no solid line, and a body must fit somewhere
 * across the gap between them. The sight line alone is not enough: it is a
 * point, and a point threads gaps a body wedges in.
 *
 * Diagonals second, because a diagonal is only allowed where one of the two
 * L-shaped ways round it is allowed - as STEPS, not merely as cells a player
 * fits in, which is why they cannot be done in the same pass. Both cells
 * either side of a corner being roomy says nothing about getting between
 * them, and on E1M1 that let the route cut across a corner whose southward
 * step is a wall, with no other way down the field from that cell.
 *
 * Either way round and not both: a corridor running diagonally has one of its
 * two legs in a wall at every step along it. */
/* Where a teleport line with this tag puts the player, if anywhere.
 *
 * The same search the engine itself does when the line is crossed - the
 * sectors carrying the tag, and the teleport destination standing in one of
 * them - so the route and the game agree about where a pad leads. */
static boolean teleport_target(int tag, fixed_t *tx, fixed_t *ty)
{
    int i;

    for (i = 0; i < numsectors; i++)
    {
        thinker_t *thinker;

        if (sectors[i].tag != tag)
        {
            continue;
        }
        for (thinker = thinkercap.next; thinker != &thinkercap;
             thinker = thinker->next)
        {
            mobj_t *m = (mobj_t *)thinker;

            if (thinker->function.acp1 != (actionf_p1)P_MobjThinker
                || m->type != MT_TELEPORTMAN
                || m->subsector->sector - sectors != i)
            {
                continue;
            }
            *tx = m->x;
            *ty = m->y;
            return true;
        }
    }
    return false;
}

static void add_portal(int from, int to)
{
    int i;

    if (from == to || portal_count >= grid_w * grid_h)
    {
        return;
    }
    /* A line crosses many cells and every one of them leads to the same
     * place, so the same pair arrives over and over. */
    for (i = portal_first[from]; i >= 0; i = portal_next[i])
    {
        if (portal_to[i] == to)
        {
            return;
        }
    }
    portal_to[portal_count] = to;
    portal_next[portal_count] = portal_first[from];
    portal_first[from] = portal_count;
    portal_count++;
}

/* Every cell from which stepping forward crosses a teleport line, joined to
 * the cell the player lands in.
 *
 * Only the FRONT side: the engine refuses a teleport entered from the back,
 * which is what lets a player walk off a pad they have just arrived on, and a
 * route that did not know that would send them back and forth forever.
 *
 * Only the specials a PLAYER can cross. The monster-only ones move monsters
 * and would be a route the player cannot walk. */
static void build_portals(void)
{
    int i, lines_seen = 0, lines_resolved = 0;

    portal_count = 0;
    for (i = 0; i < grid_w * grid_h; i++)
    {
        portal_first[i] = -1;
    }
    for (i = 0; i < numlines; i++)
    {
        line_t *ld = &lines[i];
        fixed_t tx, ty, dx, dy, ox, oy;
        int gx, gy, dest, steps, k;
        fixed_t len;

        if (ld->special != 39 && ld->special != 97)
        {
            continue;
        }
        lines_seen++;
        if (!teleport_target(ld->tag, &tx, &ty)
            || !cell_of(tx, ty, &gx, &gy))
        {
            continue;
        }
        lines_resolved++;
        dest = gy * grid_w + gx;
        dx = ld->v2->x - ld->v1->x;
        dy = ld->v2->y - ld->v1->y;
        len = P_AproxDistance(dx, dy);
        if (len <= 0)
        {
            continue;
        }
        steps = len / FRACUNIT / (ROUTE_CELL / 2) + 1;
        /* (dy, -dx) normalised and taken out a body's width, which puts the
         * sample clear of the line rather than on it. Which side of the two
         * is the FRONT is whichever the engine's own test calls 0 - the only
         * side a teleport fires from, which is what lets a player walk off a
         * pad they have just arrived on. */
        ox = FixedMul(FixedDiv(dy, len), PORTAL_STANDOFF * FRACUNIT);
        oy = FixedMul(FixedDiv(-dx, len), PORTAL_STANDOFF * FRACUNIT);
        if (P_PointOnLineSide(ld->v1->x + dx / 2 + ox,
                              ld->v1->y + dy / 2 + oy, ld) != 0)
        {
            ox = -ox;
            oy = -oy;
        }
        /* Along the line: the cells a player is standing in when their next
         * step takes them across it. */
        for (k = 0; k <= steps; k++)
        {
            fixed_t px = ld->v1->x + FixedMul(dx, (k << FRACBITS) / steps) + ox;
            fixed_t py = ld->v1->y + FixedMul(dy, (k << FRACBITS) / steps) + oy;
            int fx, fy;

            if (cell_of(px, py, &fx, &fy) && walk[fy * grid_w + fx])
            {
                add_portal(fy * grid_w + fx, dest);
            }
        }
    }
    if (lines_seen > 0)
    {
        printf("API_Route: %d teleport lines, %d with a destination, %d jumps\n",
               lines_seen, lines_resolved, portal_count);
    }
}

static void build_edges(mobj_t *probe)
{
    dropped_locked = 0;

    int k, d;

    kept_straight = kept_diagonal = dropped_wall = dropped_body = 0;
    memset(edges, 0, grid_w * grid_h);

    for (k = 0; k < grid_w * grid_h; k++)
    {
        int ax = k % grid_w, ay = k / grid_w;

        if (!walk[k])
        {
            continue;
        }
        /* +x and +y only: -x and -y are the same pairs seen from the other
         * end, and crossing is symmetric. */
        for (d = 0; d < 4; d += 2)
        {
            int nx = ax + ROUTE_DX[d];
            int ny = ay + ROUTE_DY[d];
            int ni;

            if (nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (!walk[ni])
            {
                continue;
            }
            if (!can_cross(cell_x(ax), cell_y(ay), cell_x(nx), cell_y(ny)))
            {
                dropped_wall++;
                if (cross_lock != 0)
                {
                    locked_at[k] |= 1 << cross_lock;
                    locked_at[ni] |= 1 << cross_lock;
                }
                continue;
            }
            if (!body_fits(probe, (cell_x(ax) + cell_x(nx)) / 2,
                                  (cell_y(ay) + cell_y(ny)) / 2, d))
            {
                dropped_body++;
                continue;
            }
            edges[k] |= 1 << d;
            edges[ni] |= 1 << ROUTE_OPPOSITE[d];
            kept_straight++;
        }
    }

    for (k = 0; k < grid_w * grid_h; k++)
    {
        int ax = k % grid_w, ay = k / grid_w;

        if (!walk[k])
        {
            continue;
        }
        /* Two of the four, mirrored, for the same reason as the straights,
         * and here it is not just an economy - see the symmetry check at the
         * end of the build for what asking twice cost. */
        for (d = 4; d < 6; d++)
        {
            int nx = ax + ROUTE_DX[d];
            int ny = ay + ROUTE_DY[d];
            int across = ROUTE_DX[d] > 0 ? 0 : 1;
            int along = ROUTE_DY[d] > 0 ? 2 : 3;
            int ni;

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (!walk[ni])
            {
                continue;
            }
            /* Either one of the two ways round the corner is open the whole
             * way, or the corner point itself is floor a body fits on.
             *
             * The L-path test alone refuses a corridor that RUNS diagonally:
             * such a corridor has one of its two legs in a wall at every step
             * along it, so neither way round is ever open, and on E1M2 two
             * such steps were the only thing joining the level's two halves.
             *
             * The corner point alone is what let E1M1's route cut south-west
             * across a corner whose southward step is a wall. It does not,
             * once it means what it says: at a corner where two walls meet, a
             * 32-unit body centred on that point overlaps both of them, and
             * `walkable` says so. In a diagonal corridor wide enough to walk,
             * the same point is open floor. Deliberately not `body_fits` here
             * - its sideways search would find the open floor to one side of
             * the wall corner and call that passable. */
            if (!((edges[k] & (1 << across))
                  && (edges[ay * grid_w + nx] & (1 << along)))
                && !((edges[k] & (1 << along))
                     && (edges[ny * grid_w + ax] & (1 << across)))
                && !walkable(probe, (cell_x(ax) + cell_x(nx)) / 2,
                                    (cell_y(ay) + cell_y(ny)) / 2))
            {
                continue;
            }
            if (!can_cross(cell_x(ax), cell_y(ay), cell_x(nx), cell_y(ny)))
            {
                dropped_wall++;
                if (cross_lock != 0)
                {
                    locked_at[k] |= 1 << cross_lock;
                    locked_at[ni] |= 1 << cross_lock;
                }
                continue;
            }
            if (!body_fits(probe, (cell_x(ax) + cell_x(nx)) / 2,
                                  (cell_y(ay) + cell_y(ny)) / 2, d))
            {
                dropped_body++;
                continue;
            }
            edges[k] |= 1 << d;
            edges[ni] |= 1 << ROUTE_OPPOSITE[d];
            kept_diagonal++;
        }
    }
}

/* What the lines crossed between two cells are FOR.
 *
 * Enough of DOOM's line-special table to name the things that separate one
 * part of a level from another. A locked door and a lift are both "shut" to a
 * distance field over geometry, and they need completely different answers
 * from an agent, so a diagnostic that lumps them together is no use. */
static const char *special_name(int special)
{
    switch (special)
    {
        case 0:   return NULL;
        case 26: case 32: case 99: case 133:  return "BLUE-KEY door";
        case 28: case 33: case 134: case 135: return "RED-KEY door";
        case 27: case 34: case 136: case 137: return "YELLOW-KEY door";
        case 1: case 31: case 117: case 118:  return "door";
        case 29: case 50: case 103: case 111: case 112: case 113:
        case 42: case 61: case 63: case 114: case 115: case 116:
            return "door on a switch or a trigger";
        case 62: case 88: case 120: case 121: case 122: case 123:
            return "lift";
        case 10: case 21: case 87: case 53:
            return "lift on a trigger";
        case 39: case 97: case 125: case 126:
            return "teleporter";
        default: return "some other special";
    }
}

static int boundary_specials[8];
static int boundary_count;

static boolean PTR_BoundaryTraverse(intercept_t *in)
{
    line_t *ld = in->d.line;
    int i;

    if (ld->special == 0)
    {
        return true;
    }
    for (i = 0; i < boundary_count; i++)
    {
        if (boundary_specials[i] == ld->special)
        {
            return true;
        }
    }
    if (boundary_count < 8)
    {
        boundary_specials[boundary_count++] = ld->special;
    }
    return true;
}

/* Which of the level's own doors, lifts and switch-operated floors have the
 * player on one side and the exit on the other.
 *
 * This is the question that matters and it is not the same as "what is in the
 * shortest gap": the shortest gap between two components is usually a thin
 * wall the level never intended anyone to cross. Every line the author gave a
 * special to is a thing the player can OPERATE, so a special with the two
 * components either side of it is the way through, by construction. */
static void report_joining_lines(void)
{
    int i, found = 0;

    for (i = 0; i < numlines; i++)
    {
        line_t *ld = &lines[i];
        fixed_t mx, my, dx, dy, len;
        int cx, cy, sides[2] = { 0, 0 }, j;

        if (ld->special == 0 || ld->backsector == NULL || ld->frontsector == NULL)
        {
            continue;
        }
        mx = (ld->v1->x + ld->v2->x) / 2;
        my = (ld->v1->y + ld->v2->y) / 2;
        dx = ld->v2->x - ld->v1->x;
        dy = ld->v2->y - ld->v1->y;
        len = P_AproxDistance(dx, dy);
        if (len == 0)
        {
            continue;
        }
        /* A point a player's width off each face of the line. */
        dx = FixedDiv(dx, len);
        dy = FixedDiv(dy, len);
        for (j = 0; j < 2; j++)
        {
            int step;

            /* Walk out from the line until a cell belongs to something. The
             * first sample often lands inside the door's own sector, which is
             * a component of its own and answers nothing. */
            for (step = 40; step <= 160 && sides[j] == 0; step += 24)
            {
                fixed_t off = (j == 0 ? step : -step) * FRACUNIT;
                fixed_t px = mx - FixedMul(dy, off);
                fixed_t py = my + FixedMul(dx, off);

                if (!cell_of(px, py, &cx, &cy))
                {
                    continue;
                }
                if (reachable[cy * grid_w + cx] != ROUTE_UNREACHED)
                {
                    sides[j] = 1;
                }
                else if (field[cy * grid_w + cx] != ROUTE_UNREACHED)
                {
                    sides[j] = 2;
                }
            }
        }
        if ((sides[0] == 1 && sides[1] == 2) || (sides[0] == 2 && sides[1] == 1))
        {
            const char *name = special_name(ld->special);

            printf("API_Route:   line %d at %d,%d - special %d, %s - has the player on one "
                   "side and the exit on the other\n", i, mx >> FRACBITS, my >> FRACBITS,
                   ld->special, name ? name : "nothing");
            found++;
        }
    }
    if (found == 0)
    {
        printf("API_Route:   no door, lift or switch in the level has the two sides either "
               "side of it\n");
    }
}

/* Why the player's own component stops where it does.
 *
 * Every cell the player can reach that has a walkable neighbour it cannot step
 * to, grouped by which test refused the step. A level coming out in pieces is
 * either the level's doing - a locked door, a lift - or this file's, and these
 * counts are the difference. */
static void report_frontier(mobj_t *probe)
{
    int k, d;
    int by_wall = 0, by_body = 0, by_corner = 0;

    for (k = 0; k < grid_w * grid_h; k++)
    {
        int ax = k % grid_w, ay = k / grid_w;

        if (reachable[k] == ROUTE_UNREACHED)
        {
            continue;
        }
        for (d = 0; d < 8; d++)
        {
            int nx = ax + ROUTE_DX[d];
            int ny = ay + ROUTE_DY[d];
            int ni;

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            /* Only steps to somewhere the player fits and has not reached. */
            if (!walk[ni] || reachable[ni] != ROUTE_UNREACHED || (edges[k] & (1 << d)))
            {
                continue;
            }
            if (d >= 4 && (!(edges[k] & (1 << (ROUTE_DX[d] > 0 ? 0 : 1)))
                           || !(edges[k] & (1 << (ROUTE_DY[d] > 0 ? 2 : 3)))))
            {
                by_corner++;
            }
            else if (!can_cross(cell_x(ax), cell_y(ay), cell_x(nx), cell_y(ny)))
            {
                by_wall++;
            }
            else if (!body_fits(probe, (cell_x(ax) + cell_x(nx)) / 2,
                                       (cell_y(ay) + cell_y(ny)) / 2, d))
            {
                by_body++;
            }
        }
    }
    printf("API_Route: at the edge of what the player can reach, %d steps refused by a "
           "wall, %d for no room for a body, %d as a corner cut\n",
           by_wall, by_body, by_corner);
}

/* Can the two sides be joined by walking over cells a player FITS in, if the
 * step tests are set aside?
 *
 * If yes, the separation is this file's crossing tests and the place they gave
 * up is worth printing. If no, the cells in between are ones a player cannot
 * stand in at all, and the answer is in the level - a floor that has not been
 * lowered yet, a lift still up. Those are the two possibilities and nothing
 * else, so this narrows it in one pass. */
static void report_fit_path(int *queue)
{
    int *from = malloc(sizeof(int) * grid_w * grid_h);
    int head = 0, tail = 0, k, hit = -1;

    if (from == NULL)
    {
        return;
    }
    for (k = 0; k < grid_w * grid_h; k++)
    {
        from[k] = -2;
        if (field[k] != ROUTE_UNREACHED)
        {
            from[k] = -1;
            queue[tail++] = k;
        }
    }
    while (head < tail && hit < 0)
    {
        int at = queue[head++];
        int ax = at % grid_w, ay = at / grid_w, d;

        for (d = 0; d < 8; d++)
        {
            int nx = ax + ROUTE_DX[d];
            int ny = ay + ROUTE_DY[d];
            int ni;

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (from[ni] != -2 || !walk[ni])
            {
                continue;
            }
            from[ni] = at;
            if (reachable[ni] != ROUTE_UNREACHED)
            {
                hit = ni;
                break;
            }
            queue[tail++] = ni;
        }
    }
    if (hit < 0)
    {
        printf("API_Route: and no chain of cells a player FITS in joins them either - so "
               "what is in between is not standable ground yet, which is the level's "
               "doing and not this file's\n");
        free(from);
        return;
    }
    printf("API_Route: but a chain of cells the player FITS in DOES join them, so the "
           "separation is a crossing test here. It gives up at:\n");
    {
        int at = hit, shown = 0, chain = 0, missing = 0;

        while (from[at] >= 0)
        {
            chain++;
            at = from[at];
        }
        at = hit;
        printf("API_Route:   the chain is %d cells long, and the steps it needs that "
               "are missing are:\n", chain);
        while (from[at] >= 0 && shown < 4)
        {
            int prev = from[at];
            int ax = at % grid_w, ay = at / grid_w;
            int px = prev % grid_w, py = prev / grid_w;
            int d;

            for (d = 0; d < 8; d++)
            {
                if (ax + ROUTE_DX[d] == px && ay + ROUTE_DY[d] == py)
                {
                    break;
                }
            }
            if (d < 8 && !(edges[at] & (1 << d)))
            {
                subsector_t *a = R_PointInSubsector(cell_x(ax), cell_y(ay));
                subsector_t *b = R_PointInSubsector(cell_x(px), cell_y(py));

                boolean walled =
                    !can_cross(cell_x(ax), cell_y(ay), cell_x(px), cell_y(py));
                char what[176];

                if (!walled || cross_blocker == NULL)
                {
                    M_StringCopy(what, walled ? "a wall in the way"
                                              : "no room for a body, or a corner cut",
                                 sizeof(what));
                }
                else
                {
                    line_t *bl = cross_blocker;

                    P_LineOpening(bl);
                    M_snprintf(what, sizeof(what),
                               "line %d, special %d, tag %d%s%s, opening %d, lock %d; "
                               "back sector %d tag %d floor %d ceiling %d",
                               (int)(bl - lines), bl->special, bl->tag,
                               bl->backsector == NULL ? ", one-sided" : "",
                               (bl->flags & ML_BLOCKING) ? ", flagged blocking" : "",
                               openrange >> FRACBITS, cross_lock,
                               bl->backsector == NULL ? -1
                                   : (int)(bl->backsector - sectors),
                               bl->backsector == NULL ? 0 : bl->backsector->tag,
                               bl->backsector == NULL ? 0
                                   : bl->backsector->floorheight >> FRACBITS,
                               bl->backsector == NULL ? 0
                                   : bl->backsector->ceilingheight >> FRACBITS);
                }
                printf("API_Route:   %d,%d (floor %d) -> %d,%d (floor %d): %s\n",
                       cell_x(ax) >> FRACBITS, cell_y(ay) >> FRACBITS,
                       a->sector->floorheight >> FRACBITS,
                       cell_x(px) >> FRACBITS, cell_y(py) >> FRACBITS,
                       b->sector->floorheight >> FRACBITS,
                       what);
                shown++;
                missing++;
            }
            at = prev;
        }
        if (missing == 0)
        {
            printf("API_Route:   ...but every step along it has one, which should not be "
                   "possible for two separate components\n");
        }
    }
    free(from);
}

/* The shortest way from the exit's side of the level to the player's side,
 * ignoring whether a player could walk it, and what stands along it.
 *
 * The two components usually do not touch: whatever separates them is itself
 * several cells thick, so asking only about cells that are neighbours finds
 * nothing. This crosses the gap regardless and then reports the line specials
 * on the way, which is the thing worth knowing - a locked door, a lift and a
 * floor a switch raises are all "shut" to a distance field over geometry, and
 * they need completely different answers from an agent.
 */
static void report_boundary(mobj_t *probe, int *queue)
{
    int *from = malloc(sizeof(int) * grid_w * grid_h);
    int head = 0, tail = 0, k, hit = -1;

    if (from == NULL)
    {
        return;
    }
    for (k = 0; k < grid_w * grid_h; k++)
    {
        from[k] = -2;
        if (field[k] != ROUTE_UNREACHED)
        {
            from[k] = -1;
            queue[tail++] = k;
        }
    }
    while (head < tail && hit < 0)
    {
        int at = queue[head++];
        int ax = at % grid_w, ay = at / grid_w, d;

        for (d = 0; d < 4; d++)
        {
            int nx = ax + ROUTE_DX[d];
            int ny = ay + ROUTE_DY[d];
            int ni;

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (from[ni] != -2)
            {
                continue;
            }
            from[ni] = at;
            if (reachable[ni] != ROUTE_UNREACHED)
            {
                hit = ni;
                break;
            }
            queue[tail++] = ni;
        }
    }
    if (hit < 0)
    {
        printf("API_Route: nothing connects the two sides at all, not even through walls\n");
        free(from);
        return;
    }

    boundary_count = 0;
    {
        int at = hit, steps = 0;

        while (from[at] >= 0)
        {
            int prev = from[at];

            P_PathTraverse(cell_x(at % grid_w), cell_y(at / grid_w),
                           cell_x(prev % grid_w), cell_y(prev / grid_w),
                           PT_ADDLINES, PTR_BoundaryTraverse);
            at = prev;
            steps++;
        }
        printf("API_Route: the nearest way across is %d cells (%d units) of ground the "
               "player cannot walk, from the player's side at %d,%d to the exit's at "
               "%d,%d, and the lines along it are:\n", steps, steps * ROUTE_CELL,
               cell_x(hit % grid_w) >> FRACBITS, cell_y(hit / grid_w) >> FRACBITS,
               cell_x(at % grid_w) >> FRACBITS, cell_y(at / grid_w) >> FRACBITS);
    }
    if (boundary_count == 0)
    {
        printf("API_Route:   no line special at all - which means this gap is a wall the "
               "level never meant anyone to cross, not the way through\n");
    }
    for (k = 0; k < boundary_count; k++)
    {
        const char *name = special_name(boundary_specials[k]);

        printf("API_Route:   line special %d - %s\n", boundary_specials[k],
               name ? name : "nothing");
    }
    free(from);
    printf("API_Route: the operable lines between the two sides:\n");
    report_joining_lines();
    report_frontier(probe);
    report_fit_path(queue);
}

/* Whether opening this sector would join the player's side of the level to
 * the exit's.
 *
 * A level is full of shut sectors some switch opens - every lift, every
 * switch-door, every raising floor - and all but one of them is irrelevant to
 * getting out. Taking the first one found sent E1M2's player to a switch
 * fourteen hundred units the wrong way, which it duly pressed, after which the
 * route wanted a different switch four thousand units further off.
 *
 * The one that matters is the one with the player on one side and the exit on
 * the other, and that is answerable without opening anything: sample just off
 * each of the sector's own lines and see which of the two islands the cell
 * there belongs to. Off the LINES rather than inside the sector, because these
 * things are often a few units thick - the diagonal slab this was written for
 * is eleven - and no cell centre lands inside them at all. */
static boolean sector_joins(sector_t *sec, const unsigned short *island)
{
    boolean touches_player = false, touches_exit = false;
    int i;

    for (i = 0; i < numlines && !(touches_player && touches_exit); i++)
    {
        line_t *ld = &lines[i];
        fixed_t dx, dy, len, mx, my;
        int side;

        if (ld->frontsector != sec && ld->backsector != sec)
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
        mx = (ld->v1->x + ld->v2->x) / 2;
        my = (ld->v1->y + ld->v2->y) / 2;
        for (side = 0; side < 2; side++)
        {
            fixed_t sign = side == 0 ? FRACUNIT : -FRACUNIT;
            fixed_t nx = FixedDiv(FixedMul(dy, sign), len);
            fixed_t ny = FixedDiv(FixedMul(-dx, sign), len);
            fixed_t px = mx + FixedMul(40 * FRACUNIT, nx);
            fixed_t py = my + FixedMul(40 * FRACUNIT, ny);
            int cx, cy, ci;

            if (!cell_of(px, py, &cx, &cy))
            {
                continue;
            }
            ci = cy * grid_w + cx;
            touches_player |= reachable[ci] != ROUTE_UNREACHED;
            touches_exit |= island[ci] != ROUTE_UNREACHED;
        }
    }
    return touches_player && touches_exit;
}

/* A walkable place to stand in front of a switch line, on either side.
 *
 * Not the line's own midpoint, which is inside the wall the switch is mounted
 * on - the same reason the exit's spots are offsets rather than the exit line
 * itself. */
static boolean opener_spot(mobj_t *probe, line_t *ld, fixed_t *ox, fixed_t *oy)
{
    static const int offsets[] = { 32, 48, 64, 96 };
    fixed_t dx = ld->v2->x - ld->v1->x;
    fixed_t dy = ld->v2->y - ld->v1->y;
    fixed_t len = P_AproxDistance(dx, dy);
    fixed_t mx = (ld->v1->x + ld->v2->x) / 2;
    fixed_t my = (ld->v1->y + ld->v2->y) / 2;
    unsigned int oi;
    int side;

    if (len <= 0)
    {
        return false;
    }
    for (oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]); oi++)
    {
        for (side = 0; side < 2; side++)
        {
            fixed_t sign = side == 0 ? FRACUNIT : -FRACUNIT;
            fixed_t nx = FixedDiv(FixedMul(dy, sign), len);
            fixed_t ny = FixedDiv(FixedMul(-dx, sign), len);
            fixed_t px = mx + FixedMul(offsets[oi] * FRACUNIT, nx);
            fixed_t py = my + FixedMul(offsets[oi] * FRACUNIT, ny);

            if (walkable(probe, px, py))
            {
                *ox = px;
                *oy = py;
                return true;
            }
        }
    }
    return false;
}

/* Choose what the route leads to and flood the field from it.
 *
 * Split from building the grid because the two change on completely different
 * clocks. The grid is geometry and changes when the level does; WHERE the
 * route leads changes every time the player sees another room, which under
 * fair play is every few decisions. Flooding is a pass over an array and the
 * grid is thousands of engine queries, so re-asking the cheap question
 * cheaply is the difference between an honest route and an unaffordable one.
 */
static boolean seed_and_flood(mobj_t *probe, boolean allow_keys, int *queue,
                              api_point_t *spots, int n_spots, int build_ms,
                              boolean *no_seed)
{
    int i, cx, cy, seed;
    unsigned short *reach;

    route_goal_key = 0;
    route_goal_switch = false;
    route_goal_frontier = false;
    frontier_step = -1;
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
    /* Asked without the no-damage rule, whatever rule the rest of the build
     * is using. The player IS standing here, so it is by definition somewhere
     * they can be - and a player who has stepped into nukage is standing on
     * floor that `walkable` refuses while the field is trying to stay dry.
     * That read as "not on the grid", the build failed, the next attempt
     * allowed damage and succeeded, and E1M1 then routed every remaining
     * episode straight through its nukage room. */
    {
        boolean dry = allow_damage;
        boolean on_grid;

        allow_damage = true;
        on_grid = cell_of(probe->x, probe->y, &cx, &cy)
                  && walkable(probe, probe->x, probe->y);
        allow_damage = dry;
        if (!on_grid)
        {
            printf("API_Route: the player is not on the grid\n");
            free(queue);
            free(field);
            field = NULL;
            return false;
        }
    }    reach = malloc(sizeof(unsigned short) * grid_w * grid_h);
    if (reach == NULL)
    {
        free(queue);
        free(field);
        field = NULL;
        return false;
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
    /* WHICH keys are in the way, read off the edge of what the player can
     * reach now that the flood has said what that is.
     *
     * This was a running total, set while the steps were tested and then
     * zeroed at the top of this function - one call later, before anything
     * read it. So the branch below that heads for a key never fired on any
     * level, and an exit behind a locked door produced no route at all rather
     * than a route to the key that opens it. */
    keys_wanted = 0;
    if (locked_at != NULL)
    {
        int c;

        for (c = 0; c < grid_w * grid_h; c++)
        {
            if (reachable[c] != ROUTE_UNREACHED)
            {
                keys_wanted |= locked_at[c];
            }
        }
    }
    keys_wanted &= ~held_keys;
    /* The exit is unreachable and a locked door is why: head for its key
     * instead. That is what a player does, and it needs no new machinery -
     * the same field, flooded from a different place. Picking the key up
     * changes which doors are walls, the grid is rebuilt on that, and the
     * route goes back to pointing at the exit on its own. */
    if (seed < 0 && allow_keys && keys_wanted != 0)
    {
        int colour;

        for (colour = API_KEY_BLUE; colour <= API_KEY_RED; colour++)
        {
            mobj_t *key;
            int kx, ky;

            if (!(keys_wanted & (1 << colour)))
            {
                continue;
            }
            key = find_key(colour);
            if (key == NULL || !cell_of(key->x, key->y, &kx, &ky)
                || reachable[ky * grid_w + kx] == ROUTE_UNREACHED)
            {
                continue;
            }
            seed = ky * grid_w + kx;
            route_goal_key = colour;
            break;
        }
    }
    /* Still nothing, and a shut sector some switch opens is why: head for the
     * switch. The same move as the key, for the same reason - it is the
     * level's own next objective - and the same machinery: one field, flooded
     * from somewhere else. E1M2's way on from the red key is exactly this,
     * and being told only "a wall" had a follower shoving at it for six
     * hundred decisions with the switch a hundred units to its left. */
    if (seed < 0 && allow_keys && wanted_count > 0)
    {
        unsigned short *island = malloc(sizeof(unsigned short) * grid_w * grid_h);
        int w, exit_cell = -1;

        /* The exit's own side of the level, to tell the one switch that leads
         * out from the dozen that do not. */
        for (i = 0; i < n_spots && exit_cell < 0; i++)
        {
            int sx, sy;

            if (cell_of(spots[i].x, spots[i].y, &sx, &sy) && walk[sy * grid_w + sx])
            {
                exit_cell = sy * grid_w + sx;
            }
        }
        if (island != NULL && exit_cell >= 0)
        {
            flood(island, exit_cell, queue);
        }
        for (w = 0; w < wanted_count && seed < 0; w++)
        {
            int j;

            if (island == NULL || exit_cell < 0
                || !sector_joins(shut_sector_of(wanted_lines[w]), island))
            {
                continue;
            }
            for (j = 0; j < numlines; j++)
            {
                line_t *ld = &lines[j];
                fixed_t sx, sy;
                int scx, scy;

                if (ld->special == 0
                    || ld->tag != shut_sector_of(wanted_lines[w])->tag)
                {
                    continue;
                }
                if (!opener_spot(probe, ld, &sx, &sy) || !cell_of(sx, sy, &scx, &scy)
                    || reachable[scy * grid_w + scx] == ROUTE_UNREACHED)
                {
                    continue;
                }
                seed = scy * grid_w + scx;
                route_goal_switch = true;
                route_switch_x = (ld->v1->x + ld->v2->x) / 2;
                route_switch_y = (ld->v1->y + ld->v2->y) / 2;
                break;
            }
        }
        free(island);
    }

    /* Nowhere known to head for: go and look.
     *
     * The frontier is the edge of what the player has seen - a cell they can
     * walk to with a neighbour they have not laid eyes on - and walking to it
     * is the whole of exploring. This is what replaces being handed the exit
     * on a level nobody has walked: the route answers "where is there still
     * something to find", which is a question a player can answer too, and
     * the exit becomes the goal the moment it has been seen.
     *
     * The nearest one, measured over ground the player has seen rather than
     * as the crow flies, because the crow flies through walls. */
    if (seed < 0 && fair_play)
    {
        int best = -1, best_d = 0, k;

        for (k = 0; k < grid_w * grid_h; k++)
        {
            int d;

            if (reachable[k] == ROUTE_UNREACHED)
            {
                continue;
            }
            for (d = 0; d < 8; d++)
            {
                int nx = k % grid_w + ROUTE_DX[d];
                int ny = k / grid_w + ROUTE_DY[d];
                int ni;

                if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
                {
                    continue;
                }
                ni = ny * grid_w + nx;
                /* A STEP into somewhere unseen, not merely a neighbour that
                 * happens to be unseen. Without the edge test the cell on the
                 * far side of a thin wall qualifies for ever: it is walkable,
                 * it is unseen, and no amount of standing next to it changes
                 * either - which is a player facing a wall waiting for it to
                 * become interesting. */
                if (!(edges[k] & (1 << d)) || (seen != NULL && seen[ni]))
                {
                    continue;
                }
                if (best < 0 || reachable[k] < best_d)
                {
                    best = k;
                    best_d = reachable[k];
                    frontier_step = ni;
                }
                break;
            }
        }
        if (best >= 0)
        {
            seed = best;
            route_goal_frontier = true;
        }
    }

    if (seed < 0)
    {
        int walkable_cells = 0, reached = 0;

        for (i = 0; i < grid_w * grid_h; i++)
        {
            walkable_cells += walk[i] != 0;
            reached += reachable[i] != ROUTE_UNREACHED;
        }
        /* A handful of cells is not a level, it is the tic one loaded on.
         * Nothing can be concluded from it - and what was concluded was "no
         * dry way to the exit", after which the route waded for the whole of
         * the rest of the run and the player died in the nukage it waded
         * into. Under fair play the reachable region starts at the size of
         * one room and grows, so this says "ask again", not "give up". */
        if (reached < 8)
        {
            printf("API_Route: the level is not ready to be mapped yet\n");
            *no_seed = false;
            free(queue);
            free(field);
            field = NULL;
            return false;
        }
        printf("API_Route: none of the %d standing spots at the exit is in the %d cells "
               "the player can reach, of %d walkable%s\n",
               n_spots, reached, walkable_cells,
               allow_damage ? " (damaging floor allowed)" : "");
        /* How big the exit's OWN island is, and what walls it off. A handful
         * of cells means the spot search put the exit in a sealed pocket and
         * the bug is here. A whole region means the level is in two pieces as
         * far as walking is concerned, and the specials on the lines between
         * them say why - a locked door, a lift, a floor some switch raises.
         * None of those is geometry, so no distance field over geometry finds
         * the way across on its own. */
        for (i = 0; i < n_spots; i++)
        {
            int sx, sy;

            if (cell_of(spots[i].x, spots[i].y, &sx, &sy) && walk[sy * grid_w + sx])
            {
                int got = flood(field, sy * grid_w + sx, queue);

                printf("API_Route: the exit's own side of the level is %d cells\n", got);
                report_boundary(probe, queue);
                break;
            }
        }
        for (i = 0; i < n_spots; i++)
        {
            int sx, sy;

            if (!cell_of(spots[i].x, spots[i].y, &sx, &sy))
            {
                printf("API_Route:   spot %d at %d,%d is off the grid\n", i,
                       spots[i].x >> FRACBITS, spots[i].y >> FRACBITS);
                continue;
            }
            printf("API_Route:   spot %d at %d,%d is %s\n", i,
                   spots[i].x >> FRACBITS, spots[i].y >> FRACBITS,
                   !walk[sy * grid_w + sx] ? "not somewhere the player fits"
                                           : "walled off from the player");
        }
        *no_seed = true;
        free(queue);
        free(field);
        field = NULL;
        return false;
    }
    {
        /* Sequenced deliberately: C does not order function arguments, and
         * reading the field in the same printf that fills it reported the
         * spawn as unreachable on a field that had not been built yet. */
        int got = flood(field, seed, queue);
        int at_spawn = field[cy * grid_w + cx];

        static const char *colour_name[] = { "", "blue", "yellow", "red" };

        if (build_ms > 0)
        {
        printf("API_Route: %dx%d cells at %d units, %d reachable, %s %d cells from the "
               "spawn%s, %d ms to map the steps between them\n",
               grid_w, grid_h, ROUTE_CELL, got,
               route_goal_frontier ? "the frontier"
                   : route_goal_switch ? "a switch"
                   : route_goal_key ? colour_name[route_goal_key] : "exit",
               at_spawn == ROUTE_UNREACHED ? -1 : at_spawn,
               allow_damage ? " (through damaging floor)" : "", build_ms);
        }
    }

    free(queue);
    return true;
}

/* Build the field once, with `allow_damage` as it stands. False when there is
 * no field to be had; *no_seed says the reason was that no standing spot at
 * the exit could be reached, which is the one failure worth retrying. */
static boolean build_field(mobj_t *probe, boolean allow_keys, boolean *no_seed)
{
    fixed_t minx = 0, miny = 0, maxx = 0, maxy = 0;
    api_point_t spots[API_MAX_EXIT_SPOTS];
    int n_spots;
    int i, cx, cy, seed;
    int build_ms;
    int *queue;
    unsigned short *reach;

    *no_seed = false;
    free(field);
    field = NULL;
    held_keys = keys_of(probe);
    route_goal_key = 0;
    route_goal_switch = false;
    route_goal_frontier = false;
    frontier_step = -1;
    wanted_count = 0;

    n_spots = API_ExitSpots(probe, spots, API_MAX_EXIT_SPOTS);
    if (numvertexes <= 0 || n_spots == 0)
    {
        printf("API_Route: no exit line, or nowhere to stand at it\n");
        return false;
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
        return false;
    }

    field = malloc(sizeof(unsigned short) * grid_w * grid_h);
    queue = malloc(sizeof(int) * grid_w * grid_h);
    if (field == NULL || queue == NULL)
    {
        printf("API_Route: out of memory for a %dx%d grid\n", grid_w, grid_h);
        free(queue);
        free(field);
        field = NULL;
        return false;
    }
    free(walk);
    free(edges);
    free(visited);
    free(seen);
    free(cell_sector);
    free(portal_first);
    free(portal_to);
    free(portal_next);
    free(locked_at);
    walk = calloc(1, grid_w * grid_h);
    edges = calloc(1, grid_w * grid_h);
    locked_at = calloc(1, grid_w * grid_h);
    visited = calloc(1, grid_w * grid_h);
    seen = calloc(1, grid_w * grid_h);
    cell_sector = malloc(sizeof(int) * grid_w * grid_h);
    portal_first = malloc(sizeof(int) * grid_w * grid_h);
    portal_to = malloc(sizeof(int) * grid_w * grid_h);
    portal_next = malloc(sizeof(int) * grid_w * grid_h);
    portal_count = 0;
    if (walk == NULL || edges == NULL || visited == NULL || seen == NULL
        || cell_sector == NULL || portal_first == NULL || portal_to == NULL
        || portal_next == NULL || locked_at == NULL)
    {
        printf("API_Route: out of memory for a %dx%d grid\n", grid_w, grid_h);
        free(queue);
        free(field);
        field = NULL;
        return false;
    }
    for (i = 0; i < grid_w * grid_h; i++)
    {
        subsector_t *ss;

        field[i] = ROUTE_UNREACHED;
        walk[i] = walkable(probe, cell_x(i % grid_w), cell_y(i / grid_w)) ? 1 : 0;
        /* Which sector each cell is in, looked up once. What the player has
         * SEEN changes every few decisions and is decided from this. */
        ss = R_PointInSubsector(cell_x(i % grid_w), cell_y(i / grid_w));
        cell_sector[i] = ss != NULL && ss->sector != NULL ? ss->sector - sectors : -1;
    }
    /* The player is standing here, so this cell is one they can be in -
     * whatever the test at its CENTRE says. A centre can fall inside a wall
     * the player is standing beside, or in nukage while the route is trying
     * to stay dry, and then the flood starts nowhere and reports that not one
     * of the level's cells is reachable. Measured on E1M3: nineteen builds in
     * four hundred decisions each reporting "0 cells the player can reach",
     * after which the route gave up on staying dry and waded until it died. */
    if (cell_of(probe->x, probe->y, &cx, &cy))
    {
        walk[cy * grid_w + cx] = 1;
    }
    mark_seen(probe);
    {
        int fx, fy;

        flooded_from = cell_of(probe->x, probe->y, &fx, &fy) ? fy * grid_w + fx : -1;
    }
    build_ms = I_GetTimeMS();
    build_edges(probe);
    build_portals();
    build_ms = I_GetTimeMS() - build_ms;
    /* Every step has to exist from both ends. A traverse between two points
     * is not bit-for-bit the same run in reverse, so asking the question from
     * each end and believing both answers produced 128 one-way steps on E1M1
     * - and a breadth-first field over a graph like that has local minima, a
     * cell one step further from the exit than its neighbour with no step to
     * it. The descent stops dead and the agent is told there is no route at
     * all. Both directions are written from one test now; this is what says
     * so, and it costs one pass over the grid. */
    {
        int bad = 0, k2, d2;

        for (k2 = 0; k2 < grid_w * grid_h; k2++)
        {
            for (d2 = 0; d2 < 8; d2++)
            {
                int nx2, ny2;

                if (!(edges[k2] & (1 << d2)))
                {
                    continue;
                }
                nx2 = k2 % grid_w + ROUTE_DX[d2];
                ny2 = k2 / grid_w + ROUTE_DY[d2];
                if (nx2 < 0 || ny2 < 0 || nx2 >= grid_w || ny2 >= grid_h
                    || !(edges[ny2 * grid_w + nx2] & (1 << ROUTE_OPPOSITE[d2])))
                {
                    bad++;
                }
            }
        }
        if (bad > 0)
        {
            printf("API_Route: %d steps exist one way and not the other - the field "
                   "will have dead ends in it\n", bad);
        }
    }
    printf("API_Route: %d straight steps and %d diagonal ones; %d dropped by a wall, "
           "%d for no room for a body, %d for want of a key\n",
           kept_straight, kept_diagonal, dropped_wall, dropped_body, dropped_locked);

    return seed_and_flood(probe, allow_keys, queue, spots, n_spots, build_ms, no_seed);

}

/* Rebuild when the level changed. Keyed on the `lines` array, which a level
 * reload reallocates - a stale field would route through a map that is gone. */
/* Re-decide where the route leads, because the player has seen more of the
 * level. Cheap on purpose - see `seed_and_flood`. */
static void reflood(mobj_t *probe)
{
    api_point_t spots[API_MAX_EXIT_SPOTS];
    int *queue;
    int n_spots;
    boolean no_seed;

    if (field == NULL || grid_w <= 0)
    {
        return;
    }
    queue = malloc(sizeof(int) * grid_w * grid_h);
    if (queue == NULL)
    {
        return;
    }
    n_spots = API_ExitSpots(probe, spots, API_MAX_EXIT_SPOTS);
    mark_seen(probe);
    seed_and_flood(probe, true, queue, spots, n_spots, 0, &no_seed);
    {
        int cx, cy;

        flooded_from = cell_of(probe->x, probe->y, &cx, &cy) ? cy * grid_w + cx : -1;
    }
}

/* Whether a shut sector the route was waiting on has since been opened.
 *
 * The grid describes the level as it was when it was built, and a switch is
 * the player CHANGING the level. Without this the field goes on routing to a
 * switch that has already been pressed, which is the same class of mistake as
 * routing to a key already picked up - and that one is caught because holding
 * the key is part of the build stamp. This is the rest of that stamp. */
static boolean wanted_sector_opened(void)
{
    int i;

    for (i = 0; i < wanted_count; i++)
    {
        /* The OPENING that was refused, not the sector's own height. A sector
         * can be head-high on its own and still have no gap at all against
         * the one next to it - a raised platform is exactly that - so asking
         * about the sector said "opened" from the moment it was recorded, and
         * the whole grid was rebuilt on every observation. Measured on E1M1:
         * 11 to 24 ms of engine time per decision, and nine of twelve
         * episodes stalled in the same spot. */
        P_LineOpening(wanted_lines[i]);
        if (openrange >= ROUTE_HEIGHT)
        {
            return true;
        }
    }
    return false;
}

static void ensure_built(mobj_t *probe)
{
    boolean no_seed;

    if (built_for != (void *)lines || built_count != numlines)
    {
        /* A new level. Everything remembered about the last one points into
         * memory that has been freed and reallocated - the wanted lines
         * especially - so it goes before anything reads it, and the dry route
         * is worth trying again. */
        allow_damage = false;
        wanted_count = 0;
        field_stale = true;
    }
    if (field != NULL && !field_stale && built_keys == keys_of(probe)
        && !wanted_sector_opened())
    {
        /* The grid is current. What the route LEADS TO may not be: the seen
         * set grows every time the player looks at a wall they have not
         * looked at before, and the frontier is chosen as the nearest one to
         * where the player is standing - so both inputs change as they walk.
         *
         * Re-deciding on either is what makes an observation a function of
         * the world rather than of when it was last asked for. Without the
         * position half, the same six tics answered differently depending on
         * whether they were requested in one call or six: the frontier had
         * been picked from wherever the player happened to be at the last
         * reflood. Measured on E1M1, two runs identical for 160 decisions and
         * then not - which is exactly the property a recording needs, since
         * recording is the thing that asks tic by tic.
         *
         * A flood is one pass over an array. The grid build it is split from
         * is thousands of engine queries. */
        if (fair_play) {
            int cx, cy;

            if (mapped_lines() != seen_from
                || (cell_of(probe->x, probe->y, &cx, &cy)
                    && cy * grid_w + cx != flooded_from))
            {
                reflood(probe);
            }
        }
        return;
    }
    field_stale = false;
    /* Picking up a key changes which doors are walls, so it changes the map.
     * Rebuilding on that is what turns "go and get the blue key" into "now go
     * to the exit" without anything else having to notice. */
    built_keys = keys_of(probe);
    built_for = (void *)lines;
    built_count = numlines;

    /* Four questions, in the order a player would ask them: can I walk to the
     * exit, can I if I am willing to wade, is there a key I should fetch
     * first, and the same again wading.
     *
     * The order is the whole of it and it used to be wrong: a key outranked
     * wading, so E1M3 - whose exit is over a bridge with slime either side -
     * picked up its blue key, found no dry way on, and set off for the YELLOW
     * one, which opens nothing it needs. Silently, because the "nowhere to
     * stand at the exit" report only prints when there is no route at all,
     * and a route to the wrong key is a route.
     *
     * The retry used to clear built_for and call back into ensure_built,
     * which saw a level it had not built and reset allow_damage on the way in
     * - so it asked the same question again, got the same answer, and
     * recursed until the engine stopped answering. E1M2, whose exit is across
     * nukage, hung on its first observation. Hence one flat loop. */
    {
        static const char *why[4] = {
            NULL,
            "no dry way to the exit; wading",
            "no way to the exit at all; fetching a key first",
            "no dry way to a key either; wading to one",
        };
        int try_;

        for (try_ = 0; try_ < 4; try_++)
        {
            allow_damage = (try_ & 1) != 0;
            if (build_field(probe, (try_ & 2) != 0, &no_seed))
            {
                if (why[try_] != NULL)
                {
                    printf("API_Route: %s\n", why[try_]);
                }
                return;
            }
            if (!no_seed)
            {
                /* It failed for a reason a different question does not
                 * answer - no level loaded, no exit line, the player nowhere
                 * on the grid. Asking again with the slime allowed gets the
                 * same answer, and TAKING it means routing through slime for
                 * the rest of the run on the strength of an unrelated
                 * failure, which is what E1M1 did from its fourth episode on.
                 */
                break;
            }
        }
    }
    printf("API_Route: nowhere reachable to stand at the exit\n");
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
     * walk to a point beyond it until it is open.
     *
     * The third test is the drop on the far side, and it is what makes a
     * WINDOW a window rather than a doorway: low enough to climb, open above,
     * and P_TryMove refuses anyway because the player would be standing over
     * a fall of more than 24 units. */
    if (openrange < 56 * FRACUNIT || openbottom - walk_probe_z > 24 * FRACUNIT
        || openbottom - lowfloor > 24 * FRACUNIT)
    {
        walk_blocked = true;
        walk_block_line = ld;
        walk_block_frac = in->frac;
        return false;
    }
    return true;
}

/* Whether the straight line between two points stays off damaging floor.
 *
 * The field routes around nukage when it can, cell by cell, and then the
 * follower is handed a waypoint four cells ahead and walks the STRAIGHT line
 * to it - which cuts the corner the route went round. Measured on E1M3, whose
 * route to the exit after the blue key is entirely dry: the player took a
 * hundred and four points of nukage damage following it and died at decision
 * 394. Sampling is enough, at half the player's width: a pool one sample wide
 * is not a pool. */
static boolean dry_line(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    fixed_t dx = x2 - x1, dy = y2 - y1;
    fixed_t len = P_AproxDistance(dx, dy);
    int steps, i, wet = 0;

    if (len <= 0)
    {
        return true;
    }
    steps = (len >> FRACBITS) / 16 + 1;
    for (i = 1; i <= steps; i++)
    {
        subsector_t *ss = R_PointInSubsector(x1 + dx * i / steps, y1 + dy * i / steps);

        if (ss != NULL && ss->sector != NULL && hurts(ss->sector->special))
        {
            wet++;
            /* A splash is not a pool. Sixty-four units of it is under a
             * second of walking and costs one tick of damage at worst, and
             * refusing every line that clips one is how a follower came to
             * stand two cells short of E1M2's exit for a thousand decisions:
             * the cell path round it exists, and no straight line to any
             * waypoint along it is completely dry. */
            if (wet > 4)
            {
                return false;
            }
        }
        else
        {
            wet = 0;
        }
    }
    return true;
}

static boolean can_walk_to(mobj_t *player, fixed_t x, fixed_t y)
{
    walk_blocked = false;
    walk_block_line = NULL;
    walk_probe_z = player->z;
    P_PathTraverse(player->x, player->y, x, y, PT_ADDLINES, PTR_WalkTraverse);
    if (walk_blocked)
    {
        return false;
    }
    /* And not through the slime, unless the route has already given up on
     * staying dry, or the player is standing in it - where every way out is
     * wet and refusing them all leaves the agent with no route at the one
     * moment it is losing health for want of one. */
    if (!allow_damage && !hurts(player->subsector->sector->special)
        && !dry_line(player->x, player->y, x, y))
    {
        return false;
    }
    return true;
}

/* The nearest line whose special operates this sector's tag: the switch that
 * opens it.
 *
 * A tag can be operated from several places and any of them will do, so the
 * nearest is the answer. This is the level's own wiring read back: the mapper
 * joined a switch to a sector by giving both the same number, and that number
 * is still there to be read. */
static line_t *opener_of(sector_t *sec, fixed_t x, fixed_t y)
{
    line_t *best = NULL;
    fixed_t best_d = 0;
    int i;

    if (sec == NULL || sec->tag == 0)
    {
        return NULL;
    }
    for (i = 0; i < numlines; i++)
    {
        line_t *ld = &lines[i];
        fixed_t mx, my, d;

        if (ld->special == 0 || ld->tag != sec->tag)
        {
            continue;
        }
        mx = (ld->v1->x + ld->v2->x) / 2;
        my = (ld->v1->y + ld->v2->y) / 2;
        d = P_AproxDistance(mx - x, my - y);
        if (best == NULL || d < best_d)
        {
            best = ld;
            best_d = d;
        }
    }
    return best;
}

/* Say where the switch is, when the thing in the way is not one the player can
 * open by pushing on it.
 *
 * E1M2's is the case this was written for. The way on from the red key is
 * through a sector of zero height that a switch a hundred units off to one
 * side raises; every walk test correctly refuses to cross it, the route
 * correctly runs through it, and the agent was told "a wall" - so it pushed at
 * the wall for six hundred decisions with the switch in plain view beside it. */
static void note_opener(mobj_t *player, line_t *shut, api_route_t *out)
{
    line_t *sw;

    if (shut == NULL || shut->special != 0)
    {
        /* It opens itself: pushing on it is the whole of the answer. */
        return;
    }
    sw = opener_of(shut->backsector, player->x, player->y);
    if (sw == NULL)
    {
        sw = opener_of(shut->frontsector, player->x, player->y);
    }
    if (sw == NULL)
    {
        return;
    }
    out->have_switch = true;
    out->switch_x = (sw->v1->x + sw->v2->x) / 2;
    out->switch_y = (sw->v1->y + sw->v2->y) / 2;
}

/* The solid thing found standing between the player and the next waypoint. */
static mobj_t *block_probe_self;
static fixed_t block_probe_x, block_probe_y;
static mobj_t *block_probe_hit;

static boolean PIT_ThingInTheWay(mobj_t *th)
{
    fixed_t reach;

    if (th == block_probe_self || !(th->flags & MF_SOLID))
    {
        return true;
    }
    reach = block_probe_self->radius + th->radius;
    if (abs(th->x - block_probe_x) >= reach || abs(th->y - block_probe_y) >= reach)
    {
        return true;
    }
    /* Something the player can step over or duck under is not in the way. */
    if (th->z >= block_probe_self->z + block_probe_self->height
        || th->z + th->height <= block_probe_self->z)
    {
        return true;
    }
    block_probe_hit = th;
    return false;
}

/* What is standing between here and there, if anything is.
 *
 * Sampled along the line at half the player's width, because a thing is a
 * circle and the question is whether the player's own circle can pass it. The
 * route's own tests are all about LINES - it is a distance field over the
 * level's geometry, and the geometry does not move - so without this a
 * barrel in a doorway is a route the agent is told is clear and cannot walk,
 * and the only symptom is a player pressing forward for six hundred
 * decisions with a correct bearing. */
static mobj_t *thing_in_the_way(mobj_t *player, fixed_t tx, fixed_t ty)
{
    fixed_t dx = tx - player->x, dy = ty - player->y;
    fixed_t len = P_AproxDistance(dx, dy);
    int steps, i;

    if (len <= 0)
    {
        return NULL;
    }
    /* Only what is immediately in the way. The waypoint can be four cells
     * off, and a solid thing anywhere along that line is not necessarily
     * something the player will even brush - Doom slides them past whatever
     * they touch. Within one decision's walking it is a different matter:
     * there is no getting past a barrel filling the doorway in front of you
     * without doing something about it. */
    if (len > 96 * FRACUNIT)
    {
        len = 96 * FRACUNIT;
        dx = FixedMul(FixedDiv(dx, P_AproxDistance(dx, dy)), len);
        dy = FixedMul(FixedDiv(dy, P_AproxDistance(dx, dy)), len);
    }
    steps = (len >> FRACBITS) / 16 + 1;
    block_probe_self = player;
    for (i = 1; i <= steps; i++)
    {
        fixed_t px = player->x + dx * i / steps;
        fixed_t py = player->y + dy * i / steps;
        fixed_t dist = player->radius + MAXRADIUS;
        int xl, xh, yl, yh, bx, by;

        block_probe_x = px;
        block_probe_y = py;
        block_probe_hit = NULL;
        xl = (px - dist - bmaporgx) >> MAPBLOCKSHIFT;
        xh = (px + dist - bmaporgx) >> MAPBLOCKSHIFT;
        yl = (py - dist - bmaporgy) >> MAPBLOCKSHIFT;
        yh = (py + dist - bmaporgy) >> MAPBLOCKSHIFT;
        for (bx = xl; bx <= xh; bx++)
        {
            for (by = yl; by <= yh; by++)
            {
                if (!P_BlockThingsIterator(bx, by, PIT_ThingInTheWay))
                {
                    return block_probe_hit;
                }
            }
        }
    }
    return NULL;
}

static line_t *shut_door_line;
static fixed_t shut_door_frac;

static boolean PTR_DoorTraverse(intercept_t *in)
{
    line_t *ld = in->d.line;

    if (ld->backsector == NULL || ld->frontsector == NULL)
    {
        /* A wall. Whatever is behind it is not on the way to anywhere. */
        return false;
    }
    P_LineOpening(ld);
    if (openrange >= ROUTE_HEIGHT)
    {
        return true;
    }
    if (!line_can_open(ld))
    {
        return false;
    }
    shut_door_line = ld;
    shut_door_frac = in->frac;
    return false;
}

/* Say so when a shut door stands between the player and where the route is
 * sending them.
 *
 * The field crosses doors on purpose - a player opens them - so "go north"
 * and "there is a door in the way" are both true, and the agent needs to be
 * told the second or the first is an instruction to press into wood. Until
 * this existed the only thing that reported a door was the walk test FAILING,
 * which is an accident waiting to happen: a cell centre can land exactly on a
 * linedef, and a traverse that ends on a line does not reliably intercept it.
 * Measured on E1M3, whose starting room's door lies along a row of cell
 * centres - every test said the way was clear, and the scripted player walked
 * at that door for the whole of fourteen hundred decisions. */
static void note_shut_door(mobj_t *player, api_route_t *out)
{
    fixed_t dx, dy, len, tx, ty;

    if (!out->have_step || out->blocked)
    {
        return;
    }
    {
        mobj_t *th = thing_in_the_way(player, out->x, out->y);

        if (th != NULL)
        {
            out->blocked_by_thing = true;
            out->thing_x = th->x;
            out->thing_y = th->y;
            out->thing_what = API_TypeName(th);
            out->thing_alive = (th->flags & MF_COUNTKILL) != 0 && th->health > 0;
            return;
        }
    }
    dx = out->x - player->x;
    dy = out->y - player->y;
    len = P_AproxDistance(dx, dy);
    if (len <= 0)
    {
        return;
    }
    /* Past the waypoint by a step, so a door sitting exactly on it counts. */
    tx = out->x + FixedMul(FixedDiv(dx, len), 24 * FRACUNIT);
    ty = out->y + FixedMul(FixedDiv(dy, len), 24 * FRACUNIT);
    shut_door_line = NULL;
    P_PathTraverse(player->x, player->y, tx, ty, PT_ADDLINES, PTR_DoorTraverse);
    if (shut_door_line != NULL)
    {
        out->blocked = true;
        out->can_open = true;
        out->block_x = player->x + FixedMul(tx - player->x, shut_door_frac);
        out->block_y = player->y + FixedMul(ty - player->y, shut_door_frac);
        note_opener(player, shut_door_line, out);
    }
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
        /* The player is standing somewhere the grid does not cover.
         *
         * That happens, and no amount of tuning the crossing tests will stop
         * it happening: the grid is a 32-unit sample of a level built from
         * arbitrary polygons, so there are always nooks a player can walk into
         * that no cell centre lands in, and pockets the crossing tests judge
         * sealed. Reporting no route at all there is the worst answer - the
         * agent loses the exit entirely at the exact moment it is lost - and
         * it is how a follower came to spend the rest of an episode in a
         * corner 400 units from its own spawn.
         *
         * So: find the nearest cell that IS on the route and that the player
         * can walk straight to, and steer back onto it. */
        int best = -1, bestd = 0, r;

        for (r = 1; r <= ROUTE_RECOVER && best < 0; r++)
        {
            int dx, dy;

            for (dy = -r; dy <= r; dy++)
            {
                for (dx = -r; dx <= r; dx++)
                {
                    int nx = cx + dx, ny = cy + dy, ni, dist;

                    /* The ring at this radius only; inner ones are done. */
                    if (abs(dx) != r && abs(dy) != r)
                    {
                        continue;
                    }
                    if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
                    {
                        continue;
                    }
                    ni = ny * grid_w + nx;
                    if (field[ni] == ROUTE_UNREACHED
                        || !can_walk_to(player, cell_x(nx), cell_y(ny)))
                    {
                        continue;
                    }
                    dist = field[ni];
                    if (best < 0 || dist < bestd)
                    {
                        best = ni;
                        bestd = dist;
                    }
                }
            }
        }
        if (best < 0)
        {
            /* Nothing the player can walk straight at. Take the nearest cell
             * that is on the field at all and aim at that.
             *
             * Standing still is the one answer that is always wrong here, and
             * it is what "no route" means to the agent above. Measured on
             * E1M3: the player walked into a nukage pool, every cell in it is
             * unwalkable while the route is trying to stay dry, so the ring
             * search found nothing, the observation carried no route - and
             * the scripted player strafed into the same wall seventy-eight
             * times and died there with the pool's edge forty units away. A
             * bearing that is roughly right beats no bearing. */
            for (r = 1; r <= ROUTE_RECOVER * 2 && best < 0; r++)
            {
                int dx, dy;

                for (dy = -r; dy <= r; dy++)
                {
                    for (dx = -r; dx <= r; dx++)
                    {
                        int nx = cx + dx, ny = cy + dy, ni;

                        if ((abs(dx) != r && abs(dy) != r) || nx < 0 || ny < 0
                            || nx >= grid_w || ny >= grid_h)
                        {
                            continue;
                        }
                        ni = ny * grid_w + nx;
                        if (field[ni] == ROUTE_UNREACHED)
                        {
                            continue;
                        }
                        if (best < 0 || field[ni] < bestd)
                        {
                            best = ni;
                            bestd = field[ni];
                        }
                    }
                }
            }
        }
        if (best < 0)
        {
            return false;
        }
        out->cells = field[best];
        out->have_step = true;
        out->blocked = false;
        out->can_open = false;
        out->have_switch = false;
        out->blocked_by_thing = false;
        out->goal_key = route_goal_key;
        out->goal_switch = route_goal_switch;
        out->goal_frontier = route_goal_frontier;
    out->goal_switch = route_goal_switch;
        out->x = cell_x(best % grid_w);
        out->y = cell_y(best / grid_w);
        note_shut_door(player, out);
        return true;
    }

    out->cells = field[at];
    out->have_step = false;
    out->blocked = false;
    out->can_open = false;
    out->have_switch = false;
    out->blocked_by_thing = false;
    out->goal_key = route_goal_key;
    out->goal_switch = route_goal_switch;
    out->goal_frontier = route_goal_frontier;
    if (field[at] == 0)
    {
        /* Standing at the goal. When the goal is a switch that is not the end
         * of it: the whole point of walking here was to press the thing, and
         * an agent told only "you have arrived" has nothing left to do and
         * wanders off - measured on E1M2, which reached its switch at
         * decision 162 and spent the next twelve hundred decisions elsewhere.
         * A key needs no such thing, because walking onto one picks it up. */
        /* Standing on the frontier is not arriving anywhere: the job was to
         * see what is past it, and that needs one more step. */
        if (route_goal_frontier && frontier_step >= 0)
        {
            out->have_step = true;
            out->x = cell_x(frontier_step % grid_w);
            out->y = cell_y(frontier_step / grid_w);
            /* And that step is very often through a door: the far side of one
             * is unseen by definition until it is opened, so the edge of the
             * explored world and the doors of a level are largely the same
             * set of places. */
            if (!can_walk_to(player, out->x, out->y) && walk_block_line != NULL)
            {
                line_t *ld = walk_block_line;

                out->blocked = true;
                out->can_open = ld->special != 0;
                out->block_x = player->x
                               + FixedMul(out->x - player->x, walk_block_frac);
                out->block_y = player->y
                               + FixedMul(out->y - player->y, walk_block_frac);
                note_opener(player, ld, out);
            }
            note_shut_door(player, out);
            return true;
        }
        if (route_goal_switch)
        {
            out->blocked = true;
            out->can_open = false;
            out->have_switch = true;
            out->block_x = route_switch_x;
            out->block_y = route_switch_y;
            out->switch_x = route_switch_x;
            out->switch_y = route_switch_y;
        }
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
                note_opener(player, ld, out);
            }
            /* A diagonal the player cannot walk straight at is still a
             * corner they can round: go via one of its two legs.
             *
             * The grid admits a diagonal step when an L-path of real edges
             * exists OR the point between the two cells is floor, and in both
             * cases the two orthogonal cells either side of it are where a
             * player standing anywhere but the exact middle of a cell has to
             * pass. Aiming at the far corner instead puts the bearing into the
             * wall between them, and the follower presses at it: measured on
             * E1M2, three cells for six hundred decisions, twelve hundred
             * units short of the exit, with the whole rest of the route
             * correct behind it. */
            {
                int fx = first % grid_w, fy = first / grid_w;

                if (fx != cx && fy != cy)
                {
                    int legs[2];
                    int i, pick = -1;

                    legs[0] = fy * grid_w + cx;
                    legs[1] = cy * grid_w + fx;
                    for (i = 0; i < 2; i++)
                    {
                        int lx = legs[i] % grid_w, ly = legs[i] / grid_w;

                        if (!walk[legs[i]]
                            || !can_walk_to(player, cell_x(lx), cell_y(ly)))
                        {
                            continue;
                        }
                        if (pick < 0 || field[legs[i]] < field[pick])
                        {
                            pick = legs[i];
                        }
                    }
                    if (pick >= 0)
                    {
                        out->have_step = true;
                        out->x = cell_x(pick % grid_w);
                        out->y = cell_y(pick / grid_w);
                        note_shut_door(player, out);
                        return true;
                    }
                }
            }

            /* The descent walks one chain of cells and gives up the moment
             * the next link is not straight ahead. That is too literal: the
             * player is standing somewhere in a cell rather than on its middle,
             * so which neighbours they can walk straight at depends on exactly
             * where they are, and in a corridor that bends the chain's own next
             * cell is often not one of them. Measured on E1M2, a follower
             * oscillating across five cells for four hundred decisions with the
             * route bearing swinging through 180 degrees.
             *
             * So look around instead: the nearest cell that is closer to the
             * goal than this one AND that the player can walk straight at from
             * where they are actually standing. Their own cell's middle
             * qualifies and is usually the answer when they are hugging a
             * wall, which is what this replaces. */
            {
                int best = -1, r;
                fixed_t best_d = 0;

                for (r = 0; r <= ROUTE_RECOVER; r++)
                {
                    int dx, dy;

                    for (dy = -r; dy <= r; dy++)
                    {
                        for (dx = -r; dx <= r; dx++)
                        {
                            int nx = cx + dx, ny = cy + dy, ni;
                            fixed_t d2;

                            if (abs(dx) != r && abs(dy) != r)
                            {
                                continue;
                            }
                            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
                            {
                                continue;
                            }
                            ni = ny * grid_w + nx;
                            if (field[ni] == ROUTE_UNREACHED || field[ni] >= field[at])
                            {
                                continue;
                            }
                            d2 = P_AproxDistance(cell_x(nx) - player->x,
                                                 cell_y(ny) - player->y);
                            /* Far enough to be a direction rather than noise. */
                            if (d2 < 12 * FRACUNIT)
                            {
                                continue;
                            }
                            if (!can_walk_to(player, cell_x(nx), cell_y(ny)))
                            {
                                continue;
                            }
                            if (best < 0 || field[ni] < field[best]
                                || (field[ni] == field[best] && d2 < best_d))
                            {
                                best = ni;
                                best_d = d2;
                            }
                        }
                    }
                    if (best >= 0)
                    {
                        break;
                    }
                }
                if (best >= 0)
                {
                    out->have_step = true;
                    out->x = cell_x(best % grid_w);
                    out->y = cell_y(best / grid_w);
                    note_shut_door(player, out);
                    return true;
                }
            }
            /* Nothing closer is straight ahead from where the player is
             * STANDING. Which neighbours are reachable depends on exactly
             * where in the cell they are, so the last resort is to stand in
             * the middle of it and ask again - which is what a person does
             * when they clip a doorframe. Measured on E1M2: two cells, eight
             * hundred decisions, two steps from the frontier, with the step
             * it needed refused from one side of the cell and open from the
             * other. */
            {
                fixed_t mx = cell_x(cx), my = cell_y(cy);

                if (P_AproxDistance(mx - player->x, my - player->y) > 8 * FRACUNIT
                    && can_walk_to(player, mx, my))
                {
                    out->have_step = true;
                    out->x = mx;
                    out->y = my;
                    note_shut_door(player, out);
                    return true;
                }
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
    note_shut_door(player, out);
    return true;
}

/* Why the cell the player is standing in is not on the field.
 *
 * A cell can be walkable, have edges to its neighbours, and still be nowhere
 * the distance field ever reached - the field floods from the exit over the
 * grid as it was at level load, and the player is walking the level as it is
 * NOW. A lift that has moved, a floor a switch raised, a door the grid was
 * built with shut: each of them is a place the player can get to and the field
 * cannot. From outside the engine every one of those looks the same, an agent
 * standing somewhere the route has no opinion about, and it is the line
 * specials in between that say which it is.
 *
 * So: cross the gap from the field to this cell, ignoring whether a player
 * could walk it, and report what is on the way. The same question, and the
 * same answer, that `report_boundary` prints at build time - asked here about
 * wherever the player has got to. */
static cJSON *explain_gap(int target)
{
    int *from = malloc(sizeof(int) * grid_w * grid_h);
    int *queue = malloc(sizeof(int) * grid_w * grid_h);
    int head = 0, tail = 0, k, steps = 0, at;
    cJSON *out, *arr;

    if (from == NULL || queue == NULL)
    {
        free(from);
        free(queue);
        return NULL;
    }
    for (k = 0; k < grid_w * grid_h; k++)
    {
        from[k] = -2;
        if (field[k] != ROUTE_UNREACHED)
        {
            from[k] = -1;
            queue[tail++] = k;
        }
    }
    while (head < tail && from[target] == -2)
    {
        int d;

        at = queue[head++];
        for (d = 0; d < 4; d++)
        {
            int nx = (at % grid_w) + ROUTE_DX[d];
            int ny = (at / grid_w) + ROUTE_DY[d];
            int ni;

            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (from[ni] != -2)
            {
                continue;
            }
            from[ni] = at;
            queue[tail++] = ni;
            if (ni == target)
            {
                break;
            }
        }
    }
    if (from[target] == -2)
    {
        free(from);
        free(queue);
        return NULL;
    }

    boundary_count = 0;
    at = target;
    while (from[at] >= 0)
    {
        int prev = from[at];

        P_PathTraverse(cell_x(at % grid_w), cell_y(at / grid_w),
                       cell_x(prev % grid_w), cell_y(prev / grid_w),
                       PT_ADDLINES, PTR_BoundaryTraverse);
        at = prev;
        steps++;
    }
    out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "cells", steps);
    cJSON_AddNumberToObject(out, "units", steps * ROUTE_CELL);
    cJSON_AddNumberToObject(out, "fieldX", cell_x(at % grid_w) >> FRACBITS);
    cJSON_AddNumberToObject(out, "fieldY", cell_y(at / grid_w) >> FRACBITS);
    arr = cJSON_CreateArray();
    for (k = 0; k < boundary_count; k++)
    {
        const char *name = special_name(boundary_specials[k]);
        cJSON *o = cJSON_CreateObject();

        cJSON_AddNumberToObject(o, "special", boundary_specials[k]);
        cJSON_AddStringToObject(o, "what", name != NULL ? name : "no special");
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(out, "lines", arr);
    free(from);
    free(queue);
    return out;
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
    /* How many one-way jumps the grid has, and where the one under the
     * player's feet goes. A teleporter the route does not know about is
     * indistinguishable from a level the route simply cannot cross, and from
     * outside the engine those want completely different things done. */
    cJSON_AddNumberToObject(root, "teleports", portal_count);
    if (portal_first != NULL && portal_first[at] >= 0)
    {
        int to = portal_to[portal_first[at]];
        cJSON *t = cJSON_CreateObject();

        cJSON_AddNumberToObject(t, "x", cell_x(to % grid_w) >> FRACBITS);
        cJSON_AddNumberToObject(t, "y", cell_y(to / grid_w) >> FRACBITS);
        cJSON_AddItemToObject(root, "stepsOnto", t);
    }
    /* Every jump the grid has: where stepping across a pad leaves from and
     * where it lands. A caller can ask the way to either end, which is how
     * "the route crosses a teleporter" is checkable from outside at all. */
    if (portal_first != NULL)
    {
        cJSON *jumps = cJSON_CreateArray();
        int k, i;

        for (k = 0; k < grid_w * grid_h; k++)
        {
            for (i = portal_first[k]; i >= 0; i = portal_next[i])
            {
                cJSON *t = cJSON_CreateObject();

                cJSON_AddNumberToObject(t, "fromX", cell_x(k % grid_w) >> FRACBITS);
                cJSON_AddNumberToObject(t, "fromY", cell_y(k / grid_w) >> FRACBITS);
                cJSON_AddNumberToObject(t, "x", cell_x(portal_to[i] % grid_w) >> FRACBITS);
                cJSON_AddNumberToObject(t, "y", cell_y(portal_to[i] / grid_w) >> FRACBITS);
                cJSON_AddItemToArray(jumps, t);
            }
        }
        cJSON_AddItemToObject(root, "teleportsTo", jumps);
    }
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

    /* Standing somewhere the field never reached: say what is in between,
     * because that is the whole question when a route stops answering. */
    if (field[at] == ROUTE_UNREACHED)
    {
        cJSON *gap = explain_gap(at);

        if (gap != NULL)
        {
            cJSON_AddItemToObject(root, "cutOffBy", gap);
        }
    }

    /* Standing somewhere the field never reached: say what is in between,
     * because that is the whole question when a route stops answering. */
    if (field[at] == ROUTE_UNREACHED)
    {
        cJSON *gap = explain_gap(at);

        if (gap != NULL)
        {
            cJSON_AddItemToObject(root, "cutOffBy", gap);
        }
    }

    /* Where the field would take the player from here, following the same
     * edges it was built over. It must descend to zero: a BFS field cannot do
     * otherwise. Printing it is how a route that is RIGHT and a player that
     * cannot follow it are told apart - compare this chain with the cells the
     * player actually visited, and the step it never manages is the bug. */
    arr = cJSON_CreateArray();
    {
        int here = at, n;

        for (n = 0; n < 24 && field[here] != ROUTE_UNREACHED && field[here] > 0; n++)
        {
            int best = -1, hx = here % grid_w, hy = here / grid_w;
            cJSON *o;

            for (d = 0; d < 8; d++)
            {
                int nx = hx + ROUTE_DX[d];
                int ny = hy + ROUTE_DY[d];
                int ni = ny * grid_w + nx;

                if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h
                    || !(edges[here] & (1 << d)) || field[ni] == ROUTE_UNREACHED)
                {
                    continue;
                }
                if (best < 0 || field[ni] < field[best])
                {
                    best = ni;
                }
            }
            if (best < 0 || field[best] >= field[here])
            {
                break;
            }
            o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "x", cell_x(best % grid_w) >> FRACBITS);
            cJSON_AddNumberToObject(o, "y", cell_y(best / grid_w) >> FRACBITS);
            cJSON_AddNumberToObject(o, "distance", field[best]);
            cJSON_AddBoolToObject(o, "diagonal",
                                  (best % grid_w) != hx && (best / grid_w) != hy);
            cJSON_AddItemToArray(arr, o);
            here = best;
        }
    }
    cJSON_AddItemToObject(root, "descent", arr);

    if (API_Route(player, &r) && r.have_step)
    {
        cJSON_AddNumberToObject(root, "aimX", r.x >> FRACBITS);
        cJSON_AddNumberToObject(root, "aimY", r.y >> FRACBITS);
    }
    return root;
}

void API_RouteFairPlay(boolean on)
{
    if (fair_play != on)
    {
        fair_play = on;
        field_stale = true;
    }
}

boolean API_RouteIsFair(void)
{
    return fair_play;
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
/* The way to the nearest floor that is not burning the player.
 *
 * A player standing in nukage can see where it ends; an agent given only "the
 * floor here is burning you" cannot, and the route is no help because it is
 * pointed at wherever the run is going, which on E1M3 is across more nukage.
 * Measured: the scripted player shuffled inside a pool for seventy decisions
 * and died in it with dry ground eighty units away.
 *
 * Breadth-first over the grid from where the player stands, crossing anything
 * they fit in, stopping at the first cell whose floor does not hurt. */
/* Which cells a search from the player is looking for. */
typedef boolean (*route_goal_t)(int cell, void *ctx);

/* Breadth-first over the walkable edges from where the player is standing, to
 * the nearest cell the goal test accepts.
 *
 * `lookahead` is where along the path the waypoint sits, counted in cells FROM
 * THE PLAYER: aiming straight at the far end of a path cuts the corners
 * through walls, because the bearing to it is a straight line and the path is
 * not. Negative asks for the goal itself, which is right only when the goal is
 * near enough that the straight line and the path are the same thing.
 *
 * `out->cells` is the whole path's length whatever the waypoint is, because
 * that is the answer to "how far away is it" and the waypoint is the answer to
 * "which way do I set off".
 */
static boolean route_bfs(mobj_t *player, route_goal_t want, void *ctx,
                         int lookahead, api_route_t *out)
{
    int cx, cy, head = 0, tail = 0, found = -1;
    int *queue, *parent;
    int i;

    if (player == NULL || edges == NULL || portal_first == NULL
        || !cell_of(player->x, player->y, &cx, &cy))
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
            int nx = at % grid_w + ROUTE_DX[d];
            int ny = at / grid_w + ROUTE_DY[d];
            int ni;

            if (!(edges[at] & (1 << d)) || nx < 0 || ny < 0
                || nx >= grid_w || ny >= grid_h)
            {
                continue;
            }
            ni = ny * grid_w + nx;
            if (parent[ni] != -2)
            {
                continue;
            }
            parent[ni] = at;
            if (want(ni, ctx))
            {
                found = ni;
                break;
            }
            queue[tail++] = ni;
        }
        for (d = portal_first[at]; d >= 0 && found < 0; d = portal_next[d])
        {
            int ni = portal_to[d];

            if (!walk[ni] || parent[ni] != -2)
            {
                continue;
            }
            parent[ni] = at;
            if (want(ni, ctx))
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
    {
        int at = found;
        int n = 0;
        int step;
        int *path = malloc(sizeof(int) * grid_w * grid_h);

        if (path == NULL)
        {
            free(queue);
            free(parent);
            return false;
        }
        while (at >= 0)
        {
            path[n++] = at;
            at = parent[at];
        }
        /* path[0] is the goal and path[n-1] the cell the player is in, so
         * counting from the player means counting down from the far end. */
        step = lookahead < 0 ? 0 : n - 1 - lookahead;
        if (step < 0)
        {
            step = 0;
        }
        out->cells = n - 1;
        /* Measured along the path rather than counted in cells: a diagonal
         * step covers about 1.414 cells of ground and a straight one covers
         * one, and the difference is what decides whether walking back for
         * something is worth the trip. */
        out->units = 0;
        for (i = 0; i + 1 < n; i++)
        {
            int ax = path[i] % grid_w, ay = path[i] / grid_w;
            int bx = path[i + 1] % grid_w, by = path[i + 1] / grid_w;

            out->units += (ax != bx && ay != by) ? (ROUTE_CELL * 181) / 128 : ROUTE_CELL;
        }
        out->have_step = true;
        out->x = cell_x(path[step] % grid_w);
        out->y = cell_y(path[step] / grid_w);
        free(path);
    }
    free(queue);
    free(parent);
    return true;
}

static boolean want_dry(int cell, void *ctx)
{
    (void)ctx;
    return cell_sector[cell] >= 0 && !hurts(sectors[cell_sector[cell]].special);
}

boolean API_DryLand(mobj_t *player, api_route_t *out)
{
    if (cell_sector == NULL || walk == NULL)
    {
        return false;
    }
    /* Aimed at the dry cell itself: the nearest one is a few cells away at
     * most, and a player standing in nukage wants the shortest way out of it
     * rather than a heading. */
    return route_bfs(player, want_dry, NULL, -1, out);
}

static boolean want_unvisited(int cell, void *ctx)
{
    (void)ctx;
    return !visited[cell];
}

boolean API_Frontier(mobj_t *player, api_route_t *out)
{
    int cx, cy;

    if (reachable == NULL || visited == NULL
        || !cell_of(player->x, player->y, &cx, &cy)
        || reachable[cy * grid_w + cx] == ROUTE_UNREACHED)
    {
        return false;
    }
    return route_bfs(player, want_unvisited, NULL, ROUTE_LOOKAHEAD, out);
}

static boolean want_cell(int cell, void *ctx)
{
    return cell == *(const int *)ctx;
}

boolean API_RouteTo(mobj_t *player, fixed_t x, fixed_t y, api_route_t *out)
{
    int gx, gy, goal;

    if (player == NULL)
    {
        return false;
    }
    /* The only entry point that is not downstream of an observation, so it is
     * the only one that can be the first thing asked of a level. */
    ensure_built(player);
    if (edges == NULL || !cell_of(x, y, &gx, &gy))
    {
        return false;
    }
    goal = gy * grid_w + gx;
    /* Standing on it already. A caller asking the way to where it is gets a
     * path of no cells rather than a failure, which is a different answer
     * from "there is no way there". */
    {
        int cx, cy;

        if (cell_of(player->x, player->y, &cx, &cy) && cy * grid_w + cx == goal)
        {
            out->cells = 0;
            out->units = 0;
            out->have_step = true;
            out->x = cell_x(gx);
            out->y = cell_y(gy);
            return true;
        }
    }
    return route_bfs(player, want_cell, &goal, ROUTE_LOOKAHEAD, out);
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

#ifndef __API_ROUTE_H__
#define __API_ROUTE_H__

#include "api.h"

typedef struct {
    /* Cells of walkable path between the player and the exit, 0 at it. */
    int cells;
    /* Whether there is a next doorway to walk to. */
    boolean have_step;
    fixed_t x;
    fixed_t y;
    /* What stands between the player and that next step, when something does.
     * A route may legitimately run through a shut door - a player opens it -
     * so the field treats one as passable and it falls to the agent to open
     * it. It cannot do that without being told the door is there. */
    boolean blocked;
    /* Whether pressing use against it does anything: a door, a lift or a
     * switch, as opposed to plain wall. */
    boolean can_open;
    fixed_t block_x;
    fixed_t block_y;
} api_route_t;

/* Where to go next to make progress toward the exit. False when the level has
 * no exit, or none reachable from where the player is standing. */
boolean API_Route(mobj_t *player, api_route_t *out);

#define API_MAX_EXIT_SPOTS 32

typedef struct {
    fixed_t x;
    fixed_t y;
} api_point_t;

/* Every walkable place a player might stand to use the level's exit. `probe`
 * supplies the radius the positions are tested against. Returns how many were
 * written. */
int API_ExitSpots(mobj_t *probe, api_point_t *out, int max);

/* The descent, cell by cell, as JSON: where the player is on the grid, what
 * every neighbouring step costs, which of them the player may take, and which
 * one the route chose. Enough to tell "the route is wrong" from "the player
 * cannot follow it", which from outside the engine look identical. */
cJSON *API_RouteDebug(mobj_t *player);

/* Where the player has been this episode, for the frontier search. */
void API_RouteMarkVisited(mobj_t *player);
void API_RouteForgetVisited(void);

typedef struct {
    int w;
    int h;
    int cell;
    fixed_t x0;
    fixed_t y0;
    /* w*h bytes: 0 unreachable, 1 reachable, 2 walked. Caller frees. */
    unsigned char *cells;
} api_map_t;

/* The explored map, for a viewer. */
boolean API_RouteMap(api_map_t *out);
boolean API_RouteCellOf(fixed_t x, fixed_t y, int *cx, int *cy);

/* A reachable place roughly `units` of walking from the exit, for a curriculum
 * that starts an episode a stated distance from its goal. */
boolean API_RouteSpotAt(int units, unsigned int seed, fixed_t *x, fixed_t *y);

/* The nearest place the player has NOT been, and the way to it. False when
 * every reachable place has been walked. */
boolean API_Frontier(mobj_t *player, api_route_t *out);

#endif

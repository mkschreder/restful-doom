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

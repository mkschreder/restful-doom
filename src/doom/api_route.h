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

#endif

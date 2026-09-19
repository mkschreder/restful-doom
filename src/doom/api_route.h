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
    /* Where the switch is, when what is in the way is opened from somewhere
     * else. DOOM's walls are full of these: a sector with a tag, and a line
     * carrying the special that operates that tag, standing somewhere the
     * player has to walk to. Nothing about the shut sector itself says where
     * that line is, so an agent looking at it can only push. */
    boolean have_switch;
    fixed_t switch_x;
    fixed_t switch_y;
    /* What this route actually leads to. 0 is the exit; otherwise the colour
     * of the key that the exit is locked behind, which the route heads for
     * first. Calling a key "the exit" in the observation would be a lie the
     * agent has no way to catch. */
    int goal_key;
    /* Whether what this route leads to is a switch that opens the way on,
     * rather than the exit or a key. Same idea as `goal_key`: the level's own
     * next objective, and calling it "the exit" would be a lie. */
    boolean goal_switch;
    /* A THING standing in the way of the next step - a monster, a barrel, a
     * lamp. The grid is geometry and deliberately knows nothing about who is
     * standing in it, so a perfectly good route can be one the player cannot
     * walk, and from outside that is indistinguishable from a route that is
     * wrong: the bearing is right, the way is clear, and the player does not
     * move. */
    boolean blocked_by_thing;
    fixed_t thing_x;
    fixed_t thing_y;
    const char *thing_what;
    /* Whether that thing is alive - something to shoot - as opposed to a
     * barrel or a lamp, which is something to walk round. */
    boolean thing_alive;
    /* Whether this route leads to unexplored ground rather than to anything
     * the player has found yet. Under fair play this is most of a level: the
     * exit is not a goal until it has been seen. */
    boolean goal_frontier;
} api_route_t;

/* Where to go next to make progress toward the exit. False when the level has
 * no exit, or none reachable from where the player is standing. */
boolean API_Route(mobj_t *player, api_route_t *out);

/* Key colours. A card and a skull of the same colour open the same doors. */
#define API_KEY_BLUE   1
#define API_KEY_YELLOW 2
#define API_KEY_RED    3

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

/* Whether the route may only cross ground the player has seen. Default true;
 * `false` restores the whole-level distance field, which is a solved map and
 * is only useful as a control to measure the honest one against. */
void API_RouteFairPlay(boolean on);
boolean API_RouteIsFair(void);

/* The explored map, for a viewer. */
boolean API_RouteMap(api_map_t *out);
boolean API_RouteCellOf(fixed_t x, fixed_t y, int *cx, int *cy);

/* A reachable place roughly `units` of walking from the exit, for a curriculum
 * that starts an episode a stated distance from its goal. */
boolean API_RouteSpotAt(int units, unsigned int seed, fixed_t *x, fixed_t *y);

/* The nearest floor that is not hurting the player, and the way to it. False
 * when there is none to be found - which for a player standing in nukage is
 * the difference between a way out and a death. */
boolean API_DryLand(mobj_t *player, api_route_t *out);

/* The nearest place the player has NOT been, and the way to it. False when
 * every reachable place has been walked. */
boolean API_Frontier(mobj_t *player, api_route_t *out);

#endif

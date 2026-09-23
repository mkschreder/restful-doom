#ifndef __API_ROUTE_H__
#define __API_ROUTE_H__

#include "api.h"

typedef struct {
    /* Cells of walkable path between the player and the exit, 0 at it. */
    int cells;
    /* The same path measured in map units, which is not the cell count times
     * the cell size: a diagonal step crosses one cell and covers about half
     * again as far, so a path with corners in it is under-reported by up to
     * two fifths by any count of cells. Only `API_RouteTo`, `API_Frontier`
     * and `API_DryLand` fill this in; the exit route is a distance field and
     * reports `cells`. */
    int units;
    /* Whether there is a next doorway to walk to. */
    boolean have_step;
    /* Whether that next step is one the player RIDES rather than walks: the
     * floor beyond it is higher than a player can climb, and something in the
     * level moves it. A lift, in other words, and the way on is to operate it
     * and wait - which is the one situation where standing still is progress,
     * and where an agent told only "walk that way" stands pressing forward
     * against a wall that was going to come down for it. */
    boolean step_is_ride;
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

/* Where the player has been, as bytes a snapshot can carry.
 *
 * This is PART OF THE WORLD as far as an agent is concerned, and leaving it
 * out of a snapshot was a real defect rather than an omission of convenience.
 * "The nearest ground nobody has looked at" is computed from it, so a restore
 * that does not put it back answers that question using wherever every OTHER
 * attempt happened to walk - the agent is pointed at a frontier that is not
 * its own, and two runs of identical actions diverge. Returns the byte count
 * and the grid it belongs to, or 0 when no grid is standing. */
int API_RouteVisitedBytes(int *w, int *h);
const unsigned char *API_RouteVisitedData(void);

/* Put one back. Applied at once when a grid of that size is standing, and
 * held until one is otherwise - a restore can arrive before the route has
 * ever been asked anything, and then the grid does not exist yet. */
void API_RouteRestoreVisited(const unsigned char *data, int w, int h);
void API_RouteForgetVisited(void);

// Throw away everything derived from the level that is standing: the grid,
// the flood, and how much of the map had been seen when the seen set was
// last computed.
//
// Needed because a restored snapshot puts back a level the route has already
// built against. The grid IS rebuilt whenever the lines move, but "the lines
// moved" is detected by comparing a pointer, and a zone allocator that hands
// back the block it just freed defeats that - which is the one case where
// the agent would be routed around a map that no longer exists.
void API_RouteInvalidate(void);

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

/* The way to a stated place, round whatever is between here and there. False
 * when there is no walkable path to it.
 *
 * The level's own distance field floods from the level's own goal, so it can
 * only ever answer "which way to the exit". An agent that remembers seeing a
 * medikit two rooms back has a different question, and without this the only
 * answer available is a straight line at it - which in a corridor is a
 * heading into the wall between here and there. */
boolean API_RouteTo(mobj_t *player, fixed_t x, fixed_t y, api_route_t *out);

#endif

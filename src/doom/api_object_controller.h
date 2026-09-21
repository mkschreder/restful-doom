#include <limits.h>

#include "api.h"

angle_t degreesToAngle(int degrees);

api_response_t API_PostObject(cJSON *req);
api_response_t API_GetObjects(int max_distance);
api_response_t API_PatchObject(int id, cJSON *req);
api_response_t API_DeleteObject(int id);
api_response_t API_GetObject(int id);
api_response_t API_GetLineOfSightToObject(int id, int id2);
api_response_t API_GetMoveTest(int id, float x, float y);
const char *API_TypeName(mobj_t *t);

/* What `API_CanStand` found. `floor` and `ceiling` are the surfaces that would
 * support and cover the body there; `why` names the rule that refused, and is
 * "ok" when none did. */
/* Ask about a spot on its own terms rather than as somewhere to move to. */
#define API_STAND_HERE INT_MIN

typedef struct {
    boolean ok;
    const char *why;
    fixed_t floor;
    fixed_t ceiling;
    fixed_t dropoff;
    mobj_t *blocker;
} api_stand_t;

/* Whether `probe` could be standing at (x, y), by the rules `P_TryMove`
 * applies and by no others.
 *
 * ONE predicate, because there were four and they disagreed. The route's grid
 * asked only whether a body's radius cleared the lines, and so admitted cells
 * with no headroom and cells a player would have to climb to; the movement
 * probe asked a fuller question but applied the dropoff rule to things the
 * engine exempts from it. A planner and the test that checks the planner have
 * to be asking the same question or agreement between them means nothing.
 *
 * `from_z` is the height the move sets off from. The step-up and headroom
 * rules are about the body that is arriving, so the same spot can be
 * reachable from one side and not from the other. Pass `API_STAND_HERE` to
 * ask the standalone question "does a body fit here", which puts it on
 * whatever surface actually supports it: a body has a radius, so the floor
 * holding it up is the highest one under any part of it, and that is not
 * knowable until the position has been checked.
 *
 * `ignore_things` asks about the LEVEL rather than the moment. The route's
 * grid is built once and a monster asleep in a doorway is not a wall - it
 * wanders off or dies, and recording it as geometry cuts the map in pieces
 * for the rest of the episode. What is standing in the way NOW is a question
 * for the observation, which answers it every step. */
boolean API_CanStand(mobj_t *probe, fixed_t x, fixed_t y, fixed_t from_z,
                     boolean ignore_things, api_stand_t *out);
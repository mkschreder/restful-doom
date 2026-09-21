#include <stdlib.h>

#include "api_object_controller.h"

#include "d_player.h"
#include "doomstat.h"
#include "p_local.h"

extern api_obj_description_t api_descriptors[];

// externally-defined game variables
extern void             P_KillMobj( mobj_t* source, mobj_t* target );

angle_t degreesToAngle(int degrees) {
    return ((float)degrees / 360) * ANG_MAX;
}

/* The human-readable name of a thing's type, as the API spells it. */
const char *API_TypeName(mobj_t *t)
{
    int i;

    for (i = 0; i < NUMDESCRIPTIONS; i++)
    {
        if (api_descriptors[i].id == mobjinfo[t->type].doomednum)
        {
            return api_descriptors[i].text;
        }
    }
    return "unknown";
}

int GetInternalTypeIdFromObjectDescription(char *objectDescription)
{
    for (int i = 0; i < NUMDESCRIPTIONS; i++)
    {
        if (strcmp(api_descriptors[i].text, objectDescription) == 0) {
            for (int j = 0; j < NUMMOBJTYPES; j++)
            {
                if (mobjinfo[j].doomednum == api_descriptors[i].id)
                {
                    return j;
                }
            }
            break;
        }
    }
    return -1;
}

mobj_t *FindObjectById(long id) 
{
    mobj_t *t;
    for (int i = 0; i < numsectors; i++) {
        t = sectors[i].thinglist;
        while (t)
        {
            if (t->id == id) {
                return t;
            }
            t = t->snext;    
        }
    }
    return NULL;
}

// Controller methods

api_response_t API_PostObject(cJSON *req)
{
    mobj_t *pobj;
    mobj_t *mobj;
    fixed_t angle;    
    fixed_t x, y, z;
    cJSON *val;
    cJSON *pos;
    cJSON *root;
    int dist;
    int typeNbr;
    char *type;

    if (M_CheckParm("-connect") > 0)
        return API_CreateErrorResponse(403, "Clients may not spawn objects");

    pobj = players[consoleplayer].mo;

    // `bearing` turns the spawn point around the player: degrees clockwise
    // from where they are facing, the same frame the observation reports
    // things in. Without it `distance` can only put something directly in
    // front, so a caller building a scenario - several monsters around the
    // player, the way ViZDoom's do - had to move the player between spawns.
    //
    // Distinct from `angle` below, which is the spawned object's OWN facing.
    angle = pobj->angle;
    val = cJSON_GetObjectItem(req, "bearing");
    if (val)
    {
        if (!cJSON_IsNumber(val))
        {
            return API_CreateErrorResponse(400, "bearing must be a number");
        }
        angle += degreesToAngle(val->valueint);
    }
    angle >>= ANGLETOFINESHIFT;

    val = cJSON_GetObjectItem(req, "distance");
    if (val)
    {
        dist = API_FloatToFixed(val->valueint);
        x = pobj->x + FixedMul(dist, finecosine[angle]);
        y = pobj->y + FixedMul(dist, finesine[angle]);
        // ON THE FLOOR, not the ceiling. A monster spawned at the ceiling
        // falls to the floor before it can act, and an item spawned there is
        // not on the floor to be walked over; both are what a caller asking
        // for something "250 units ahead" means.
        z = ONFLOORZ;
    }
    else
    {
        pos = cJSON_GetObjectItem(req, "position");
        val = cJSON_GetObjectItem(pos, "x");
        x = API_FloatToFixed(val->valuedouble);
        val = cJSON_GetObjectItem(pos, "y");
        y = API_FloatToFixed(val->valuedouble);
        val = cJSON_GetObjectItem(pos, "z");
        z = API_FloatToFixed(val->valuedouble);
    }

    type = cJSON_GetObjectItem(req, "type")->valuestring;
    typeNbr = GetInternalTypeIdFromObjectDescription(type);
    if (typeNbr == -1)
    {
        return API_CreateErrorResponse(400, "type not found");
    }

    mobj = P_SpawnMobj(x, y, z, typeNbr);

    // A monster spawned after the level loaded is not in the total the level
    // counted at setup, so without this "killed 3 of 6" stops being true the
    // moment anything is added - and a consumer scoring progress against that
    // total is scoring against a number that no longer describes the level.
    if (mobj->flags & MF_COUNTKILL)
    {
        totalkills++;
    }
    if (mobj->flags & MF_COUNTITEM)
    {
        totalitems++;
    }

    val = cJSON_GetObjectItem(req, "angle");
    if (val) mobj->angle = degreesToAngle(val->valueint);

    val = cJSON_GetObjectItem(req, "id");
    if (val) mobj->id = val->valueint;

    root = DescribeMObj(mobj);
    return (api_response_t) {201, root};
}

api_response_t API_GetObjects(int max_distance)
{
    cJSON *root;
    cJSON *objJson;
    mobj_t *t;
    mobj_t *player;
    float dist;

    root = cJSON_CreateArray();
    player = players[consoleplayer].mo;
    for (int i = 0; i < numsectors; i++) {
        t = sectors[i].thinglist;
        while (t)
        {
            dist = API_FixedToFloat(P_AproxDistance(player->x - t->x, player->y - t->y));
            if ((max_distance > 0 && dist > max_distance))
            {
                t = t->snext;
                continue;
            }

            objJson = DescribeMObj(t);
            cJSON_AddNumberToObject(objJson, "distance", dist);
            cJSON_AddItemToArray(root, objJson);
            t = t->snext;
        }
    }
    return (api_response_t) {200, root};
}

api_response_t API_PatchObject(int id, cJSON *req)
{
    mobj_t *obj;
    mobj_t *obj_to_attack;
    cJSON *pos;
    cJSON *val;
    cJSON *flags;
    cJSON *root;

    if (M_CheckParm("-connect") > 0)
        return API_CreateErrorResponse(403, "Clients may not modify objects");

    obj = FindObjectById(id);
    if (!obj)
    {
        return API_CreateErrorResponse(404, "object not found");
    }
    pos = cJSON_GetObjectItem(req, "position");
    if (pos) {
        cJSON *zval = cJSON_GetObjectItem(pos, "z");

        P_UnsetThingPosition(obj);
        val = cJSON_GetObjectItem(pos, "x");
        if (val) obj->x = API_FloatToFixed(val->valuedouble);

        val = cJSON_GetObjectItem(pos, "y");
        if (val) obj->y = API_FloatToFixed(val->valuedouble);

        if (zval) obj->z = API_FloatToFixed(zval->valuedouble);

        P_SetThingPosition(obj);

        // A sector's floor is only known once the thing is linked into it, and
        // it is rarely the floor of the sector the thing came from. Carrying
        // the old z across leaves the thing buried in the new floor or hanging
        // above it; buried, every P_TryMove is refused as too big a step up and
        // the thing cannot move in any direction at all.
        obj->floorz = obj->subsector->sector->floorheight;
        obj->ceilingz = obj->subsector->sector->ceilingheight;
        if (!zval)
        {
            obj->z = obj->floorz;
        }
        if (obj->z < obj->floorz) obj->z = obj->floorz;
        if (obj->z + obj->height > obj->ceilingz)
            obj->z = obj->ceilingz - obj->height;
    }
    val = cJSON_GetObjectItem(req, "angle");
    if (val) obj->angle = degreesToAngle(val->valueint);

    val = cJSON_GetObjectItem(req, "health");
    if (val) obj->health = val->valueint;

    val = cJSON_GetObjectItem(req, "attacking");
    if (val)
    {
        obj_to_attack = FindObjectById(val->valueint);
        if (!obj) {
            return API_CreateErrorResponse(400, "attacking object not valid");
        }
        obj->target = obj_to_attack;
        P_SetMobjState (obj, obj->info->seestate);
    }

    flags = cJSON_GetObjectItem(req, "flags");
    if (flags) {
        val = cJSON_GetObjectItem(flags, "MF_SHOOTABLE");
        if (val) API_FlipFlag(&obj->flags, MF_SHOOTABLE, val->valueint == 1);
        val = cJSON_GetObjectItem(flags, "MF_SHADOW");
        if (val) API_FlipFlag(&obj->flags, MF_SHADOW, val->valueint == 1);
        val = cJSON_GetObjectItem(flags, "MF_NOBLOOD");
        if (val) API_FlipFlag(&obj->flags, MF_NOBLOOD, val->valueint == 1);
        val = cJSON_GetObjectItem(flags, "MF_NOGRAVITY");
        if (val) API_FlipFlag(&obj->flags, MF_NOGRAVITY, val->valueint == 1);
    }
    root = DescribeMObj(obj);
    return (api_response_t) {200, root};
}

api_response_t API_DeleteObject(int id)
{
    mobj_t *obj;

    if (M_CheckParm("-connect") > 0)
        return API_CreateErrorResponse(403, "Clients may not delete objects");

    obj = FindObjectById(id);
    if (!obj)
    {
        return API_CreateErrorResponse(404, "object not found");
    }
    P_KillMobj(NULL, obj);
    return (api_response_t) {204, NULL};
}





api_response_t API_GetObject(int id)
{
    mobj_t *obj;
    cJSON *root;

    obj = FindObjectById(id);
    if (!obj)
    {
        return API_CreateErrorResponse(404, "object not found");
    }
    root = DescribeMObj(obj);
    return (api_response_t) {200, root};
}

api_response_t API_GetLineOfSightToObject(int id, int id2)
{
    boolean los;
    cJSON *root;
    mobj_t *obj = FindObjectById(id);
    mobj_t *obj2 = FindObjectById(id2);

    if (!obj || !obj2)
    {
        return API_CreateErrorResponse(404, "object not found");
    }

    los = P_CheckSight (obj, obj2);  

    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", obj->id);
    cJSON_AddNumberToObject(root, "id2", obj2->id);
    cJSON_AddBoolToObject(root,"los", los);
    
    return (api_response_t) {200, root};
}

//API_GetCheckMove
/* Why a move would be refused.
 *
 * The endpoint this replaces was named "movetest" and ran P_PathTraverse with
 * the AUTO-AIM callback, so it answered "can the thing SEE that point" - a
 * question a 32-unit-wide body does not care about. A ray threads a gap the
 * player wedges in, and a ray knows nothing about step height, headroom or a
 * corpse in the doorway. The symptom of trusting it is a route that looks
 * authoritative while the player stands still against a wall.
 *
 * What follows is P_TryMove's own test, minus the move: P_CheckPosition for
 * the box against lines and things, then the four height rules from
 * p_map.c. The reason a move fails is reported, because "no" on its own does
 * not tell an agent whether to open the door, find a step, or go around. */

static fixed_t probe_x, probe_y;
static mobj_t *probe_self;
static mobj_t *probe_hit;

static boolean PIT_ProbeBlocker(mobj_t *thing)
{
    fixed_t blockdist;

    if (thing == probe_self || !(thing->flags & MF_SOLID))
    {
        return true;
    }
    blockdist = thing->radius + probe_self->radius;
    if (abs(thing->x - probe_x) >= blockdist || abs(thing->y - probe_y) >= blockdist)
    {
        return true;
    }
    probe_hit = thing;
    return false;
}

/* The solid thing overlapping the target box, if one is what refused the move.
 * P_CheckPosition knows this and does not keep it, and the answer is the
 * difference between "shoot it" and "give up on this way through". */
static mobj_t *BlockingThingAt(mobj_t *self, fixed_t x, fixed_t y)
{
    int xl, xh, yl, yh, bx, by;
    fixed_t dist = self->radius + MAXRADIUS;

    probe_self = self;
    probe_x = x;
    probe_y = y;
    probe_hit = NULL;

    xl = (x - dist - bmaporgx) >> MAPBLOCKSHIFT;
    xh = (x + dist - bmaporgx) >> MAPBLOCKSHIFT;
    yl = (y - dist - bmaporgy) >> MAPBLOCKSHIFT;
    yh = (y + dist - bmaporgy) >> MAPBLOCKSHIFT;

    for (bx = xl; bx <= xh; bx++)
        for (by = yl; by <= yh; by++)
            if (!P_BlockThingsIterator(bx, by, PIT_ProbeBlocker))
                return probe_hit;
    return NULL;
}

api_response_t API_GetMoveTest(int id, float x, float y)
{
    fixed_t tx, ty, ox, oy, oz;
    boolean fits;
    const char *reason = "ok";
    boolean ok = true;
    mobj_t *blocker = NULL;
    cJSON *root;
    mobj_t *obj = FindObjectById(id);

    if (!obj)
    {
        return API_CreateErrorResponse(404, "object not found");
    }

    tx = API_FloatToFixed(x);
    ty = API_FloatToFixed(y);

    /* P_CheckPosition leaves its scratch state pointing at the thing and can
     * leave the thing itself moved; the probe must not disturb the game. */
    ox = obj->x;
    oy = obj->y;
    oz = obj->z;
    fits = P_CheckPosition(obj, tx, ty);
    obj->x = ox;
    obj->y = oy;
    obj->z = oz;

    if (!fits)
    {
        ok = false;
        blocker = BlockingThingAt(obj, tx, ty);
        reason = blocker ? "thing" : "wall";
    }
    else if (tmceilingz - tmfloorz < obj->height)
    {
        ok = false;
        reason = "does-not-fit";
    }
    else if (tmceilingz - obj->z < obj->height)
    {
        ok = false;
        reason = "headroom";
    }
    else if (tmfloorz - obj->z > 24 * FRACUNIT)
    {
        ok = false;
        reason = "step-up";
    }
    /* The dropoff rule does not apply to everything. `P_TryMove` exempts
     * anything carrying MF_DROPOFF or MF_FLOAT, and the player carries
     * MF_DROPOFF - walking off a ledge is a move a player makes. Asking this
     * of the player without the exemption refuses moves the engine itself
     * allows, which is the worse of the two errors here: it makes a level
     * look less connected than it is, and there is nothing in the answer to
     * say the answer is wrong. */
    else if (!(obj->flags & (MF_DROPOFF | MF_FLOAT))
             && tmfloorz - tmdropoffz > 24 * FRACUNIT)
    {
        ok = false;
        reason = "dropoff";
    }

    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "x", x);
    cJSON_AddNumberToObject(root, "y", y);
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "reason", reason);
    cJSON_AddNumberToObject(root, "z", obj->z >> FRACBITS);
    cJSON_AddNumberToObject(root, "floor", tmfloorz >> FRACBITS);
    cJSON_AddNumberToObject(root, "ceiling", tmceilingz >> FRACBITS);
    cJSON_AddNumberToObject(root, "dropoff", tmdropoffz >> FRACBITS);
    if (blocker)
    {
        cJSON_AddNumberToObject(root, "blockedBy", blocker->id);
        cJSON_AddStringToObject(root, "blockedByType", API_TypeName(blocker));
    }

    return (api_response_t) {200, root};
}

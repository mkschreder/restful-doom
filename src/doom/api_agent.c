//
// Agent-facing API: one observation per request, lockstep tics, episodes.
// See api_agent.h for why this is separate from the other controllers.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_agent.h"
#include "api_player_controller.h"
#include "api_object_controller.h"
#include "api_route.h"
#include "d_player.h"
#include "doomstat.h"
#include "g_game.h"
#include "m_random.h"
#include "p_local.h"
#include "r_main.h"
#include "r_state.h"
#include "tables.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_main.h"
#include "d_main.h"
#include "../d_event.h"
#include "../d_loop.h"
#include "../i_video.h"
#include "../m_misc.h"

// prndindex is the engine's second random cursor; doomstat.h exports rndindex
// but not this one, and seeding an episode means moving both.
extern int prndindex;

// How far the observation looks, in map units. 1024 is about four rooms of
// E1M1 and is already more than the player can see; beyond it the list is
// dominated by things behind walls that no decision depends on.
#define AGENT_SIGHT 1024

// Events are derived per tic and drained by whatever asks for a state next,
// so this only has to cover one step's worth. A step of 4 tics cannot produce
// 64 distinct state changes.
#define AGENT_MAX_EVENTS 64

// Everything within AGENT_SIGHT, gathered before it is sorted. Bounded: a
// room with more than this many things in it is not a room whose extra
// contents change a decision, and an unbounded list would put a malloc in the
// per-step path.
#define AGENT_MAX_NEAR 96

enum { NEAR_THREAT, NEAR_HAZARD, NEAR_PICKUP };

typedef struct
{
    mobj_t *thing;
    fixed_t dist;
    int kind;
} agent_near_t;

typedef struct
{
    int tic;
    const char *type;
    char detail[40];
    int amount;
} agent_event_t;

static agent_event_t events[AGENT_MAX_EVENTS];
static int event_count;
static int events_dropped;

// What the previous tic looked like. Events are the DIFFERENCE between two of
// these rather than hooks in the engine: every reward this environment pays
// out leaves a trace in the player's own counters, and a diff cannot miss a
// call site that a hook can.
typedef struct
{
    boolean valid;
    int health;
    int armor;
    int kills;
    int items;
    int secrets;
    int ammo[NUMAMMO];
    boolean weapons[NUMWEAPONS];
    boolean cards[NUMCARDS];
    boolean dead;
    gamestate_t state;
} agent_snapshot_t;

static agent_snapshot_t prev;

// Lockstep bookkeeping.
static boolean lockstep;
static int tics_remaining;
static boolean response_owed;
// An episode restart is deferred by the engine to the next tic, so the reply
// waits for the level to actually be running.
static boolean awaiting_level;
static int pending_seed;
static boolean have_pending_seed;
/* Where along the route to the exit the next episode starts, in map units.
 * Negative leaves the player at the level's own spawn. */
static int pending_start_distance = -1;
static int episode_start_tic;

// ---------------------------------------------------------------------------
//  Events
// ---------------------------------------------------------------------------

static void PushEvent(const char *type, const char *detail, int amount)
{
    agent_event_t *e;

    if (event_count >= AGENT_MAX_EVENTS)
    {
        events_dropped++;
        return;
    }
    e = &events[event_count++];
    e->tic = leveltime;
    e->type = type;
    e->amount = amount;
    if (detail != NULL)
    {
        M_StringCopy(e->detail, detail, sizeof(e->detail));
    }
    else
    {
        e->detail[0] = 0;
    }
}

static const char *ammo_names[NUMAMMO] = { "bullets", "shells", "cells", "rockets" };
static const char *weapon_names[NUMWEAPONS] =
{
    "fist", "pistol", "shotgun", "chaingun", "rocket launcher",
    "plasma rifle", "bfg", "chainsaw", "super shotgun"
};
static const char *card_names[NUMCARDS] =
{
    "blue keycard", "yellow keycard", "red keycard",
    "blue skull key", "yellow skull key", "red skull key"
};

static void SampleInto(agent_snapshot_t *s)
{
    player_t *p = &players[consoleplayer];
    int i;

    s->valid = true;
    s->health = p->health;
    s->armor = p->armorpoints;
    s->kills = p->killcount;
    s->items = p->itemcount;
    s->secrets = p->secretcount;
    for (i = 0; i < NUMAMMO; i++)
    {
        s->ammo[i] = p->ammo[i];
    }
    for (i = 0; i < NUMWEAPONS; i++)
    {
        s->weapons[i] = p->weaponowned[i];
    }
    for (i = 0; i < NUMCARDS; i++)
    {
        s->cards[i] = p->cards[i];
    }
    s->dead = p->playerstate == PST_DEAD || p->health <= 0;
    s->state = gamestate;
}

static void DeriveEvents(void)
{
    agent_snapshot_t now;
    int i;

    if (players[consoleplayer].mo == NULL)
    {
        return;
    }
    SampleInto(&now);

    if (!prev.valid)
    {
        prev = now;
        return;
    }

    if (now.health < prev.health)
    {
        PushEvent("hurt", NULL, prev.health - now.health);
    }
    else if (now.health > prev.health)
    {
        PushEvent("heal", NULL, now.health - prev.health);
    }
    if (now.armor > prev.armor)
    {
        PushEvent("armor", NULL, now.armor - prev.armor);
    }
    if (now.kills > prev.kills)
    {
        PushEvent("kill", NULL, now.kills - prev.kills);
    }
    if (now.items > prev.items)
    {
        PushEvent("item", NULL, now.items - prev.items);
    }
    if (now.secrets > prev.secrets)
    {
        PushEvent("secret", NULL, now.secrets - prev.secrets);
    }
    for (i = 0; i < NUMAMMO; i++)
    {
        if (now.ammo[i] > prev.ammo[i])
        {
            PushEvent("ammo", ammo_names[i], now.ammo[i] - prev.ammo[i]);
        }
    }
    for (i = 0; i < NUMWEAPONS; i++)
    {
        if (now.weapons[i] && !prev.weapons[i])
        {
            PushEvent("weapon", weapon_names[i], 1);
        }
    }
    for (i = 0; i < NUMCARDS; i++)
    {
        if (now.cards[i] && !prev.cards[i])
        {
            PushEvent("key", card_names[i], 1);
        }
    }
    if (now.dead && !prev.dead)
    {
        PushEvent("death", NULL, 1);
    }
    if (now.state != prev.state && now.state != GS_LEVEL && prev.state == GS_LEVEL)
    {
        // Leaving a live level without dying is the exit switch.
        PushEvent("exit", NULL, 1);
    }

    prev = now;
}

static cJSON *DescribeEvents(void)
{
    cJSON *arr = cJSON_CreateArray();
    int i;

    for (i = 0; i < event_count; i++)
    {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "tic", events[i].tic);
        cJSON_AddStringToObject(e, "type", events[i].type);
        if (events[i].detail[0])
        {
            cJSON_AddStringToObject(e, "what", events[i].detail);
        }
        cJSON_AddNumberToObject(e, "amount", events[i].amount);
        cJSON_AddItemToArray(arr, e);
    }
    return arr;
}

// ---------------------------------------------------------------------------
//  Observation
// ---------------------------------------------------------------------------

// Where something is RELATIVE TO THE PLAYER's facing, in degrees, negative to
// the left. An absolute map angle is the wrong frame for a decision: "the imp
// is 20 degrees to your right" is actionable and "the imp is at 143 degrees"
// is not, and converting between them is the kind of arithmetic a policy
// should not have to learn.
static int BearingTo(mobj_t *from, mobj_t *to)
{
    angle_t a = R_PointToAngle2(from->x, from->y, to->x, to->y);
    int rel = angleToDegrees(a - from->angle);

    if (rel > 180)
    {
        rel -= 360;
    }
    return rel;
}

static cJSON *DescribeThing(mobj_t *player, mobj_t *t)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "id", t->id);
    cJSON_AddStringToObject(o, "type", API_TypeName(t));
    cJSON_AddNumberToObject(o, "distance",
                            (int)API_FixedToFloat(P_AproxDistance(player->x - t->x,
                                                                  player->y - t->y)));
    cJSON_AddNumberToObject(o, "bearing", BearingTo(player, t));
    return o;
}

static cJSON *DescribeLevel(void)
{
    cJSON *o = cJSON_CreateObject();
    player_t *p = &players[consoleplayer];

    cJSON_AddNumberToObject(o, "episode", gameepisode);
    cJSON_AddNumberToObject(o, "map", gamemap);
    cJSON_AddNumberToObject(o, "skill", (int)gameskill);
    cJSON_AddNumberToObject(o, "tic", leveltime);
    cJSON_AddNumberToObject(o, "kills", p->killcount);
    cJSON_AddNumberToObject(o, "totalKills", totalkills);
    cJSON_AddNumberToObject(o, "items", p->itemcount);
    cJSON_AddNumberToObject(o, "totalItems", totalitems);
    cJSON_AddNumberToObject(o, "secrets", p->secretcount);
    cJSON_AddNumberToObject(o, "totalSecrets", totalsecret);
    return o;
}

static cJSON *DescribeAgentPlayer(void)
{
    player_t *p = &players[consoleplayer];
    mobj_t *mo = p->mo;
    cJSON *o = cJSON_CreateObject();
    cJSON *keys;
    int i;
    int weapon = (int)p->readyweapon;

    cJSON_AddNumberToObject(o, "id", mo != NULL ? mo->id : -1);
    // Whether the floor underfoot is hurting the player. A human sees the
    // screen flash red and their health tick down; without it an agent walks
    // across a nukage pool wondering why it is dying.
    if (mo != NULL && mo->subsector != NULL && mo->subsector->sector != NULL)
    {
        int sp = mo->subsector->sector->special;

        cJSON_AddBoolToObject(o, "standingInDamage",
                              sp == 4 || sp == 5 || sp == 7 || sp == 16 || sp == 11);
    }
    cJSON_AddNumberToObject(o, "health", p->health);
    cJSON_AddNumberToObject(o, "armor", p->armorpoints);
    if (mo != NULL)
    {
        cJSON_AddNumberToObject(o, "x", (int)API_FixedToFloat(mo->x));
        cJSON_AddNumberToObject(o, "y", (int)API_FixedToFloat(mo->y));
        cJSON_AddNumberToObject(o, "angle", angleToDegrees(mo->angle));
    }
    if (weapon >= 0 && weapon < NUMWEAPONS)
    {
        cJSON_AddStringToObject(o, "weapon", weapon_names[weapon]);
        cJSON_AddNumberToObject(o, "ammo", weaponinfo[weapon].ammo < NUMAMMO
                                              ? p->ammo[weaponinfo[weapon].ammo]
                                              : -1);
    }
    keys = cJSON_CreateArray();
    for (i = 0; i < NUMCARDS; i++)
    {
        if (p->cards[i])
        {
            cJSON_AddItemToArray(keys, cJSON_CreateString(card_names[i]));
        }
    }
    cJSON_AddItemToObject(o, "keys", keys);
    return o;
}

// What is around the player, in three lists that mean different things:
//
//   threats  live MONSTERS (MF_COUNTKILL - the same set the level's kill
//            total counts, so "killed 3 of 4" and this list agree)
//   hazards  other shootable things, which on E1M1 means exploding barrels:
//            worth shooting, but not progress and not a danger on their own
//   pickups  MF_SPECIAL, everything that can be walked over
//
// Lumping the first two together is what the first cut did, and it reported a
// barrel as an enemy standing 384 units away - a policy told to clear the
// room would have been scored against a target that does not count.
static void DescribeSurroundings(cJSON *root)
{
    cJSON *threats = cJSON_CreateArray();
    cJSON *hazards = cJSON_CreateArray();
    cJSON *pickups = cJSON_CreateArray();
    mobj_t *player = players[consoleplayer].mo;
    agent_near_t near[AGENT_MAX_NEAR];
    int n = 0;
    int i;

    if (player == NULL)
    {
        cJSON_AddItemToObject(root, "threats", threats);
        cJSON_AddItemToObject(root, "hazards", hazards);
        cJSON_AddItemToObject(root, "pickups", pickups);
        return;
    }

    for (i = 0; i < numsectors && n < AGENT_MAX_NEAR; i++)
    {
        mobj_t *t = sectors[i].thinglist;

        while (t && n < AGENT_MAX_NEAR)
        {
            fixed_t d = P_AproxDistance(player->x - t->x, player->y - t->y);
            int kind = -1;

            if (t == player || API_FixedToFloat(d) > AGENT_SIGHT)
            {
                t = t->snext;
                continue;
            }
            if ((t->flags & MF_SHOOTABLE) && t->health > 0 && !(t->flags & MF_CORPSE))
            {
                kind = (t->flags & MF_COUNTKILL) ? NEAR_THREAT : NEAR_HAZARD;
            }
            else if (t->flags & MF_SPECIAL)
            {
                kind = NEAR_PICKUP;
            }
            if (kind >= 0)
            {
                near[n].thing = t;
                near[n].dist = d;
                near[n].kind = kind;
                n++;
            }
            t = t->snext;
        }
    }

    // BY DISTANCE. The sectors are walked in map order, so without this the
    // "first" threat is an arbitrary one - which is not a cosmetic detail: a
    // policy offered "shoot the nearest imp" was being pointed at whichever
    // monster the map file happened to list first, three rooms away and
    // through a wall. Insertion sort, because n is at most AGENT_MAX_NEAR and
    // this runs once per observation.
    for (i = 1; i < n; i++)
    {
        agent_near_t key = near[i];
        int j = i - 1;

        while (j >= 0 && near[j].dist > key.dist)
        {
            near[j + 1] = near[j];
            j--;
        }
        near[j + 1] = key;
    }

    for (i = 0; i < n; i++)
    {
        mobj_t *t = near[i].thing;
        cJSON *o = DescribeThing(player, t);

        switch (near[i].kind)
        {
            case NEAR_THREAT:
                cJSON_AddNumberToObject(o, "health", t->health);
                cJSON_AddBoolToObject(o, "visible", P_CheckSight(player, t));
                cJSON_AddBoolToObject(o, "targetingMe", t->target == player);
                cJSON_AddItemToArray(threats, o);
                break;
            case NEAR_HAZARD:
                cJSON_AddNumberToObject(o, "health", t->health);
                cJSON_AddBoolToObject(o, "visible", P_CheckSight(player, t));
                cJSON_AddItemToArray(hazards, o);
                break;
            default:
                cJSON_AddBoolToObject(o, "visible", P_CheckSight(player, t));
                cJSON_AddItemToArray(pickups, o);
                break;
        }
    }

    cJSON_AddItemToObject(root, "threats", threats);
    cJSON_AddItemToObject(root, "hazards", hazards);
    cJSON_AddItemToObject(root, "pickups", pickups);
}

// How far the player could WALK in a given direction before the level stops
// them, capped at AGENT_PROBE_MAX.
//
// This is a geometry question, not an occupancy one. The first version asked
// P_CheckPosition whether the player could STAND at a series of points along
// the ray, and that is a stricter test than walking: Doom slides a player
// along whatever they brush against, so a decorative pillar or a floor lamp
// standing beside the path made the probe report a wall where a player walks
// straight past. Measured on E1M1, it reported "0 units ahead" at a spot the
// player then crossed 83 units of - and the action set, which hides "walk
// forward" when there is no room, had hidden the only option that was going
// anywhere.
//
// So: trace the ray against LINES only, the way the engine's own sight and
// shot code does, and stop at the first one that cannot be walked through -
// one-sided, explicitly blocking, too short to fit in, or a step too high to
// climb. A closed door stops it, which is correct and is what makes "push the
// door in front of you" the right option to offer next.
#define AGENT_PROBE_MAX 320
/// The player's own height and the tallest step they can climb, from p_map.c.
#define AGENT_HEIGHT (56 * FRACUNIT)
#define AGENT_STEP (24 * FRACUNIT)

static fixed_t probe_frac;
static fixed_t probe_z;

static boolean PTR_ProbeTraverse(intercept_t *in)
{
    line_t *ld = in->d.line;

    if (!(ld->flags & ML_TWOSIDED) || (ld->flags & ML_BLOCKING))
    {
        probe_frac = in->frac;
        return false;
    }
    P_LineOpening(ld);
    if (openrange < AGENT_HEIGHT || openbottom - probe_z > AGENT_STEP)
    {
        probe_frac = in->frac;
        return false;
    }
    return true;
}

static int Clearance(mobj_t *player, int bearing_deg)
{
    angle_t a = player->angle + degreesToAngle(bearing_deg);
    fixed_t fx = finecosine[a >> ANGLETOFINESHIFT];
    fixed_t fy = finesine[a >> ANGLETOFINESHIFT];
    fixed_t tx = player->x + FixedMul(AGENT_PROBE_MAX << FRACBITS, fx);
    fixed_t ty = player->y + FixedMul(AGENT_PROBE_MAX << FRACBITS, fy);

    probe_frac = FRACUNIT;
    probe_z = player->z;
    P_PathTraverse(player->x, player->y, tx, ty, PT_ADDLINES, PTR_ProbeTraverse);

    // Round DOWN to whole units: a reader comparing this against a threshold
    // should never be told there is room that is not there.
    return (int)((long long)AGENT_PROBE_MAX * probe_frac / FRACUNIT);
}

static void DescribeClearance(cJSON *root)
{
    mobj_t *player = players[consoleplayer].mo;
    cJSON *o;
    static const struct { const char *name; int bearing; } dirs[] =
    {
        { "ahead", 0 }, { "right", 90 }, { "behind", 180 }, { "left", 270 },
        { "aheadRight", 45 }, { "aheadLeft", 315 },
    };
    unsigned int i;

    if (player == NULL)
    {
        return;
    }
    o = cJSON_CreateObject();
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    {
        cJSON_AddNumberToObject(o, dirs[i].name, Clearance(player, dirs[i].bearing));
    }
    cJSON_AddItemToObject(root, "clearance", o);
}

// The nearest line that ends the level, as a bearing and a distance.
//
// A level is won by reaching its exit, and nothing else in the observation
// says where that is - a policy reading only what is visible can clear every
// monster and still never finish. Doom marks the exit on a linedef special:
// 11 and 51 are switches to press, 52 and 124 are lines to walk over.
static void DescribeExit(cJSON *root)
{
    mobj_t *player = players[consoleplayer].mo;
    line_t *best = NULL;
    fixed_t best_dist = 0;
    int i;

    if (player == NULL)
    {
        return;
    }
    for (i = 0; i < numlines; i++)
    {
        int sp = lines[i].special;
        fixed_t mx, my, d;

        if (sp != 11 && sp != 51 && sp != 52 && sp != 124)
        {
            continue;
        }
        mx = (lines[i].v1->x + lines[i].v2->x) / 2;
        my = (lines[i].v1->y + lines[i].v2->y) / 2;
        d = P_AproxDistance(player->x - mx, player->y - my);
        if (best == NULL || d < best_dist)
        {
            best = &lines[i];
            best_dist = d;
        }
    }
    if (best != NULL)
    {
        fixed_t mx = (best->v1->x + best->v2->x) / 2;
        fixed_t my = (best->v1->y + best->v2->y) / 2;
        angle_t a = R_PointToAngle2(player->x, player->y, mx, my);
        int rel = angleToDegrees(a - player->angle);
        cJSON *o = cJSON_CreateObject();

        if (rel > 180)
        {
            rel -= 360;
        }
        cJSON_AddNumberToObject(o, "distance", (int)API_FixedToFloat(best_dist));
        cJSON_AddNumberToObject(o, "bearing", rel);
        // How far the player would actually get if they set off toward it.
        // Without this, "head for the exit" is an option whose feasibility
        // nothing in the observation reports: the six fixed probes are at
        // fixed bearings and the exit is wherever it is, so an agent offered
        // the exit had no way to know it was on the far side of a wall - and
        // a scripted one spent whole episodes walking into that wall.
        cJSON_AddNumberToObject(o, "clearance", Clearance(player, rel));
        cJSON_AddStringToObject(o, "kind",
                                (best->special == 11 || best->special == 51)
                                    ? "switch" : "walkover");

        // A place to STAND to use this exit.
        //
        // Not the line's midpoint, which is inside the wall the switch is
        // mounted on. The walkable side is found by stepping off the line
        // along its own normal and asking which offsets the player could
        // actually occupy - geometry the engine can answer and a caller
        // cannot, and without it "put me at the exit" means "put me inside a
        // wall".
        //
        // A SEARCH rather than one offset: a single 48-unit step found nothing
        // on E1M1, because the space in front of an exit switch is whatever
        // the level author happened to leave there. Both sides, several
        // distances, three points along the line, nearest first.
        {
            fixed_t dx = best->v2->x - best->v1->x;
            fixed_t dy = best->v2->y - best->v1->y;
            fixed_t len = P_AproxDistance(dx, dy);
            cJSON *spot = cJSON_CreateObject();
            boolean found = false;
            static const int offsets[] = { 32, 56, 80, 112, 144, 192 };
            static const int alongs[] = { 2, 1, 3 };
            unsigned int oi, ai;
            int side;

            for (oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]) && !found; oi++)
            {
                for (ai = 0; ai < sizeof(alongs) / sizeof(alongs[0]) && !found; ai++)
                {
                    fixed_t px = best->v1->x + dx * alongs[ai] / 4;
                    fixed_t py = best->v1->y + dy * alongs[ai] / 4;

                    for (side = 0; side < 2 && !found && len > 0; side++)
                    {
                        fixed_t sign = side == 0 ? FRACUNIT : -FRACUNIT;
                        fixed_t nx = FixedDiv(FixedMul(dy, sign), len);
                        fixed_t ny = FixedDiv(FixedMul(-dx, sign), len);
                        fixed_t sx = px + FixedMul(offsets[oi] * FRACUNIT, nx);
                        fixed_t sy = py + FixedMul(offsets[oi] * FRACUNIT, ny);
                        fixed_t ox = player->x;
                        fixed_t oy = player->y;
                        fixed_t oz = player->z;

                        if (P_CheckPosition(player, sx, sy))
                        {
                            cJSON_AddNumberToObject(spot, "x", API_FixedToFloat(sx));
                            cJSON_AddNumberToObject(spot, "y", API_FixedToFloat(sy));
                            found = true;
                        }
                        player->x = ox;
                        player->y = oy;
                        player->z = oz;
                    }
                }
            }
            if (found)
            {
                cJSON_AddItemToObject(o, "spot", spot);
            }
            else
            {
                cJSON_Delete(spot);
            }
        }


        // The way a player would actually WALK there: how many rooms away the
        // exit is, and the bearing to the next doorway on the route. The
        // straight-line bearing above is kept because it is what a player
        // facing the right way sees, but it points through walls and an agent
        // that followed it pressed against them - see api_route.c.
        {
            api_route_t route;

            if (API_Route(player, &route))
            {
                cJSON_AddNumberToObject(o, "pathDistance", route.cells * 32);
                if (route.have_step)
                {
                    angle_t ra = R_PointToAngle2(player->x, player->y, route.x, route.y);
                    int rrel = angleToDegrees(ra - player->angle);

                    if (rrel > 180)
                    {
                        rrel -= 360;
                    }
                    cJSON_AddNumberToObject(o, "routeBearing", rrel);
                    cJSON_AddNumberToObject(
                        o, "routeDistance",
                        (int)API_FixedToFloat(P_AproxDistance(player->x - route.x,
                                                              player->y - route.y)));
                    cJSON_AddNumberToObject(o, "routeClearance", Clearance(player, rrel));
                }
                // What is in the way, when the route says one thing and the
                // player's body says another. Almost always a shut door: the
                // route runs through doors because a player opens them.
                if (route.blocked)
                {
                    angle_t ba = R_PointToAngle2(player->x, player->y,
                                                 route.block_x, route.block_y);
                    int brel = angleToDegrees(ba - player->angle);
                    cJSON *b = cJSON_CreateObject();

                    if (brel > 180)
                    {
                        brel -= 360;
                    }
                    cJSON_AddStringToObject(b, "kind",
                                            route.can_open ? "door" : "wall");
                    cJSON_AddNumberToObject(b, "bearing", brel);
                    cJSON_AddNumberToObject(
                        b, "distance",
                        (int)API_FixedToFloat(P_AproxDistance(
                            player->x - route.block_x, player->y - route.block_y)));
                    cJSON_AddItemToObject(o, "blockedBy", b);
                }
            }
        }

        cJSON_AddItemToObject(root, "exit", o);

    // The nearest place the player has not been, and the way there. This is
    // what makes a level explorable rather than wandered: the search crosses
    // ground already walked, so an agent is never trapped on the near side of
    // a corridor it has crossed with unexplored space beyond it.
    {
        api_route_t frontier;

        if (API_Frontier(player, &frontier) && frontier.have_step)
        {
            angle_t fa = R_PointToAngle2(player->x, player->y, frontier.x, frontier.y);
            int frel = angleToDegrees(fa - player->angle);
            cJSON *f = cJSON_CreateObject();

            if (frel > 180)
            {
                frel -= 360;
            }
            cJSON_AddNumberToObject(f, "distance", frontier.cells * 32);
            cJSON_AddNumberToObject(f, "bearing", frel);
            cJSON_AddNumberToObject(f, "clearance", Clearance(player, frel));
            cJSON_AddItemToObject(root, "unexplored", f);
        }
    }
    }
}

// Whether the episode is over, and how it went. An episode ends when the
// player dies or when the level is left - the two things this environment is
// scored on.
static const char *Outcome(boolean *done)
{
    player_t *p = &players[consoleplayer];

    if (p->playerstate == PST_DEAD || p->health <= 0)
    {
        *done = true;
        return "dead";
    }
    if (gamestate != GS_LEVEL)
    {
        *done = true;
        return "exited";
    }
    *done = false;
    return "alive";
}

static cJSON *BuildState(void)
{
    cJSON *root = cJSON_CreateObject();
    boolean done = false;
    const char *outcome = Outcome(&done);

    cJSON_AddNumberToObject(root, "tic", gametic);
    cJSON_AddNumberToObject(root, "episodeTic", gametic - episode_start_tic);
    cJSON_AddItemToObject(root, "level", DescribeLevel());
    cJSON_AddItemToObject(root, "player", DescribeAgentPlayer());
    DescribeSurroundings(root);
    DescribeClearance(root);
    DescribeExit(root);
    cJSON_AddItemToObject(root, "events", DescribeEvents());
    if (events_dropped > 0)
    {
        cJSON_AddNumberToObject(root, "eventsDropped", events_dropped);
    }
    cJSON_AddBoolToObject(root, "done", done);
    cJSON_AddStringToObject(root, "outcome", outcome);

    // Drained: an event is reported exactly once, to exactly one reader.
    event_count = 0;
    events_dropped = 0;
    return root;
}

api_response_t API_GetState(void)
{
    return (api_response_t) { 200, BuildState() };
}

// ---------------------------------------------------------------------------
//  Frame
// ---------------------------------------------------------------------------

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Returns a malloc'd NUL-terminated string the caller owns.
static char *Base64(const byte *in, int len)
{
    int out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    int i;
    int o = 0;

    if (out == NULL)
    {
        return NULL;
    }
    for (i = 0; i + 2 < len; i += 3)
    {
        unsigned int v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = b64[(v >> 6) & 63];
        out[o++] = b64[v & 63];
    }
    if (i < len)
    {
        unsigned int v = in[i] << 16;
        boolean two = i + 1 < len;

        if (two)
        {
            v |= in[i + 1] << 8;
        }
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = two ? b64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

// The 320x200 8-bit framebuffer the engine just drew, plus the palette it
// would be shown through. Indexed rather than RGB because it is a fifth of
// the bytes and the expansion is one table lookup on the far side; the
// palette is the CURRENT one, so the damage and pickup tints a player would
// see are in the frame a reader gets.
// The explored map: what the agent knows about where it can go and where it
// has been. Served separately from the frame because a viewer wants both and
// a policy wants neither.
api_response_t API_GetRouteDebug(void)
{
    mobj_t *player = players[consoleplayer].mo;
    cJSON *root;

    if (player == NULL)
    {
        return API_CreateErrorResponse(404, "no player");
    }
    root = API_RouteDebug(player);
    if (root == NULL)
    {
        return API_CreateErrorResponse(500, "no route information");
    }
    return (api_response_t) {200, root};
}

api_response_t API_GetMap(void)
{
    api_map_t map;
    mobj_t *player = players[consoleplayer].mo;
    cJSON *root;
    char *encoded;
    int cx = -1, cy = -1;

    if (!API_RouteMap(&map))
    {
        return API_CreateErrorResponse(503, "no map has been built for this level");
    }
    encoded = Base64(map.cells, map.w * map.h);
    free(map.cells);
    if (encoded == NULL)
    {
        return API_CreateErrorResponse(500, "out of memory");
    }
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "width", map.w);
    cJSON_AddNumberToObject(root, "height", map.h);
    cJSON_AddNumberToObject(root, "cell", map.cell);
    cJSON_AddStringToObject(root, "legend", "0 unreachable, 1 reachable, 2 walked");
    cJSON_AddStringToObject(root, "cells", encoded);
    free(encoded);
    if (player != NULL && API_RouteCellOf(player->x, player->y, &cx, &cy))
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "x", cx);
        cJSON_AddNumberToObject(p, "y", cy);
        cJSON_AddNumberToObject(p, "angle", angleToDegrees(player->angle));
        cJSON_AddItemToObject(root, "player", p);
    }
    return (api_response_t) { 200, root };
}

api_response_t API_GetFrame(void)
{
    cJSON *root;
    char *pix;
    char *pal;
    byte pal_bytes[768];
    int i;

    if (I_VideoBuffer == NULL)
    {
        return API_CreateErrorResponse(503, "no framebuffer yet");
    }

    I_GetPaletteRGB(pal_bytes);

    pix = Base64(I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
    pal = Base64(pal_bytes, sizeof(pal_bytes));
    if (pix == NULL || pal == NULL)
    {
        free(pix);
        free(pal);
        return API_CreateErrorResponse(500, "out of memory");
    }

    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "width", SCREENWIDTH);
    cJSON_AddNumberToObject(root, "height", SCREENHEIGHT);
    cJSON_AddStringToObject(root, "format", "indexed8");
    cJSON_AddStringToObject(root, "pixels", pix);
    cJSON_AddStringToObject(root, "palette", pal);
    free(pix);
    free(pal);
    (void)i;
    return (api_response_t) { 200, root };
}

// ---------------------------------------------------------------------------
//  Episodes and stepping
// ---------------------------------------------------------------------------

void API_Agent_Init(void)
{
    lockstep = M_CheckParm("-apilockstep") > 0;
    if (lockstep)
    {
        // Run one tic per pass of the game loop and never wait on the wall
        // clock: in lockstep the agent is the clock.
        singletics = true;
        printf("API_Agent: lockstep - the game advances only on POST /api/step\n");
    }
    memset(&prev, 0, sizeof(prev));
    episode_start_tic = gametic;
}

boolean API_Agent_Lockstep(void)
{
    return lockstep;
}

api_response_t API_PostEpisode(cJSON *req)
{
    cJSON *val;
    int episode = gameepisode;
    int map = gamemap;
    int skill = (int)gameskill;

    val = cJSON_GetObjectItem(req, "episode");
    if (val != NULL)
    {
        if (!cJSON_IsNumber(val))
        {
            return API_CreateErrorResponse(400, "episode must be a number");
        }
        episode = val->valueint;
    }
    val = cJSON_GetObjectItem(req, "map");
    if (val != NULL)
    {
        if (!cJSON_IsNumber(val))
        {
            return API_CreateErrorResponse(400, "map must be a number");
        }
        map = val->valueint;
    }
    val = cJSON_GetObjectItem(req, "skill");
    if (val != NULL)
    {
        if (!cJSON_IsNumber(val) || val->valueint < 0 || val->valueint > 4)
        {
            return API_CreateErrorResponse(400, "skill must be 0-4");
        }
        skill = val->valueint;
    }
    val = cJSON_GetObjectItem(req, "startDistance");
    if (val != NULL)
    {
        if (!cJSON_IsNumber(val))
        {
            return API_CreateErrorResponse(400, "startDistance must be a number");
        }
        pending_start_distance = val->valueint;
    }
    else
    {
        pending_start_distance = -1;
    }

    val = cJSON_GetObjectItem(req, "seed");
    if (val != NULL)
    {
        if (!cJSON_IsNumber(val))
        {
            return API_CreateErrorResponse(400, "seed must be a number");
        }
        pending_seed = val->valueint;
        have_pending_seed = true;
    }

    G_DeferedInitNew((skill_t)skill, episode, map);

    // The engine acts on that at the top of the next tic, and the level is not
    // there to describe until it has. Hold the reply until it is.
    awaiting_level = true;
    tics_remaining = 0;
    response_owed = true;
    event_count = 0;
    events_dropped = 0;
    memset(&prev, 0, sizeof(prev));

    // Status 0 is this file's signal to the transport that the response is
    // owed but not ready - see API_Agent_PerTic.
    return (api_response_t) { 0, NULL };
}

api_response_t API_PostStep(cJSON *req)
{
    cJSON *val;
    cJSON *actions;
    int tics = 1;

    if (!lockstep)
    {
        return API_CreateErrorResponse(409,
            "not running in lockstep; restart with -apilockstep");
    }

    val = cJSON_GetObjectItem(req, "tics");
    if (val != NULL)
    {
        if (!cJSON_IsNumber(val) || val->valueint < 1 || val->valueint > 350)
        {
            return API_CreateErrorResponse(400, "tics must be 1-350");
        }
        tics = val->valueint;
    }

    actions = cJSON_GetObjectItem(req, "actions");
    if (actions != NULL)
    {
        cJSON *a;

        if (!cJSON_IsArray(actions))
        {
            return API_CreateErrorResponse(400, "actions must be an array");
        }
        cJSON_ArrayForEach(a, actions)
        {
            // One vocabulary: the step body takes exactly what
            // POST /api/player/actions takes, by calling it.
            api_response_t r = API_PostPlayerAction(a);

            if (r.status_code >= 400)
            {
                return r;
            }
            if (r.json)
            {
                cJSON_Delete(r.json);
            }
        }
    }

    tics_remaining = tics;
    response_owed = true;
    return (api_response_t) { 0, NULL };
}

// Once per tic, before the tic runs.
void API_Agent_PerTic(void)
{
    DeriveEvents();
    if (players[consoleplayer].mo != NULL && gamestate == GS_LEVEL)
    {
        API_RouteMarkVisited(players[consoleplayer].mo);
    }

    if (awaiting_level)
    {
        if (gamestate == GS_LEVEL && gameaction == ga_nothing)
        {
            awaiting_level = false;
            episode_start_tic = gametic;
            if (have_pending_seed)
            {
                // G_InitNew has already cleared these; move them to where the
                // caller asked so two episodes with different seeds diverge.
                rndindex = pending_seed & 0xff;
                prndindex = (pending_seed >> 8) & 0xff;
                have_pending_seed = false;
            }
            if (pending_start_distance >= 0 && players[consoleplayer].mo != NULL)
            {
                fixed_t sx, sy;
                mobj_t *mo = players[consoleplayer].mo;

                /* Build the field first - the placement reads it. */
                (void)API_Route(mo, &(api_route_t){ 0 });
                if (API_RouteSpotAt(pending_start_distance, (unsigned int)pending_seed,
                                    &sx, &sy))
                {
                    P_UnsetThingPosition(mo);
                    mo->x = sx;
                    mo->y = sy;
                    P_SetThingPosition(mo);
                    // ONFLOORZ is a SENTINEL for P_SpawnMobj, not a
                    // coordinate: assigning it puts the player at INT_MIN and
                    // the next tic walks off the bottom of the world. The
                    // floor has to be read from the sector actually landed in,
                    // which is only known after the relink above.
                    mo->z = mo->subsector->sector->floorheight;
                    mo->floorz = mo->z;
                    mo->ceilingz = mo->subsector->sector->ceilingheight;
                }
            }
            memset(&prev, 0, sizeof(prev));
            event_count = 0;
            events_dropped = 0;
            API_RouteForgetVisited();
            if (response_owed)
            {
                response_owed = false;
                API_SendResponse((api_response_t) { 200, BuildState() });
            }
        }
    }
    else if (tics_remaining > 0)
    {
        tics_remaining--;
        if (tics_remaining == 0 && response_owed)
        {
            response_owed = false;
            API_SendResponse((api_response_t) { 200, BuildState() });
        }
    }

    if (!lockstep)
    {
        return;
    }

    // Hand the loop to the API: nothing advances until a request asks it to.
    while (tics_remaining == 0 && !awaiting_level)
    {
        if (API_Poll(-1))
        {
            API_ServeRequest();
        }
    }
}

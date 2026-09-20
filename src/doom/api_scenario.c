//
// Small levels that each pose one problem, built fresh every episode.
// See api_scenario.h for why.
//

#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "d_player.h"
#include "g_game.h"
#include "i_system.h"
#include "../m_misc.h"
#include "info.h"
#include "p_local.h"
#include "r_state.h"
#include "w_wad.h"
#include "z_zone.h"

#include "api_mapgen.h"
#include "api_scenario.h"

// A generated level is published under a name of its own rather than into an
// ExMy slot. Nothing of the game's is displaced, an agent can be scored on
// E1M1 in the same process that trained it here, and - because the episode
// and map the engine thinks it is playing never change - the sky, the music
// and the intermission screen stay the ones the IWAD actually ships. Asking
// for an episode the IWAD has never heard of costs all three.
#define SCENARIO_LUMPNAME "SCENARIO"
#define SCENARIO_LUMPS 11

#define ALL_SKILLS 7

#define THING_PLAYER 1
#define THING_MEDIKIT 2012
#define THING_STIMPACK 2011
#define THING_GREEN_ARMOR 2018
#define THING_IMP 3001
#define THING_DEMON 3002
#define THING_ZOMBIE 3004
#define THING_SHOTGUY 9
#define THING_CACODEMON 3005
#define THING_ROCKET_LAUNCHER 2003
#define THING_ROCKET_BOX 2046
#define THING_SHOTGUN 2001
#define THING_SHELLBOX 2049
#define THING_CLIPBOX 2048

// 7 is the gentlest damaging floor DOOM has: five points every 32 tics.
#define BURNING_FLOOR 7

// W1 Exit Level. A DOOM level ends at a LINE, not at an object, and the route
// can only lead somewhere it can see a line for. A scenario whose task is to
// get somewhere therefore needs a real one: without it the route has no goal
// at all, falls back to frontier exploration for the whole episode, and the
// policy never once practises the thing every real level asks of it.
#define EXIT_LINE 52

// ---------------------------------------------------------------- the dice
//
// Map generation must not draw on the game's own randomness. M_Random is a
// cursor into a fixed table that the engine advances as it plays, so building
// a level from it would make the level depend on what happened in the last
// one.

typedef struct
{
    unsigned int s;
} rng_t;

static void rng_seed(rng_t *r, unsigned int seed)
{
    r->s = seed * 2654435761u + 1u;
    if (r->s == 0)
    {
        r->s = 0x9e3779b9u;
    }
}

static unsigned int rng_next(rng_t *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static int rng_range(rng_t *r, int lo, int hi)
{
    if (hi <= lo)
    {
        return lo;
    }
    return lo + (int)(rng_next(r) % (unsigned int)(hi - lo + 1));
}

// ------------------------------------------------------------ what to run
//
// Every scenario is geometry plus a few knobs. The rules that play out over
// the episode - medkits raining, monsters coming back, what counts as
// winning - are the same code for all of them, reading these.

typedef struct
{
    mapgen_t *m;
    rng_t rng;

    int kills_to_win;       // ends the episode when every monster is dead
    int one_hit_monsters;   // every monster spawns with one hit point
    int freeze_monsters;    // monsters stay where they were put

    // A single monster walked back and forth across the far wall, so that a
    // slow projectile has to be aimed where it is going rather than where it
    // is. DOOM has no way to ask a monster for this, so the scenario moves it
    // itself, which is what ViZDoom's script does too.
    int patrol_x1, patrol_x2, patrol_y, patrol_speed;

    int medkit_every;       // tics between medkits, 0 for none
    int medkit_cap;
    int respawn_after;      // tics between topping the monsters back up
    int monster_target;     // how many should be alive at once
    int monster_type[2];    // what to top up with
    int reinforce_every;    // tics between extra monsters, 0 for none
    int reinforce_type;

    // Where medkits and reinforcements may appear.
    int drop_x1, drop_y1, drop_x2, drop_y2;
    int drop_rooms;         // when set, drop into one of the maze's rooms
    int room_x[64], room_y[64], nrooms;
    // The maze's spanning tree, so a scenario can ask how far apart two
    // rooms are rather than only where they are.
    int maze_n;
    int link_e[64], link_n[64];
} plan_t;

typedef struct
{
    const char *name;
    void (*build)(plan_t *p);
} scenario_t;

// --------------------------------------------------------------- the maze

#define MAZE_PITCH 320
#define MAZE_ROOM 256
#define MAZE_HALFDOOR 48

// A spanning tree over an n x n grid of rooms joined by short corridors, so
// every room is reachable and exactly one route joins any two of them.
static void build_maze(plan_t *p, int n, int special, const char *floorpic,
                       const char *const *textures, int ntex, int *roomsector,
                       int exit_room)
{
    int east[8][8], north[8][8];
    int corr_h[8][8], corr_v[8][8];
    int seen[8][8];
    int stack[64][2], top = 0;
    int cx, cy;

    memset(east, 0, sizeof(east));
    memset(north, 0, sizeof(north));
    memset(seen, 0, sizeof(seen));
    for (cy = 0; cy < 8; cy++)
    {
        for (cx = 0; cx < 8; cx++)
        {
            corr_h[cy][cx] = -1;
            corr_v[cy][cx] = -1;
        }
    }

    stack[top][0] = rng_range(&p->rng, 0, n - 1);
    stack[top][1] = rng_range(&p->rng, 0, n - 1);
    seen[stack[0][1]][stack[0][0]] = 1;
    top = 1;
    while (top > 0)
    {
        int x = stack[top - 1][0], y = stack[top - 1][1];
        int cand[4][3], nc = 0, pick;

        if (x + 1 < n && !seen[y][x + 1]) { cand[nc][0] = x + 1; cand[nc][1] = y; cand[nc][2] = 0; nc++; }
        if (x - 1 >= 0 && !seen[y][x - 1]) { cand[nc][0] = x - 1; cand[nc][1] = y; cand[nc][2] = 1; nc++; }
        if (y + 1 < n && !seen[y + 1][x]) { cand[nc][0] = x; cand[nc][1] = y + 1; cand[nc][2] = 2; nc++; }
        if (y - 1 >= 0 && !seen[y - 1][x]) { cand[nc][0] = x; cand[nc][1] = y - 1; cand[nc][2] = 3; nc++; }
        if (nc == 0)
        {
            top--;
            continue;
        }
        pick = rng_range(&p->rng, 0, nc - 1);
        switch (cand[pick][2])
        {
          case 0: east[y][x] = 1; break;
          case 1: east[y][x - 1] = 1; break;
          case 2: north[y][x] = 1; break;
          default: north[y - 1][x] = 1; break;
        }
        seen[cand[pick][1]][cand[pick][0]] = 1;
        stack[top][0] = cand[pick][0];
        stack[top][1] = cand[pick][1];
        top++;
    }

    // Rooms first, then the corridors, because a room's doorway has to name
    // the sector on the far side of it.
    p->nrooms = 0;
    p->maze_n = n;
    for (cy = 0; cy < n; cy++)
    {
        for (cx = 0; cx < n; cx++)
        {
            p->link_e[cy * n + cx] = east[cy][cx];
            p->link_n[cy * n + cx] = north[cy][cx];
        }
    }
    for (cy = 0; cy < n; cy++)
    {
        for (cx = 0; cx < n; cx++)
        {
            const char *pic = floorpic;
            roomsector[cy * n + cx] =
                MapGen_AddSector(p->m, 0, 128, pic, "CEIL3_5", 176, special, 0);
            p->room_x[p->nrooms] = cx * MAZE_PITCH + MAZE_ROOM / 2;
            p->room_y[p->nrooms] = cy * MAZE_PITCH + MAZE_ROOM / 2;
            p->nrooms++;
        }
    }
    for (cy = 0; cy < n; cy++)
    {
        for (cx = 0; cx < n; cx++)
        {
            if (east[cy][cx])
            {
                corr_h[cy][cx] = MapGen_AddSector(p->m, 0, 128, floorpic,
                                                  "CEIL3_5", 144, special, 0);
            }
            if (north[cy][cx])
            {
                corr_v[cy][cx] = MapGen_AddSector(p->m, 0, 128, floorpic,
                                                  "CEIL3_5", 144, special, 0);
            }
        }
    }

    for (cy = 0; cy < n; cy++)
    {
        for (cx = 0; cx < n; cx++)
        {
            int x1 = cx * MAZE_PITCH, y1 = cy * MAZE_PITCH;
            int x2 = x1 + MAZE_ROOM, y2 = y1 + MAZE_ROOM;
            int mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
            int s = roomsector[cy * n + cx];
            const char *tex = textures[(cy * n + cx) % ntex];
            int back;

            // West, running north, room on the right.
            back = cx > 0 ? corr_h[cy][cx - 1] : -1;
            if (back >= 0)
            {
                MapGen_AddWall(p->m, x1, y1, x1, my - MAZE_HALFDOOR, s, tex);
                MapGen_AddPortal(p->m, x1, my - MAZE_HALFDOOR, x1, my + MAZE_HALFDOOR,
                                 s, back, NULL, NULL, NULL);
                if (cy * n + cx == exit_room)
                {
                    MapGen_SetLineSpecial(p->m, EXIT_LINE, 0);
                }
                MapGen_AddWall(p->m, x1, my + MAZE_HALFDOOR, x1, y2, s, tex);
            }
            else
            {
                MapGen_AddWall(p->m, x1, y1, x1, y2, s, tex);
            }

            // North, running east.
            back = corr_v[cy][cx];
            if (back >= 0)
            {
                MapGen_AddWall(p->m, x1, y2, mx - MAZE_HALFDOOR, y2, s, tex);
                MapGen_AddPortal(p->m, mx - MAZE_HALFDOOR, y2, mx + MAZE_HALFDOOR, y2,
                                 s, back, NULL, NULL, NULL);
                if (cy * n + cx == exit_room)
                {
                    MapGen_SetLineSpecial(p->m, EXIT_LINE, 0);
                }
                MapGen_AddWall(p->m, mx + MAZE_HALFDOOR, y2, x2, y2, s, tex);
            }
            else
            {
                MapGen_AddWall(p->m, x1, y2, x2, y2, s, tex);
            }

            // East, running south.
            back = corr_h[cy][cx];
            if (back >= 0)
            {
                MapGen_AddWall(p->m, x2, y2, x2, my + MAZE_HALFDOOR, s, tex);
                MapGen_AddPortal(p->m, x2, my + MAZE_HALFDOOR, x2, my - MAZE_HALFDOOR,
                                 s, back, NULL, NULL, NULL);
                if (cy * n + cx == exit_room)
                {
                    MapGen_SetLineSpecial(p->m, EXIT_LINE, 0);
                }
                MapGen_AddWall(p->m, x2, my - MAZE_HALFDOOR, x2, y1, s, tex);
            }
            else
            {
                MapGen_AddWall(p->m, x2, y2, x2, y1, s, tex);
            }

            // South, running west.
            back = cy > 0 ? corr_v[cy - 1][cx] : -1;
            if (back >= 0)
            {
                MapGen_AddWall(p->m, x2, y1, mx + MAZE_HALFDOOR, y1, s, tex);
                MapGen_AddPortal(p->m, mx + MAZE_HALFDOOR, y1, mx - MAZE_HALFDOOR, y1,
                                 s, back, NULL, NULL, NULL);
                if (cy * n + cx == exit_room)
                {
                    MapGen_SetLineSpecial(p->m, EXIT_LINE, 0);
                }
                MapGen_AddWall(p->m, mx - MAZE_HALFDOOR, y1, x1, y1, s, tex);
            }
            else
            {
                MapGen_AddWall(p->m, x2, y1, x1, y1, s, tex);
            }
        }
    }

    // The corridors' long sides. Their ends are the doorways above.
    for (cy = 0; cy < n; cy++)
    {
        for (cx = 0; cx < n; cx++)
        {
            int my = cy * MAZE_PITCH + MAZE_ROOM / 2;
            int mx = cx * MAZE_PITCH + MAZE_ROOM / 2;

            if (corr_h[cy][cx] >= 0)
            {
                int xa = cx * MAZE_PITCH + MAZE_ROOM;
                int xb = (cx + 1) * MAZE_PITCH;
                int s = corr_h[cy][cx];

                MapGen_AddWall(p->m, xa, my + MAZE_HALFDOOR, xb, my + MAZE_HALFDOOR, s, "GRAY7");
                MapGen_AddWall(p->m, xb, my - MAZE_HALFDOOR, xa, my - MAZE_HALFDOOR, s, "GRAY7");
            }
            if (corr_v[cy][cx] >= 0)
            {
                int ya = cy * MAZE_PITCH + MAZE_ROOM;
                int yb = (cy + 1) * MAZE_PITCH;
                int s = corr_v[cy][cx];

                MapGen_AddWall(p->m, mx - MAZE_HALFDOOR, ya, mx - MAZE_HALFDOOR, yb, s, "GRAY7");
                MapGen_AddWall(p->m, mx + MAZE_HALFDOOR, yb, mx + MAZE_HALFDOOR, ya, s, "GRAY7");
            }
        }
    }
}

// ---------------------------------------------------------- the scenarios

// Kill the one monster in the room. The smallest thing that is still a game:
// see something, face it, shoot it.
static void build_basic(plan_t *p)
{
    int s = MapGen_AddSector(p->m, 0, 128, "FLOOR4_8", "CEIL3_5", 192, 0, 0);

    MapGen_AddRoom(p->m, 0, 0, 1024, 704, s, "STARTAN3");
    MapGen_AddThing(p->m, 512, 96, 90, THING_PLAYER, ALL_SKILLS);
    MapGen_AddThing(p->m, rng_range(&p->rng, 128, 896), 608, 270,
                    THING_IMP, ALL_SKILLS);
    MapGen_AddThing(p->m, 512, 96, 0, THING_CLIPBOX, ALL_SKILLS);
    p->kills_to_win = 1;
    p->one_hit_monsters = 1;
    p->freeze_monsters = 1;
}

// Run the length of a corridor to the armour at the end without being shot
// off it. Advancing and fighting pull against each other, and the guards
// cannot come to you: they stand where they were put and shoot down the
// corridor, so every step forward is taken under fire.
static void build_deadly_corridor(plan_t *p)
{
    int s = MapGen_AddSector(p->m, 0, 128, "FLOOR0_1", "CEIL3_5", 144, 0, 0);
    int k;

    // The corridor, with its last stretch cut off as a sector of its own so
    // that the line between them can be the exit. Without a real exit line
    // the route has nothing to lead to and the whole run is frontier
    // exploration, which is not what the corridor is for.
    {
        int end = MapGen_AddSector(p->m, 0, 128, "FLOOR0_1", "CEIL3_5", 192, 0, 0);

        MapGen_AddWall(p->m, 0, 0, 0, 2176, s, "BROWN1");
        MapGen_AddPortal(p->m, 0, 2176, 320, 2176, s, end, NULL, NULL, NULL);
        MapGen_SetLineSpecial(p->m, EXIT_LINE, 0);
        MapGen_AddWall(p->m, 320, 2176, 320, 0, s, "BROWN1");
        MapGen_AddWall(p->m, 320, 0, 0, 0, s, "BROWN1");

        MapGen_AddWall(p->m, 0, 2176, 0, 2304, end, "BROWN1");
        MapGen_AddWall(p->m, 0, 2304, 320, 2304, end, "BROWN1");
        MapGen_AddWall(p->m, 320, 2304, 320, 2176, end, "BROWN1");
    }

    // Three down each side, staggered, facing back down the corridor. Where
    // exactly is drawn each episode: the task is the corridor, not this one
    // arrangement of it.
    for (k = 0; k < 3; k++)
    {
        MapGen_AddThing(p->m, 64, 512 + k * 576 + rng_range(&p->rng, -128, 128),
                        270, THING_SHOTGUY, ALL_SKILLS);
        MapGen_AddThing(p->m, 256, 800 + k * 576 + rng_range(&p->rng, -128, 128),
                        270, THING_SHOTGUY, ALL_SKILLS);
    }
    MapGen_AddThing(p->m, 160, 96, 90, THING_PLAYER, ALL_SKILLS);
    MapGen_AddThing(p->m, 160, 96, 0, THING_SHOTGUN, ALL_SKILLS);
    MapGen_AddThing(p->m, 160, 160, 0, THING_SHELLBOX, ALL_SKILLS);
    MapGen_AddThing(p->m, 160, 2240, 0, THING_GREEN_ARMOR, ALL_SKILLS);
    p->one_hit_monsters = 1;
    p->freeze_monsters = 1;
}

// The armour in each of these two is the exit: the map has no exit line, so
// the route cannot be handed the answer and the agent has to look for it.
static void walls_of_a_ring(plan_t *p, int s, int cx, int cy, int radius,
                            const char *tex)
{
    // Eighteen sides. Convex, so the node builder never has to cut it.
    static const int cos18[18] = {
        1000, 940, 766, 500, 174, -174, -500, -766, -940, -1000,
        -940, -766, -500, -174, 174, 500, 766, 940
    };
    static const int sin18[18] = {
        0, 342, 643, 866, 985, 985, 866, 643, 342, 0,
        -342, -643, -866, -985, -985, -866, -643, -342
    };
    int i;

    // Clockwise, so the room is on each wall's right.
    for (i = 0; i < 18; i++)
    {
        int j = (i + 17) % 18;
        int ax = cx + cos18[i] * radius / 1000, ay = cy + sin18[i] * radius / 1000;
        int bx = cx + cos18[j] * radius / 1000, by = cy + sin18[j] * radius / 1000;

        MapGen_AddWall(p->m, ax, ay, bx, by, s, tex);
    }
}

// Stand in the middle of a ring with monsters closing from every side and
// limited ammunition. Killing is good; being killed is not.
static void build_defend_the_center(plan_t *p)
{
    int s = MapGen_AddSector(p->m, 0, 160, "FLOOR4_8", "CEIL3_5", 160, 0, 0);
    int i;

    walls_of_a_ring(p, s, 640, 640, 512, "GRAY5");
    MapGen_AddThing(p->m, 640, 640, rng_range(&p->rng, 0, 7) * 45,
                    THING_PLAYER, ALL_SKILLS);
    MapGen_AddThing(p->m, 640, 640, 0, THING_SHOTGUN, ALL_SKILLS);
    for (i = 0; i < 5; i++)
    {
        // Five, spread around the wall, each nudged off its mark so that no
        // two episodes present the same ring.
        static const int cx[5] = { 400, 124, -324, -324, 124 };
        static const int cy[5] = { 0, 380, 235, -235, -380 };

        MapGen_AddThing(p->m, 640 + cx[i] + rng_range(&p->rng, -48, 48),
                        640 + cy[i] + rng_range(&p->rng, -48, 48), 0,
                        THING_DEMON, ALL_SKILLS);
    }
    p->one_hit_monsters = 1;
    p->respawn_after = 245;
    p->monster_target = 5;
    p->monster_type[0] = THING_DEMON;
    p->monster_type[1] = THING_DEMON;
    p->drop_x1 = 640 - 380;
    p->drop_y1 = 640 - 380;
    p->drop_x2 = 640 + 380;
    p->drop_y2 = 640 + 380;
}

// The same lesson with a wall at your back and monsters that shoot.
static void build_defend_the_line(plan_t *p)
{
    int s = MapGen_AddSector(p->m, 0, 128, "FLOOR4_8", "CEIL3_5", 176, 0, 0);
    int i;

    MapGen_AddRoom(p->m, 0, 0, 1280, 896, s, "STARTAN2");
    MapGen_AddThing(p->m, 640, 96, 90, THING_PLAYER, ALL_SKILLS);
    MapGen_AddThing(p->m, 640, 96, 0, THING_SHOTGUN, ALL_SKILLS);
    for (i = 0; i < 3; i++)
    {
        MapGen_AddThing(p->m, 256 + i * 384 + rng_range(&p->rng, -96, 96),
                        768, 270, THING_DEMON, ALL_SKILLS);
        MapGen_AddThing(p->m, 448 + i * 384 + rng_range(&p->rng, -96, 96),
                        768, 270, THING_SHOTGUY, ALL_SKILLS);
    }
    p->one_hit_monsters = 1;
    p->respawn_after = 245;
    p->monster_target = 6;
    p->monster_type[0] = THING_DEMON;
    p->monster_type[1] = THING_SHOTGUY;
    p->drop_x1 = 128;
    p->drop_y1 = 640;
    p->drop_x2 = 1152;
    p->drop_y2 = 832;
}

// A floor that burns, and medkits scattered over it. Nothing says the floor
// is the problem: the agent has to work out what is keeping it alive.
static void build_health_gathering(plan_t *p)
{
    int s = MapGen_AddSector(p->m, 0, 128, "NUKAGE1", "CEIL3_5", 160,
                             BURNING_FLOOR, 0);
    int i;

    MapGen_AddRoom(p->m, 0, 0, 1216, 1216, s, "SLADWALL");
    MapGen_AddThing(p->m, 608, 608, rng_range(&p->rng, 0, 7) * 45,
                    THING_PLAYER, ALL_SKILLS);
    for (i = 0; i < 16; i++)
    {
        MapGen_AddThing(p->m, rng_range(&p->rng, 64, 1152),
                        rng_range(&p->rng, 64, 1152), 0,
                        THING_MEDIKIT, ALL_SKILLS);
    }
    p->medkit_every = 30;
    p->medkit_cap = 24;
    p->drop_x1 = 64;
    p->drop_y1 = 64;
    p->drop_x2 = 1152;
    p->drop_y2 = 1152;
}

// The same, except the medkits are out of sight in a maze, so surviving means
// navigating as well as noticing.
static void build_health_gathering_supreme(plan_t *p)
{
    static const char *const tex[1] = { "SLADWALL" };
    int room[64];
    int i;

    build_maze(p, 4, BURNING_FLOOR, "NUKAGE1", tex, 1, room, -1);
    i = rng_range(&p->rng, 0, p->nrooms - 1);
    MapGen_AddThing(p->m, p->room_x[i], p->room_y[i],
                    rng_range(&p->rng, 0, 7) * 45, THING_PLAYER, ALL_SKILLS);
    for (i = 0; i < 12; i++)
    {
        int r = rng_range(&p->rng, 0, p->nrooms - 1);

        MapGen_AddThing(p->m, p->room_x[r] + rng_range(&p->rng, -80, 80),
                        p->room_y[r] + rng_range(&p->rng, -80, 80), 0,
                        THING_MEDIKIT, ALL_SKILLS);
    }
    p->medkit_every = 35;
    p->medkit_cap = 20;
    p->drop_rooms = 1;
}

// How many rooms apart each room is from `from`, over the maze's own tree.
static void maze_distances(const plan_t *p, int from, int *dist)
{
    int queue[64], head = 0, tail = 0, i;
    int n = p->maze_n;

    for (i = 0; i < n * n; i++)
    {
        dist[i] = -1;
    }
    dist[from] = 0;
    queue[tail++] = from;
    while (head < tail)
    {
        int c = queue[head++];
        int cx = c % n, cy = c / n;
        int k;

        for (k = 0; k < 4; k++)
        {
            int to = -1;

            if (k == 0 && cx + 1 < n && p->link_e[c]) to = c + 1;
            if (k == 1 && cx > 0 && p->link_e[c - 1]) to = c - 1;
            if (k == 2 && cy + 1 < n && p->link_n[c]) to = c + n;
            if (k == 3 && cy > 0 && p->link_n[c - n]) to = c - n;
            if (to >= 0 && dist[to] < 0)
            {
                dist[to] = dist[c] + 1;
                queue[tail++] = to;
            }
        }
    }
}

// Dropped somewhere in a maze facing somewhere, find the armour. This is the
// one that asks the question the real levels ask and cannot answer: a fresh
// maze every episode, so there is nothing to memorise.
static void build_my_way_home(plan_t *p)
{
    static const char *const tex[16] = {
        "BROWN1", "GRAY4", "STARTAN3", "REDWALL1", "COMPUTE1", "STONE2",
        "LITEBLU1", "SLADWALL", "METAL1", "TEKWALL1", "BROWNGRN", "GRAY7",
        "STARG3", "BROWN96", "COMPSPAN", "STONE"
    };
    int room[64];
    int home, start;

    int dist[64], far[64], nfar = 0, best = 0, i;

    // Which room is home has to be settled before the maze is built, because
    // it is that room's DOORWAYS that end the level - crossing one is what
    // "got there" means, and a level ends at a line. The maze's shape does
    // not depend on which room is picked, so nothing is lost by choosing
    // first.
    home = rng_range(&p->rng, 0, 15);
    build_maze(p, 4, 0, "FLOOR4_8", tex, 16, room, home);

    // Not next door. A start drawn uniformly lands beside the armour often
    // enough to finish some episodes in three decisions, and an episode that
    // is over before a decision matters teaches nothing. Start from the far
    // half of the maze, measured in rooms walked rather than in units.
    maze_distances(p, home, dist);
    for (i = 0; i < p->nrooms; i++)
    {
        if (dist[i] > best)
        {
            best = dist[i];
        }
    }
    for (i = 0; i < p->nrooms; i++)
    {
        if (dist[i] >= (best + 1) / 2 && dist[i] > 0)
        {
            far[nfar++] = i;
        }
    }
    start = nfar > 0 ? far[rng_range(&p->rng, 0, nfar - 1)]
                     : (home + 1) % p->nrooms;

    MapGen_AddThing(p->m, p->room_x[start], p->room_y[start],
                    rng_range(&p->rng, 0, 7) * 45, THING_PLAYER, ALL_SKILLS);
    // Still there, and still the thing that makes the room recognisable from
    // the doorway - but the level now ends at the doorway itself rather than
    // on picking it up, which is how a DOOM level ends and therefore what the
    // route can lead to.
    MapGen_AddThing(p->m, p->room_x[home], p->room_y[home], 0,
                    THING_GREEN_ARMOR, ALL_SKILLS);
}

// A rocket launcher, one target far enough away that the rocket takes time to
// arrive, and nothing else in the room.
static void build_predict_position(plan_t *p)
{
    int s = MapGen_AddSector(p->m, 0, 192, "FLOOR4_8", "CEIL3_5", 176, 0, 0);

    MapGen_AddRoom(p->m, 0, 0, 1408, 1408, s, "COMPUTE1");
    MapGen_AddThing(p->m, 704, 128, 90, THING_PLAYER, ALL_SKILLS);
    MapGen_AddThing(p->m, 704, 128, 0, THING_ROCKET_LAUNCHER, ALL_SKILLS);
    MapGen_AddThing(p->m, 704, 192, 0, THING_ROCKET_BOX, ALL_SKILLS);
    MapGen_AddThing(p->m, rng_range(&p->rng, 320, 1088), 1280, 270,
                    THING_IMP, ALL_SKILLS);
    p->kills_to_win = 1;
    p->one_hit_monsters = 1;
    p->patrol_x1 = 192;
    p->patrol_x2 = 1216;
    p->patrol_y = 1280;
    p->patrol_speed = rng_range(&p->rng, 0, 1) ? 6 : -6;
}

// Fireballs come from across the room and more of them keep coming. There is
// no winning, only lasting.
static void build_take_cover(plan_t *p)
{
    int s = MapGen_AddSector(p->m, 0, 128, "FLOOR0_1", "CEIL3_5", 176, 0, 0);
    int i;

    MapGen_AddRoom(p->m, 0, 0, 1280, 1024, s, "BROWN1");
    MapGen_AddThing(p->m, 640, 128, 90, THING_PLAYER, ALL_SKILLS);
    for (i = 0; i < 3; i++)
    {
        MapGen_AddThing(p->m, 256 + i * 384 + rng_range(&p->rng, -96, 96),
                        896, 270, THING_IMP, ALL_SKILLS);
    }
    p->reinforce_every = 420;
    p->reinforce_type = THING_IMP;
    p->drop_x1 = 128;
    p->drop_y1 = 832;
    p->drop_x2 = 1152;
    p->drop_y2 = 960;
}

static const scenario_t scenarios[] = {
    { "basic", build_basic },
    { "deadly-corridor", build_deadly_corridor },
    { "defend-the-center", build_defend_the_center },
    { "defend-the-line", build_defend_the_line },
    { "health-gathering", build_health_gathering },
    { "health-gathering-supreme", build_health_gathering_supreme },
    { "my-way-home", build_my_way_home },
    { "predict-position", build_predict_position },
    { "take-cover", build_take_cover },
};

#define NUM_SCENARIOS ((int)(sizeof(scenarios) / sizeof(scenarios[0])))

int Scenario_Count(void)
{
    return NUM_SCENARIOS;
}

const char *Scenario_Name(int index)
{
    if (index < 0 || index >= NUM_SCENARIOS)
    {
        return NULL;
    }
    return scenarios[index].name;
}

// ------------------------------------------------------- publishing a map
//
// The generated PWAD is handed to the engine through the same door an
// authored one comes in by: entries in the lump directory, backed by a
// wad_file_t that reads out of memory instead of off disk.

static byte *live_image;
static byte *pending_image;
static wad_file_t live_wad;
static wad_file_t pending_wad;
static lumpindex_t first_lump = -1;

static size_t mem_read(wad_file_t *file, unsigned int offset,
                       void *buffer, size_t buffer_len)
{
    if (file->mapped == NULL || offset >= file->length)
    {
        return 0;
    }
    if (offset + buffer_len > file->length)
    {
        buffer_len = file->length - offset;
    }
    memcpy(buffer, file->mapped + offset, buffer_len);
    return buffer_len;
}

static void mem_close(wad_file_t *file)
{
    file->mapped = NULL;
}

static wad_file_class_t mem_wad_class = { NULL, mem_close, mem_read };

static const char *const lump_names[SCENARIO_LUMPS] = {
    SCENARIO_LUMPNAME, "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES",
    "SEGS", "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP"
};

static void point_lumps_at(wad_file_t *wad, const byte *dir)
{
    int i;

    for (i = 0; i < SCENARIO_LUMPS; i++)
    {
        lumpinfo_t *l = lumpinfo[first_lump + i];
        int filepos, size;

        memcpy(&filepos, dir + i * 16, 4);
        memcpy(&size, dir + i * 16 + 4, 4);
        l->wad_file = wad;
        l->position = filepos;
        l->size = size;
        l->cache = NULL;
    }
}

static void publish(byte *img, size_t len)
{
    int infotableofs;
    int i;

    memcpy(&infotableofs, img + 8, 4);
    pending_wad.file_class = &mem_wad_class;
    pending_wad.mapped = img;
    pending_wad.length = (unsigned int)len;

    if (first_lump < 0)
    {
        first_lump = (lumpindex_t)numlumps;
        numlumps += SCENARIO_LUMPS;
        lumpinfo = realloc(lumpinfo, numlumps * sizeof(lumpinfo_t *));
        if (lumpinfo == NULL)
        {
            I_Error("Scenario: no room in the lump directory");
        }
        for (i = 0; i < SCENARIO_LUMPS; i++)
        {
            lumpinfo_t *l = calloc(1, sizeof(lumpinfo_t));

            if (l == NULL)
            {
                I_Error("Scenario: no room for a lump");
            }
            memcpy(l->name, lump_names[i],
                   strlen(lump_names[i]) < 8 ? strlen(lump_names[i]) : 8);
            lumpinfo[first_lump + i] = l;
        }
        // The name lookup was hashed at startup and has never heard of these.
        W_GenerateHashTable();
    }
    point_lumps_at(&pending_wad, img + infotableofs);
}

// ---------------------------------------------------------- running a plan

static plan_t plan;
static boolean active;
static char current[40];
static int medkits_out;
static int next_medkit;
static int next_reinforcement;
static int next_respawn;
static int respawn_turn;
static mobj_t *pinned[64];
static int pinned_x[64], pinned_y[64];
static int npinned;
static int patrol_dir;

boolean Scenario_Active(void)
{
    return active;
}

const char *Scenario_Current(void)
{
    return active ? current : NULL;
}

void Scenario_Clear(void)
{
    active = false;
    current[0] = 0;
}

boolean Scenario_Select(const char *name, unsigned int seed,
                        int *episode, int *map)
{
    const scenario_t *sc = NULL;
    mapgen_t *m;
    byte *img;
    size_t len = 0;
    const char *why = NULL;
    int i;

    for (i = 0; i < NUM_SCENARIOS; i++)
    {
        if (strcmp(scenarios[i].name, name) == 0)
        {
            sc = &scenarios[i];
            break;
        }
    }
    if (sc == NULL)
    {
        return false;
    }

    m = MapGen_New();
    if (m == NULL)
    {
        I_Error("Scenario: no room to build %s", name);
    }
    memset(&plan, 0, sizeof(plan));
    plan.m = m;
    rng_seed(&plan.rng, seed);
    sc->build(&plan);

    img = MapGen_Build(m, SCENARIO_LUMPNAME, &len, &why);
    if (img == NULL)
    {
        MapGen_Free(m);
        I_Error("Scenario %s: %s", name, why ? why : "would not build");
    }
    MapGen_Free(m);
    plan.m = NULL;

    // The level standing right now was loaded out of the last image, and
    // p_setup.c keeps a pointer into it for the reject table. It is freed
    // when the new level replaces it, not here.
    free(pending_image);
    pending_image = img;
    publish(img, len);

    M_StringCopy(current, name, sizeof(current));
    active = true;

    // Map 1 of episode 1, whatever was being played before: those two decide
    // the sky, the music, the intermission art and - at map 8 - whether
    // finishing ends the game. A generated level wants none of that to move.
    *episode = 1;
    *map = 1;
    return true;
}

const char *Scenario_LumpName(void)
{
    return SCENARIO_LUMPNAME;
}

// --------------------------------------------------------------- the rules

static int monsters_alive(void)
{
    thinker_t *th;
    int n = 0;

    for (th = thinkercap.next; th != &thinkercap && th != NULL; th = th->next)
    {
        mobj_t *mo;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
        {
            continue;
        }
        mo = (mobj_t *)th;
        if ((mo->flags & MF_COUNTKILL) && mo->health > 0)
        {
            n++;
        }
    }
    return n;
}

static int count_of(mobjtype_t type)
{
    thinker_t *th;
    int n = 0;

    for (th = thinkercap.next; th != &thinkercap && th != NULL; th = th->next)
    {
        mobj_t *mo;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
        {
            continue;
        }
        mo = (mobj_t *)th;
        if (mo->type == type)
        {
            n++;
        }
    }
    return n;
}

static void drop_spot(int *x, int *y)
{
    if (plan.drop_rooms && plan.nrooms > 0)
    {
        int r = rng_range(&plan.rng, 0, plan.nrooms - 1);

        *x = plan.room_x[r] + rng_range(&plan.rng, -80, 80);
        *y = plan.room_y[r] + rng_range(&plan.rng, -80, 80);
        return;
    }
    *x = rng_range(&plan.rng, plan.drop_x1, plan.drop_x2);
    *y = rng_range(&plan.rng, plan.drop_y1, plan.drop_y2);
}

static void spawn(int x, int y, int type, int angle)
{
    mapthing_t mt;

    memset(&mt, 0, sizeof(mt));
    mt.x = (short)x;
    mt.y = (short)y;
    mt.angle = (short)angle;
    mt.type = (short)type;
    mt.options = ALL_SKILLS;
    P_SpawnMapThing(&mt);
}

void Scenario_LevelLoaded(void)
{
    if (!active)
    {
        return;
    }

    // The level now standing came out of the pending image, so the one it
    // replaced can go. Nothing points into it any more.
    if (pending_image != NULL)
    {
        int infotableofs;

        free(live_image);
        live_image = pending_image;
        pending_image = NULL;
        live_wad = pending_wad;
        live_wad.mapped = live_image;
        memcpy(&infotableofs, live_image + 8, 4);
        point_lumps_at(&live_wad, live_image + infotableofs);
    }

    medkits_out = count_of(MT_MISC11);
    next_medkit = plan.medkit_every;
    next_reinforcement = plan.reinforce_every;
    next_respawn = plan.respawn_after;
    respawn_turn = 0;
    npinned = 0;
    patrol_dir = plan.patrol_speed;

    if (plan.one_hit_monsters || plan.freeze_monsters)
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
            if (!(mo->flags & MF_COUNTKILL))
            {
                continue;
            }
            if (plan.one_hit_monsters)
            {
                mo->health = 1;
            }
            if (plan.freeze_monsters
                && npinned < (int)(sizeof(pinned) / sizeof(pinned[0])))
            {
                pinned[npinned] = mo;
                pinned_x[npinned] = mo->x;
                pinned_y[npinned] = mo->y;
                npinned++;
            }
        }
    }
}

// Monsters that were told to stay put. DOOM keeps a movement speed per TYPE,
// not per monster, so there is nothing to set to zero without changing every
// monster of that kind in the game. Putting them back where they were each
// tic is the same thing from the outside, and it is what ViZDoom's script
// does by another name.
static void hold_the_pinned(void)
{
    thinker_t *th;

    for (th = thinkercap.next; th != &thinkercap && th != NULL; th = th->next)
    {
        mobj_t *mo;
        int i;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
        {
            continue;
        }
        mo = (mobj_t *)th;
        if (mo->health <= 0)
        {
            continue;
        }
        for (i = 0; i < npinned; i++)
        {
            if (pinned[i] != mo)
            {
                continue;
            }
            mo->momx = 0;
            mo->momy = 0;
            if (mo->x != pinned_x[i] || mo->y != pinned_y[i])
            {
                P_UnsetThingPosition(mo);
                mo->x = pinned_x[i];
                mo->y = pinned_y[i];
                P_SetThingPosition(mo);
            }
            break;
        }
    }
}

static void walk_the_patrol(void)
{
    thinker_t *th;

    for (th = thinkercap.next; th != &thinkercap && th != NULL; th = th->next)
    {
        mobj_t *mo;
        int x;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
        {
            continue;
        }
        mo = (mobj_t *)th;
        if (!(mo->flags & MF_COUNTKILL) || mo->health <= 0)
        {
            continue;
        }
        x = (mo->x >> FRACBITS) + patrol_dir;
        if (x <= plan.patrol_x1 || x >= plan.patrol_x2)
        {
            patrol_dir = -patrol_dir;
            x = (mo->x >> FRACBITS) + patrol_dir;
        }
        mo->momx = 0;
        mo->momy = 0;
        P_UnsetThingPosition(mo);
        mo->x = x << FRACBITS;
        mo->y = plan.patrol_y << FRACBITS;
        P_SetThingPosition(mo);
        return;
    }
}

void Scenario_PerTic(void)
{
    if (!active || gameaction != ga_nothing)
    {
        return;
    }

    if (npinned > 0)
    {
        hold_the_pinned();
    }
    if (plan.patrol_speed != 0)
    {
        walk_the_patrol();
    }

    if (plan.medkit_every > 0 && --next_medkit <= 0)
    {
        next_medkit = plan.medkit_every;
        if (count_of(MT_MISC11) < plan.medkit_cap)
        {
            int x, y;

            drop_spot(&x, &y);
            spawn(x, y, THING_MEDIKIT, 0);
            medkits_out++;
        }
    }

    if (plan.reinforce_every > 0 && --next_reinforcement <= 0)
    {
        int x, y;

        next_reinforcement = plan.reinforce_every;
        drop_spot(&x, &y);
        spawn(x, y, plan.reinforce_type, 270);
    }

    if (plan.respawn_after > 0 && --next_respawn <= 0)
    {
        // The pressure never lets up: whatever dies is replaced, so what runs
        // out is ammunition rather than monsters.
        next_respawn = plan.respawn_after;
        if (monsters_alive() < plan.monster_target)
        {
            int x, y;

            drop_spot(&x, &y);
            spawn(x, y, plan.monster_type[respawn_turn++ & 1], 0);
        }
    }

    if (plan.kills_to_win > 0 && leveltime > 4 && monsters_alive() == 0)
    {
        G_ExitLevel();
        return;
    }
}

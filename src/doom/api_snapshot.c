// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Martin Schröder <info@swedishembedded.com>
//
// See api_snapshot.h for why replaying a prefix is not good enough.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_snapshot.h"
#include "api_cJSON.h"
#include "api_route.h"
#include "api_agent.h"

#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"
#include "p_saveg.h"
#include "r_state.h"
#include "m_random.h"
#include "p_local.h"

// One held state. `stream` is a `tmpfile`, which is the portable way to get a
// `FILE *` the savegame writer can use without inventing a second one: the
// archive functions in p_saveg.c write through `save_stream` and nothing
// else, so the whole of vanilla's save path works unchanged against it. The
// file never reaches a name, is deleted when closed, and in practice never
// leaves the page cache.
// A held state lives in MEMORY, not in an open file.
//
// The savegame writer needs a `FILE *`, so a snapshot is still written
// through `tmpfile` - but the bytes are read straight back out and the handle
// closed, and a restore hands them to `fmemopen`. That matters because a
// search that returns to promising states wants THOUSANDS of them, and a
// thousand open file handles is a thousand file descriptors against a limit
// that is usually 1024. A DOOM savegame for this episode is tens of
// kilobytes, so the whole archive is a few hundred megabytes of heap.
typedef struct
{
    char *data;
    size_t len;
    boolean held;
} api_snapshot_t;

static api_snapshot_t slots[API_SNAPSHOT_SLOTS];

static void discard(int i)
{
    if (slots[i].data != NULL)
    {
        free(slots[i].data);
    }
    slots[i].data = NULL;
    slots[i].len = 0;
    slots[i].held = false;
}

void API_SnapshotDiscardAll(void)
{
    int i;

    for (i = 0; i < API_SNAPSHOT_SLOTS; i++)
    {
        discard(i);
    }
}


// Everything the vanilla savegame does NOT carry, and a counterfactual needs.
//
// The savegame format was built to resume a game a human will keep playing,
// not to continue a simulation. Three things it drops are fatal to a probe
// that goes back to a decision and tries a different one:
//
//   * `G_InitNew`, which a restore has to call to stand the level back up,
//     calls `M_ClearRandom`. Every restore therefore re-seeds the gameplay
//     RNG, so the same action from the "same" state plays out differently.
//   * `P_UnArchiveThinkers` sets every mobj's `target` and `tracer` to NULL,
//     so on restore no monster remembers what it was chasing. A room of
//     monsters mid-fight comes back idle.
//   * `P_UnArchiveSectors` clears `soundtarget`, so they forget they heard
//     anything either.
//
// The `id` a mobj carries for the API is not saved at all, so the client's
// own memory of which monster it has been shooting breaks across a restore
// too.
//
// This block is appended AFTER the vanilla EOF, so nothing about the standard
// format changes and a normal savegame still loads. Targets are written as
// the INDEX of the thinker in archive order rather than as an id, because the
// order is what the vanilla archive itself relies on and it needs no ids to
// be stable.
static int mobj_index(mobj_t *want)
{
    thinker_t *th;
    int i = 0;

    if (want == NULL)
    {
        return -1;
    }
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker)
        {
            if ((mobj_t *)th == want)
            {
                return i;
            }
            i++;
        }
    }
    return -1;
}

static void write_extras(FILE *f)
{
    thinker_t *th;
    int i, n = 0;

    fwrite(&rndindex, sizeof(int), 1, f);
    fwrite(&prndindex, sizeof(int), 1, f);

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker)
        {
            n++;
        }
    }
    fwrite(&n, sizeof(int), 1, f);
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker)
        {
            mobj_t *mo = (mobj_t *)th;
            int t = mobj_index(mo->target);
            int r = mobj_index(mo->tracer);

            fwrite(&mo->id, sizeof(int), 1, f);
            fwrite(&t, sizeof(int), 1, f);
            fwrite(&r, sizeof(int), 1, f);
        }
    }
    fwrite(&numsectors, sizeof(int), 1, f);
    for (i = 0; i < numsectors; i++)
    {
        int t = mobj_index(sectors[i].soundtarget);

        fwrite(&t, sizeof(int), 1, f);
    }

    /* The API's own pending INPUT.
     *
     * `keys_down` is how many more tics each key is held for and
     * `target_angle` is the turn servo's target; both deliberately carry
     * across step calls, which is what lets one decision say "walk forward
     * for eight tics" and mean it. They are therefore part of the state a
     * decision is made from, and leaving them out of a snapshot meant a
     * restore put the world back and left the CONTROLLER mid-press - so the
     * first action after a return did something else.
     *
     * Measured: from a mid-sequence snapshot, replaying the same three
     * actions continuously and after a restore ended 347 001 fixed-point
     * units apart and facing 9 degrees differently, with every field the
     * savegame carries identical at the moment of the restore. */
    fwrite(&target_angle, sizeof(int), 1, f);
    fwrite(keys_down, sizeof(int), NUMKEYS, f);
    {
        int held = API_TurnHeld();

        fwrite(&held, sizeof(int), 1, f);
    }

    /* Where the player has walked. Part of the world as far as an agent is
     * concerned: "the nearest ground nobody has looked at" is computed from
     * it, so a restore that does not put it back answers that question using
     * wherever every other attempt happened to go. Two runs of identical
     * actions then diverge - measured, two map units by the sixteenth
     * decision, growing until the option the run is trying to take is no
     * longer on offer. */
    {
        int w = 0, h = 0;
        int n = API_RouteVisitedBytes(&w, &h);

        fwrite(&w, sizeof(int), 1, f);
        fwrite(&h, sizeof(int), 1, f);
        if (n > 0)
        {
            fwrite(API_RouteVisitedData(), 1, (size_t)n, f);
        }
    }
}

// Reads what `write_extras` wrote and puts it back on the restored world.
static boolean read_extras(FILE *f)
{
    thinker_t *th;
    mobj_t **order;
    int i, n, want, count = 0;

    if (fread(&rndindex, sizeof(int), 1, f) != 1) return false;
    if (fread(&prndindex, sizeof(int), 1, f) != 1) return false;
    if (fread(&n, sizeof(int), 1, f) != 1) return false;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker)
        {
            count++;
        }
    }
    if (count != n)
    {
        // The world that came back is not the world that was written, so
        // putting references on it would attach them to the wrong monsters.
        return false;
    }
    order = malloc(sizeof(mobj_t *) * (n > 0 ? n : 1));
    if (order == NULL) return false;
    i = 0;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker)
        {
            order[i++] = (mobj_t *)th;
        }
    }
    for (i = 0; i < n; i++)
    {
        int id, t, r;

        if (fread(&id, sizeof(int), 1, f) != 1) { free(order); return false; }
        if (fread(&t, sizeof(int), 1, f) != 1) { free(order); return false; }
        if (fread(&r, sizeof(int), 1, f) != 1) { free(order); return false; }
        order[i]->id = id;
        order[i]->target = (t >= 0 && t < n) ? order[t] : NULL;
        order[i]->tracer = (r >= 0 && r < n) ? order[r] : NULL;
    }
    if (fread(&want, sizeof(int), 1, f) != 1) { free(order); return false; }
    for (i = 0; i < want && i < numsectors; i++)
    {
        int t;

        if (fread(&t, sizeof(int), 1, f) != 1) { free(order); return false; }
        sectors[i].soundtarget = (t >= 0 && t < n) ? order[t] : NULL;
    }
    free(order);

    /* The API's pending input. See write_extras. */
    if (fread(&target_angle, sizeof(int), 1, f) != 1) return false;
    if (fread(keys_down, sizeof(int), NUMKEYS, f) != (size_t)NUMKEYS) return false;
    {
        int held;

        if (fread(&held, sizeof(int), 1, f) != 1) return false;
        API_SetTurnHeld(held);
    }

    /* The visited grid, written last. A snapshot taken before the route was
     * ever asked anything has none, which is a grid of zero by zero rather
     * than a failure. */
    {
        int w = 0, h = 0;
        unsigned char *cells;

        if (fread(&w, sizeof(int), 1, f) != 1) return false;
        if (fread(&h, sizeof(int), 1, f) != 1) return false;
        if (w <= 0 || h <= 0)
        {
            API_RouteRestoreVisited(NULL, 0, 0);
            return true;
        }
        cells = malloc((size_t)w * h);
        if (cells == NULL) return false;
        if (fread(cells, 1, (size_t)w * h, f) != (size_t)w * h)
        {
            free(cells);
            return false;
        }
        API_RouteRestoreVisited(cells, w, h);
        free(cells);
    }
    return true;
}

// The slot a request names, or -1 with the error already described.
static int slot_of(cJSON *req, const char **why)
{
    cJSON *s = req ? cJSON_GetObjectItem(req, "slot") : NULL;
    int n;

    if (s == NULL)
    {
        // One snapshot is what a counterfactual needs, so asking for it
        // should not require saying which.
        return 0;
    }
    if (!cJSON_IsNumber(s))
    {
        *why = "slot must be a number";
        return -1;
    }
    n = s->valueint;
    if (n < 0 || n >= API_SNAPSHOT_SLOTS)
    {
        *why = "slot is out of range";
        return -1;
    }
    return n;
}

// The simulation state a conformance test has to be able to see.
//
// Deliberately NOT part of /api/state: that schema is parsed with unknown
// fields rejected, so growing it would break every client at once, and none
// of this belongs in an agent's observation anyway. A player cannot see the
// RNG cursor.
//
// What it exposes is exactly what a restore was silently dropping, so a test
// can assert the mechanism rather than hope the damage shows up in behaviour
// within some number of steps. It did not: a forty-step replay with five
// monsters hunting looked identical either way.
api_response_t API_GetSim(void)
{
    thinker_t *th;
    cJSON *out = cJSON_CreateObject();
    cJSON *mobjs = cJSON_CreateArray();
    cJSON *sounds = cJSON_CreateArray();
    int i;

    cJSON_AddNumberToObject(out, "rndindex", rndindex);
    cJSON_AddNumberToObject(out, "prndindex", prndindex);
    /* The player's motion at FULL precision.
     *
     * /api/state reports position in whole map units, which is the right
     * resolution for an agent and the wrong one for asking whether two runs
     * are the same run. A sixteenth of a unit of momentum is invisible there
     * and is a couple of units of position twenty tics later - so a
     * divergence that has already happened reads as agreement until it is
     * too large to diagnose. These are the raw fixed-point values. */
    if (players[0].mo != NULL)
    {
        cJSON *me = cJSON_CreateObject();

        cJSON_AddNumberToObject(me, "x", players[0].mo->x);
        cJSON_AddNumberToObject(me, "y", players[0].mo->y);
        cJSON_AddNumberToObject(me, "z", players[0].mo->z);
        cJSON_AddNumberToObject(me, "momx", players[0].mo->momx);
        cJSON_AddNumberToObject(me, "momy", players[0].mo->momy);
        cJSON_AddNumberToObject(me, "momz", players[0].mo->momz);
        cJSON_AddNumberToObject(me, "angle", (double)players[0].mo->angle);
        cJSON_AddNumberToObject(me, "viewz", players[0].viewz);
        cJSON_AddNumberToObject(me, "bob", players[0].bob);
        cJSON_AddItemToObject(out, "player", me);
    }
    cJSON_AddNumberToObject(out, "leveltime", leveltime);
    /* The API's own pending input, which outlives a step by design and is
     * therefore part of the state a decision is made from. */
    cJSON_AddNumberToObject(out, "targetAngle", target_angle);
    {
        int held = 0;

        for (i = 0; i < NUMKEYS; i++)
        {
            if (keys_down[i] > 0)
            {
                held++;
            }
        }
        cJSON_AddNumberToObject(out, "keysHeld", held);
    }
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker)
        {
            mobj_t *mo = (mobj_t *)th;
            cJSON *o = cJSON_CreateObject();

            cJSON_AddNumberToObject(o, "id", mo->id);
            cJSON_AddNumberToObject(o, "target", mo->target ? mo->target->id : -1);
            cJSON_AddNumberToObject(o, "tracer", mo->tracer ? mo->tracer->id : -1);
            cJSON_AddItemToArray(mobjs, o);
        }
    }
    cJSON_AddItemToObject(out, "mobjs", mobjs);
    for (i = 0; i < numsectors; i++)
    {
        if (sectors[i].soundtarget != NULL)
        {
            cJSON *o = cJSON_CreateObject();

            cJSON_AddNumberToObject(o, "sector", i);
            cJSON_AddNumberToObject(o, "heard", sectors[i].soundtarget->id);
            cJSON_AddItemToArray(sounds, o);
        }
    }
    cJSON_AddItemToObject(out, "soundtargets", sounds);
    return (api_response_t) { 200, out };
}

api_response_t API_PostSnapshot(cJSON *req)
{
    const char *why = NULL;
    int slot = slot_of(req, &why);
    cJSON *out;
    FILE *scratch;
    long written;

    if (slot < 0)
    {
        return API_CreateErrorResponse(400, (char *)why);
    }
    if (gamestate != GS_LEVEL)
    {
        return API_CreateErrorResponse(409, "no level is standing");
    }
    discard(slot);
    scratch = tmpfile();
    if (scratch == NULL)
    {
        return API_CreateErrorResponse(500, "could not open a snapshot stream");
    }

    save_stream = scratch;
    savegame_error = false;
    // No description: nothing reads it back, and the header's other fields -
    // skill, episode, map, who is playing, leveltime - are exactly what a
    // restore needs to put the right level back up.
    P_WriteSaveGameHeader("");
    P_ArchivePlayers();
    P_ArchiveWorld();
    P_ArchiveThinkers();
    P_ArchiveSpecials();
    P_WriteSaveGameEOF();
    write_extras(scratch);
    fflush(scratch);
    save_stream = NULL;

    if (savegame_error)
    {
        fclose(scratch);
        return API_CreateErrorResponse(500, "the snapshot did not write");
    }
    // Off the handle and into the heap, so the descriptor goes back.
    written = ftell(scratch);
    if (written <= 0)
    {
        fclose(scratch);
        return API_CreateErrorResponse(500, "the snapshot wrote nothing");
    }
    slots[slot].data = malloc((size_t)written);
    if (slots[slot].data == NULL)
    {
        fclose(scratch);
        return API_CreateErrorResponse(500, "out of memory for a snapshot");
    }
    rewind(scratch);
    slots[slot].len = fread(slots[slot].data, 1, (size_t)written, scratch);
    fclose(scratch);
    if (slots[slot].len != (size_t)written)
    {
        discard(slot);
        return API_CreateErrorResponse(500, "the snapshot did not read back");
    }
    slots[slot].held = true;

    out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "slot", slot);
    cJSON_AddNumberToObject(out, "tic", leveltime);
    return (api_response_t) { 200, out };
}

api_response_t API_PostSnapshotRestore(cJSON *req)
{
    const char *why = NULL;
    int slot = slot_of(req, &why);
    int held_leveltime;
    cJSON *out;
    FILE *replay;

    if (slot < 0)
    {
        return API_CreateErrorResponse(400, (char *)why);
    }
    if (!slots[slot].held)
    {
        return API_CreateErrorResponse(404, "nothing is held in that slot");
    }

    /* Let go of whatever is being held on the way IN, before the snapshot's
     * own countdowns overwrite the record of what that was. The press itself
     * lives in the game's `gamekeydown`, which a savegame does not carry, so
     * without this the game keeps holding a key the restored controller has
     * no idea about. API_RestoreControls below presses back what the snapshot
     * says, and the two together are a full resynchronisation. */
    API_ReleaseControls();

    replay = fmemopen(slots[slot].data, slots[slot].len, "rb");
    if (replay == NULL)
    {
        return API_CreateErrorResponse(500, "could not reopen the snapshot");
    }
    save_stream = replay;
    savegame_error = false;
    if (!P_ReadSaveGameHeader())
    {
        save_stream = NULL;
        fclose(replay);
        return API_CreateErrorResponse(500, "the snapshot header did not read");
    }
    // The header just set gameskill, gameepisode, gamemap and leveltime.
    // Stand the level back up from them and then put the world on top of it,
    // which is the vanilla load path: G_InitNew gives a clean level of the
    // right shape and the four unarchives replace everything that moved.
    //
    // For a generated scenario this reloads the SAME published image rather
    // than building a new one - the episode and map a scenario reports are
    // pinned, and the image stays published until the next episode replaces
    // it - so the level that comes back is the level that was snapshotted
    // and not another draw from the same seed.
    held_leveltime = leveltime;
    G_InitNew(gameskill, gameepisode, gamemap);
    leveltime = held_leveltime;

    P_UnArchivePlayers();
    P_UnArchiveWorld();
    P_UnArchiveThinkers();
    P_UnArchiveSpecials();

    if (!P_ReadSaveGameEOF())
    {
        save_stream = NULL;
        fclose(replay);
        return API_CreateErrorResponse(500, "the snapshot did not read to its end");
    }
    save_stream = NULL;
    if (!read_extras(replay))
    {
        fclose(replay);
        return API_CreateErrorResponse(500, "the snapshot's simulation state did not read");
    }
    fclose(replay);

    // Everything the route derived from the level it was looking at a moment
    // ago is now about a level that has been replaced under it - including
    // how much of the map had been seen, which is the whole reason a
    // snapshot exists rather than a replay.
    API_RouteInvalidate();
    /* The countdowns came back with the snapshot; the game's own key state
     * did not, because a savegame does not carry it. Whatever was held on
     * the way IN was let go before the read (see above), so this presses
     * back exactly what the snapshot says was held and the two are in step. */
    API_RestoreControls();
    /* Events are derived by comparing the world to the previous tic, and the
     * world has just moved bodily to another moment - so the comparison would
     * report damage and healing that never happened. */
    API_Agent_Rebaseline();

    out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "slot", slot);
    cJSON_AddNumberToObject(out, "tic", leveltime);
    return (api_response_t) { 200, out };
}

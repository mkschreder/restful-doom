// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Martin Schröder <info@swedishembedded.com>
//
// See api_snapshot.h for why replaying a prefix is not good enough.

#include <stdio.h>
#include <string.h>

#include "api_snapshot.h"
#include "api_cJSON.h"
#include "api_route.h"

#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"
#include "p_saveg.h"
#include "r_state.h"

// One held state. `stream` is a `tmpfile`, which is the portable way to get a
// `FILE *` the savegame writer can use without inventing a second one: the
// archive functions in p_saveg.c write through `save_stream` and nothing
// else, so the whole of vanilla's save path works unchanged against it. The
// file never reaches a name, is deleted when closed, and in practice never
// leaves the page cache.
typedef struct
{
    FILE *stream;
    boolean held;
} api_snapshot_t;

static api_snapshot_t slots[API_SNAPSHOT_SLOTS];

static void discard(int i)
{
    if (slots[i].stream != NULL)
    {
        fclose(slots[i].stream);
    }
    slots[i].stream = NULL;
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

api_response_t API_PostSnapshot(cJSON *req)
{
    const char *why = NULL;
    int slot = slot_of(req, &why);
    cJSON *out;

    if (slot < 0)
    {
        return API_CreateErrorResponse(400, (char *)why);
    }
    if (gamestate != GS_LEVEL)
    {
        return API_CreateErrorResponse(409, "no level is standing");
    }
    discard(slot);
    slots[slot].stream = tmpfile();
    if (slots[slot].stream == NULL)
    {
        return API_CreateErrorResponse(500, "could not open a snapshot stream");
    }

    save_stream = slots[slot].stream;
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
    fflush(slots[slot].stream);
    save_stream = NULL;

    if (savegame_error)
    {
        discard(slot);
        return API_CreateErrorResponse(500, "the snapshot did not write");
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

    if (slot < 0)
    {
        return API_CreateErrorResponse(400, (char *)why);
    }
    if (!slots[slot].held)
    {
        return API_CreateErrorResponse(404, "nothing is held in that slot");
    }

    rewind(slots[slot].stream);
    save_stream = slots[slot].stream;
    savegame_error = false;
    if (!P_ReadSaveGameHeader())
    {
        save_stream = NULL;
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
        return API_CreateErrorResponse(500, "the snapshot did not read to its end");
    }
    save_stream = NULL;

    // Everything the route derived from the level it was looking at a moment
    // ago is now about a level that has been replaced under it - including
    // how much of the map had been seen, which is the whole reason a
    // snapshot exists rather than a replay.
    API_RouteInvalidate();

    out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "slot", slot);
    cJSON_AddNumberToObject(out, "tic", leveltime);
    return (api_response_t) { 200, out };
}

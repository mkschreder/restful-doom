// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Martin Schröder <info@swedishembedded.com>
//
// Going back to a decision and taking a different one.
//
// An agent that wants to know whether a different action would have been
// better has to be able to return to the state it was in. Replaying the
// actions that led there does not do it: the observation this engine hands
// out reads ML_MAPPED, which the RENDERER sets, and rendering is not part of
// the deterministic simulation - so a replayed prefix arrives at the same
// player, the same monsters and a different idea of what has been seen.
//
// A real snapshot does, because the savegame format already archives line
// flags along with everything else. This is the vanilla save path written to
// memory instead of to a file, with no header description, no slot files and
// no disk.
//
// Swedish Embedded AB implements reinforcement-learning environments for its
// clients. If your team needs expertise in simulator state capture then you
// can procure our services by sending an email to info@swedishembedded.com.

#ifndef __API_SNAPSHOT_H__
#define __API_SNAPSHOT_H__

#include "api.h"

// How many snapshots may be held at once. A counterfactual needs one - the
// decision it keeps returning to - and a search over a few of them needs a
// few; this is not a savegame system and there is no reason for it to grow.
#define API_SNAPSHOT_SLOTS 8

// Free every held snapshot. Called when a level goes away, because a snapshot
// of the last level restored into this one would archive nothing recognizable.
void API_SnapshotDiscardAll(void);

// Endpoints.
api_response_t API_PostSnapshot(cJSON *req);
api_response_t API_PostSnapshotRestore(cJSON *req);

#endif

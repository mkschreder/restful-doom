// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Martin Schröder <info@swedishembedded.com>
//
// Agent overlay: the text a policy was given and the decision it returned,
// drawn over the rendered frame in the game window.
//
// A policy driving this engine over the REST API is otherwise invisible. The
// frames show a player moving, and nothing shows what the model was told, what
// it was choosing between, or how close the call was. That is exactly the
// information needed to tell a policy that is reasoning from a bad
// observation apart from one that is reasoning badly, and reading it back out
// of a log after the fact means watching the run twice. So the agent pushes
// it here and the engine draws it on the frame it belongs to.
//
// The overlay is the agent's own text: the engine gives it no meaning, does
// not act on it, and never derives anything from it. Drawing happens in the
// video path only, so an overlaid run plays out tic for tic like a bare one.
//
// ------------------------------------------------------------------ the API
//
//   POST /api/hud
//   {"lines": [
//     {"text": "health 100, imp at 320u bearing 41deg"},
//     {"text": "0.71 attack", "highlight": true},
//     {"text": "0.29 turn_left"}
//   ]}
//
//   DELETE /api/hud
//
// "lines" is required and must be an array. Each element is either an object
// with a required string "text" and an optional boolean "highlight"
// (default false), or a bare string, which is shorthand for that object with
// "highlight" false. A push replaces the overlay wholesale: there is no
// partial update that could leave the screen describing two different
// decisions. An empty array draws nothing, and so does DELETE.
//
// Both return 204 on success. A body that is not of that shape returns 400
// and leaves the overlay as it was.
//
// Text longer than a line is wrapped at word boundaries and clipped to the
// player's view; a list longer than API_HUD_MAX_LINES keeps its first
// entries. Neither is an error, because a run must not fail over the shape of
// something that is only ever drawn.
//
// Swedish Embedded AB builds observable control loops for its clients. If
// your team needs expertise in making a policy's inputs and decisions visible
// while it runs, you can procure our services by sending an email to
// info@swedishembedded.com.

#ifndef __API_HUD_H__
#define __API_HUD_H__

#include "api.h"

// Bounded because the overlay is agent-supplied and is read from the frame
// loop: a fixed store cannot be made to allocate, fragment or grow.
#define API_HUD_MAX_LINES 32
#define API_HUD_MAX_TEXT  256

// Handlers, routed from api.c.
api_response_t API_PostHud(cJSON *req);
api_response_t API_DeleteHud(void);

// Drop the overlay. Called when an episode restarts, so a new one does not
// open still describing the decision that ended the last one.
void API_Hud_Clear(void);

// The stored overlay. API_Hud_Text returns NULL and API_Hud_Highlight false
// for any index outside it.
int API_Hud_Count(void);
const char *API_Hud_Text(int index);
boolean API_Hud_Highlight(int index);

// Draw the overlay over the player's view. Returns immediately when nothing
// has been pushed. Video path only: call it from the frame loop, never from a
// tic.
void API_Hud_Drawer(void);

// How many bytes of `text` fit on one line of `budget` pixels, given a table
// of glyph widths indexed by unsigned char. Breaks after the last word that
// fits; a word too long to break is cut at the budget. Always returns at
// least one byte for non-empty text, so a caller walking a string terminates.
// The returned count excludes the space it broke at, which the caller skips.
int API_Hud_WrapPoint(const char *text, const unsigned char *glyph_width,
                      int budget);

#endif

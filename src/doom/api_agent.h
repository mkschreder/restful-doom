//
// Agent-facing API: one observation per request, lockstep tics, episodes.
//
// The endpoints in the other api_*_controller files are built for a human
// poking at a running game. An agent needs something different: the WHOLE
// observation in one round trip, the game advancing only when it says so, an
// episode it can restart reproducibly, and a record of what happened in
// between. That is what lives here.
//

#ifndef __API_AGENT_H__
#define __API_AGENT_H__

#include "api.h"

// Called from API_Init once the socket is up.
void API_Agent_Init(void);

// Whether -apilockstep was given: the game advances only on request.
boolean API_Agent_Lockstep(void);

// Called once per tic from API_RunIO, before the game runs the tic. In
// lockstep this is where the loop is handed to the API and blocks.
void API_Agent_PerTic(void);

// Endpoints.
api_response_t API_GetState(void);
api_response_t API_PostStep(cJSON *req);
api_response_t API_PostEpisode(cJSON *req);
api_response_t API_GetFrame(void);
api_response_t API_GetMap(void);
api_response_t API_GetRouteDebug(void);

#endif

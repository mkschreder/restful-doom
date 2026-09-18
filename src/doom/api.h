#ifndef __API_H__
#define __API_H__

#include <string.h>
#include "api_cJSON.h"
#include "api_yuarel.h"

#include "p_local.h"
#include "m_fixed.h"
#include "m_argv.h"

#define NUMKEYS   256
#define NUMDESCRIPTIONS 125

/* Defined once in api.c. These used to be tentative definitions in the
 * header, which only linked because pre-GCC-10 defaulted to -fcommon; every
 * translation unit that included api.h emitted its own, and the linker
 * silently merged them. */
extern int keys_down[NUMKEYS];
extern int target_angle;

void API_Init(int port);
void API_RunIO();
/* Wait up to timeout_ms (negative: forever) for a complete request to arrive;
 * returns whether one is buffered. Exposed so a lockstep driver can hand the
 * game loop over to the API between tics. */
boolean API_Poll(int timeout_ms);
/* Serve the buffered request. */
void API_ServeRequest(void);
extern boolean api_verbose;
float API_FixedToFloat(fixed_t fixed);
fixed_t API_FloatToFixed(float val);
cJSON* DescribeMObj(mobj_t *obj);
void API_SetHUDMessage(char *msg);
void API_FlipFlag(int *flags, int mask, boolean on);
void turnPlayer();
int angleToDegrees(angle_t angle);

typedef struct {
  int id;
  char *text;
} api_obj_description_t;

typedef struct {
  char method[10];
  /* The request target as received, for logging. */
  char full_path[512];
  /* The same target, owned by the request, for yuarel to chop up in place:
   * `url` points INTO this buffer, so it has to outlive the parse. */
  char path[512];
  struct yuarel url;
  char *body;
} api_request_t;

typedef struct {
  int status_code;
  cJSON *json;
} api_response_t;

api_response_t API_CreateErrorResponse(int status, char *message);
void API_SendResponse(api_response_t resp);

#endif

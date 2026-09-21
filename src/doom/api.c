#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL_net.h>
#include <math.h>

#include "api.h"
#include "../m_misc.h"
#include "d_player.h"
#include "m_menu.h"
#include "../d_event.h"
#include "../doomkeys.h"
#include "p_local.h"
#include "api_player_controller.h"
#include "api_world_controller.h"
#include "api_door_controller.h"
#include "api_object_controller.h"
#include "api_agent.h"
#include "api_snapshot.h"


extern api_obj_description_t api_descriptors[];
char hud_message[512];

int keys_down[NUMKEYS];
int target_angle;

TCPsocket server_sd;
TCPsocket client_sd;
SDLNet_SocketSet set;

// ----
//  HTTP transport
//
//  One client at a time: this is a control channel for one agent, not a web
//  server. Requests are accumulated into req_buf until a COMPLETE one is
//  present (headers plus Content-Length bytes of body) - a request is not
//  assumed to arrive in a single recv(), because a JSON body larger than the
//  path MTU does not.
// ----

#define API_REQ_MAX 65536

static char req_buf[API_REQ_MAX + 1];
static int req_len;
static boolean client_open;
static boolean client_keepalive;

// Log one line per request. Off by default: a lockstep agent issues one
// request per game tic, and the access log is then the dominant cost of a
// training run as well as the dominant content of its stdout.
boolean api_verbose;

void API_AfterTic();
boolean API_ParseRequest(char *buffer, int buffer_len, api_request_t *request);
api_response_t API_RouteRequest(api_request_t request);
static void API_CloseClient(void);

// externally-defined game variables
extern player_t players[MAXPLAYERS];
extern int consoleplayer;

void API_Init(int port)
{
    IPaddress ip;
    const char *host;

    target_angle = -1;

    if (SDLNet_Init() < 0)
    {
        fprintf(stderr, "Error: SDLNet_Init: %s\n", SDLNet_GetError());
        exit(EXIT_FAILURE);
    }
    if (SDLNet_ResolveHost(&ip, NULL, port) < 0)
    {
        fprintf(stderr, "Error: SDLNet_ResolveHost: %s\n", SDLNet_GetError());
        exit(EXIT_FAILURE);
    }

    if (!(server_sd = SDLNet_TCP_Open(&ip)))
    {
        fprintf(stderr, "Error: SDLNet_TCP_Open: %s\n", SDLNet_GetError());
        exit(EXIT_FAILURE);
    }
    if (!(set = SDLNet_AllocSocketSet(10)))
    {
        fprintf(stderr, "Error: SDLNet_AllocSocketSet: %s\n", SDLNet_GetError());
        exit(EXIT_FAILURE); 
    }
    // The LISTENING socket is in the set too, so a blocking wait wakes on an
    // incoming connection instead of having to poll for one.
    SDLNet_TCP_AddSocket(set, server_sd);

    api_verbose = M_CheckParm("-apiverbose") > 0;

    for (int i = 0; i < NUMKEYS; i++) {
        keys_down[i] = -1;
    }

    host = SDLNet_ResolveIP(&ip);
    printf("API_Init: Listening for connections on %s:%d\n", host, port);

    API_Agent_Init();
}

// Close the client connection and discard whatever it had buffered.
static void API_CloseClient(void)
{
    if (client_open)
    {
        SDLNet_TCP_DelSocket(set, client_sd);
        SDLNet_TCP_Close(client_sd);
        client_open = false;
    }
    req_len = 0;
}

// Accept a pending connection, replacing any client already attached.
static void API_AcceptClient(void)
{
    TCPsocket csd = SDLNet_TCP_Accept(server_sd);

    if (!csd)
    {
        return;
    }
    if (client_open)
    {
        API_CloseClient();
    }
    client_sd = csd;
    SDLNet_TCP_AddSocket(set, client_sd);
    client_open = true;
    client_keepalive = true;
    req_len = 0;
}

// Header field lookup, line by line and case-insensitively, because a header
// name's case is not significant and a client is free to send any of them.
static char *API_FindHeader(char *head, int head_len, const char *name)
{
    int name_len = strlen(name);
    char *p = head;
    char *end = head + head_len;

    while (p < end)
    {
        char *eol = memchr(p, '\n', end - p);
        int line_len = eol ? (int)(eol - p) : (int)(end - p);

        if (line_len >= name_len && strncasecmp(p, name, name_len) == 0)
        {
            return p + name_len;
        }
        if (!eol)
        {
            break;
        }
        p = eol + 1;
    }
    return NULL;
}

// Total byte length of the complete request at the head of req_buf, or -1 if
// what has arrived so far is not yet a whole one.
static int API_RequestBytes(void)
{
    char *hdr_end;
    char *value;
    int head_len;
    int body_len = 0;

    req_buf[req_len] = 0;
    hdr_end = strstr(req_buf, "\r\n\r\n");
    if (hdr_end == NULL)
    {
        return -1;
    }
    head_len = (int)(hdr_end - req_buf) + 4;

    value = API_FindHeader(req_buf, head_len, "Content-Length:");
    if (value != NULL)
    {
        body_len = atoi(value);
        if (body_len < 0)
        {
            body_len = 0;
        }
    }
    if (req_len < head_len + body_len)
    {
        return -1;
    }
    return head_len + body_len;
}

static void API_ReadMore(void)
{
    int n;

    if (req_len >= API_REQ_MAX)
    {
        // A request that cannot fit is not a request we can answer; dropping
        // the connection is the only honest response, and leaving the bytes
        // in the buffer would wedge the loop forever.
        printf("API: request exceeds %d bytes, dropping connection\n", API_REQ_MAX);
        API_CloseClient();
        return;
    }
    n = SDLNet_TCP_Recv(client_sd, req_buf + req_len, API_REQ_MAX - req_len);
    if (n <= 0)
    {
        API_CloseClient();
        return;
    }
    req_len += n;
}

// Wait up to timeout_ms for a complete request to be buffered. A negative
// timeout waits indefinitely. Returns whether one is now available.
//
// Both the listening socket and the client socket are in the socket set, so a
// blocking wait wakes on a new CONNECTION as well as on new data - otherwise
// the first request of a run could only be noticed by polling.
boolean API_Poll(int timeout_ms)
{
    for (;;)
    {
        boolean progress = false;

        if (API_RequestBytes() > 0)
        {
            return true;
        }
        if (SDLNet_CheckSockets(set, timeout_ms < 0 ? 100 : timeout_ms) > 0)
        {
            if (SDLNet_SocketReady(server_sd))
            {
                API_AcceptClient();
                progress = true;
            }
            if (client_open && SDLNet_SocketReady(client_sd))
            {
                API_ReadMore();
                progress = true;
            }
        }
        if (timeout_ms >= 0 && !progress)
        {
            return API_RequestBytes() > 0;
        }
    }
}

static const char *API_StatusText(int status)
{
    switch (status)
    {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static boolean API_SendAll(const char *data, int len)
{
    int sent = 0;

    while (sent < len)
    {
        int n = SDLNet_TCP_Send(client_sd, (void *)(data + sent), len - sent);
        if (n <= 0)
        {
            API_CloseClient();
            return false;
        }
        sent += n;
    }
    return true;
}

void API_SendResponse(api_response_t resp)
{
    char header[256];
    char *body = NULL;
    int body_len = 0;
    int hdr_len;

    if (!client_open)
    {
        if (resp.json)
        {
            cJSON_Delete(resp.json);
        }
        return;
    }

    if (resp.json)
    {
        // Unformatted: a lockstep agent reads one of these per tic and never
        // looks at the wire bytes, so the pretty-printer's whitespace is pure
        // cost. Anything reading them by hand can pipe through a formatter.
        body = cJSON_PrintUnformatted(resp.json);
        body_len = body ? (int)strlen(body) : 0;
    }

    hdr_len = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %d\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Connection: %s\r\n"
                       "\r\n",
                       resp.status_code, API_StatusText(resp.status_code),
                       body_len, client_keepalive ? "keep-alive" : "close");

    API_SendAll(header, hdr_len);
    if (body_len > 0)
    {
        API_SendAll(body, body_len);
    }
    if (body)
    {
        free(body);
    }
    if (resp.json)
    {
        cJSON_Delete(resp.json);
    }
    if (!client_keepalive)
    {
        API_CloseClient();
    }
}

// Serve the complete request at the head of req_buf.
void API_ServeRequest(void)
{
    api_request_t request;
    api_response_t response;
    int total = API_RequestBytes();
    char saved;
    char *value;

    if (total <= 0)
    {
        return;
    }

    // Terminate exactly at the end of THIS request: the buffer may already
    // hold the next one (pipelining), and the parser works on C strings.
    saved = req_buf[total];
    req_buf[total] = 0;

    value = API_FindHeader(req_buf, total, "Connection:");
    client_keepalive = !(value != NULL && strcasestr(value, "close") != NULL);

    if (API_ParseRequest(req_buf, total, &request))
    {
        response = API_RouteRequest(request);
    }
    else
    {
        response = API_CreateErrorResponse(400, "invalid request");
        request.method[0] = 0;
        request.full_path[0] = 0;
    }

    req_buf[total] = saved;
    memmove(req_buf, req_buf + total, req_len - total);
    req_len -= total;

    if (api_verbose)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s %s", request.method, request.full_path);
        API_SetHUDMessage(msg);
        printf("access_log: \"%s %s\" %d\n", request.method, request.full_path,
               response.status_code);
    }

    // Status 0 means the handler owes a response it cannot write yet - a step
    // is answered with the state AFTER its tics have run, so the reply is sent
    // from API_Agent_PerTic once they have.
    if (response.status_code != 0)
    {
        API_SendResponse(response);
    }
}

void API_RunIO()
{
    // In lockstep the agent owns the clock, and API_Agent_PerTic is what
    // blocks the loop until it says to advance - serving requests from there
    // rather than here, so a request cannot be answered mid-step.
    if (!API_Agent_Lockstep() && API_Poll(0))
    {
        API_ServeRequest();
    }

    API_Agent_PerTic();
    API_AfterTic();
}

boolean API_ParseRequest(char *buffer, int buffer_len, api_request_t *request)
{
    char method[10];
    char protocol[10];
    struct yuarel url;
    char *http_entity_body;
    int ret;

    // Field widths, because a request line is attacker-controlled input and
    // the destinations are fixed-size: an unbounded %s here wrote past both
    // `method` and the 100-byte global `path` on any long request.
    ret = sscanf(buffer, "%9s %511s %9s", method, request->path, protocol);
    if (ret != 3) {
        return false;
    }

    M_StringCopy(request->full_path, request->path, sizeof(request->full_path));

    // Parses IN PLACE, writing terminators into request->path - which is why
    // the buffer belongs to the request and not to this frame.
    if (-1 == yuarel_parse(&url, request->path)) {
        return false;
    }
    http_entity_body = strstr(buffer, "\r\n\r\n");
    if (http_entity_body == NULL)
    {
        return false;
    }

    request->url = url;
    M_StringCopy(request->method, method, sizeof(request->method));
    request->body = http_entity_body;
    return true;
}


// ----
//  Route an http path + method to a controller action
// ----
api_response_t API_RouteRequest(api_request_t req)
{
    int id;
    int id2;
    int distance;
    int p;
    float x = 0.0;
    float y = 0.0;
    struct yuarel_param params[3];
    char *method = req.method;
    char *path = req.url.path;
    cJSON *json = cJSON_Parse(req.body);
    
    if (strcmp(path, "api/state") == 0)
    {
        if (strcmp(method, "GET") == 0)
        {
            return API_GetState();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/step") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            return API_PostStep(json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/episode") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            return API_PostEpisode(json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/sim") == 0)
    {
        // Simulation internals, for the conformance suite. Not in
        // /api/state: that schema rejects unknown fields, and none of this is
        // anything a player could see.
        if (strcmp(method, "GET") == 0)
        {
            return API_GetSim();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/snapshot") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            return API_PostSnapshot(json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/snapshot/restore") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            return API_PostSnapshotRestore(json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/map") == 0)
    {
        if (strcmp(method, "GET") == 0)
        {
            return API_GetMap();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/route") == 0)
    {
        if (strcmp(method, "GET") == 0)
        {
            return API_GetRouteDebug();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/frame") == 0)
    {
        if (strcmp(method, "GET") == 0)
        {
            return API_GetFrame();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/message") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            return API_PostMessage(json);
        } 
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/player") == 0)
    {
        if (strcmp(method, "PATCH") == 0) 
        {
            return API_PatchPlayer(json);
        }
        else if (strcmp(method, "GET") == 0)
        {
            return API_GetPlayer();
        }
        else if (strcmp(method, "DELETE") == 0) {
            return API_DeletePlayer();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/player/actions") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            return API_PostPlayerAction(json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/player/turn") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            return API_PostTurnDegrees(json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/players") == 0)
    {
        if (strcmp(method, "GET") == 0)
        {
            return API_GetPlayers();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strstr(path, "api/players/") != NULL) {
        if (sscanf(path, "api/players/%d", &id) != 1) {
            return API_CreateErrorResponse(404, "path not found");
        }
        else if (strcmp(method, "GET") == 0)
        {
            return API_GetPlayerById(id);
        }
        else if (strcmp(method, "PATCH") == 0) 
        {
            return API_PatchPlayerById(json, id);
        }
        else if (strcmp(method, "DELETE") == 0) 
        {
            return API_DeletePlayerById(id);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/world") == 0) {
        if (strcmp(method, "GET") == 0) {
            return API_GetWorld();
        }
        else if (strcmp(method, "PATCH") == 0) {
            return API_PatchWorld(json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/world/screenshot") == 0) {
        if (strcmp(method, "GET") == 0) {
            return API_GetWorldScreenshot();
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strcmp(path, "api/world/objects") == 0)
    {
        if (strcmp(method, "POST") == 0) 
        {
            return API_PostObject(json);
        }
        else if (strcmp(method, "GET") == 0)
        {
            distance = 0;
            p = yuarel_parse_query(req.url.query, '&', params, 1);
            while (p-- > 0) {
                if (strcmp("distance", params[p].key) == 0) {
                    distance = atoi(params[p].val);
                }
            }
            return API_GetObjects(distance);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strstr(path, "api/world/objects/") != NULL) {
        if (sscanf(path, "api/world/objects/%d", &id) != 1) {
            return API_CreateErrorResponse(404, "path not found");
        }
        if (strcmp(method, "DELETE") == 0)
        {
            return API_DeleteObject(id);
        }
        else if (strcmp(method, "GET") == 0)
        {
            return API_GetObject(id);
        }
        else if (strcmp(method, "PATCH") == 0)
        {
            return API_PatchObject(id, json);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strstr(path, "api/world/los/") != NULL) {
        sscanf(path, "api/world/los/%d/%d", &id,&id2);
      
        if (strcmp(method, "GET") == 0)
        {
            return API_GetLineOfSightToObject(id,id2);
        }
       
        return API_CreateErrorResponse(405, "Method not allowed");
    }

    else if (strcmp(path, "api/world/movetest") == 0) {
        if (strcmp(method, "GET") == 0)
        {
            p = yuarel_parse_query(req.url.query, '&', params, 3);
            while (p-- > 0) {
                if (strcmp("id", params[p].key) == 0) {
                    id = atoi(params[p].val); 
                }
                if (strcmp("x", params[p].key) == 0) {
                    x = atoi(params[p].val);
                }
                if (strcmp("y", params[p].key) == 0) {
                    y = atoi(params[p].val);
                }
            }
            return API_GetMoveTest(id, x, y);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }

    else if (strcmp(path, "api/world/doors") == 0) {
        if (strcmp(method, "GET") == 0)
        {
            distance = 0;
            p = yuarel_parse_query(req.url.query, '&', params, 1);
            while (p-- > 0) {
                if (strcmp("distance", params[p].key) == 0) {
                    distance = atoi(params[p].val);
                }
            }
            return API_GetDoors(distance);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    else if (strstr(path, "api/world/doors/") != NULL)
    {
        if (sscanf(path, "api/world/doors/%d", &id) != 1) {
            return API_CreateErrorResponse(404, "path not found");
        }
        if (strcmp(method, "PATCH") == 0)
        {
            return API_PatchDoor(id, json);
        }
        else if (strcmp(method, "GET") == 0)
        {
            return API_GetDoor(id);
        }
        return API_CreateErrorResponse(405, "Method not allowed");
    }
    return API_CreateErrorResponse(404, "Not found");
}

api_response_t API_CreateErrorResponse(int status, char *message) {
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "error", message);
  return (api_response_t) { status, body };
}

void postTurnEvent(int amount)
{
    event_t event;
    event.type = ev_mouse;
    event.data1 = 0;  // buttons held down
    event.data2 = amount;  // turn (positive clockwise)
    event.data3 = 0;  // move (max 16 units per tic)
    D_PostEvent(&event);
}

int angleToDegrees(angle_t angle)
{
    return ((double)angle / ANG_MAX) * 360;
}

int turnAmount(int remainingAngle)
{
    int amount = pow(remainingAngle, 2);
    if (amount > 500)
        return 500;
    else
        return amount;
}

void turnPlayer()
{
    int direction;
    int remaining;
    int playerAngle;

    if (target_angle >= 0)
    {
        player_t *player = &players[consoleplayer];
        playerAngle = angleToDegrees(player->mo->angle);
        remaining = target_angle - playerAngle;
        if (remaining < 0)
            remaining = remaining + 360;

        if (remaining < 180)
            direction = -1;
        else
            direction = 1;

        if (remaining == 0) {
            target_angle =- 1;
            return;
        }
        else
        {
            postTurnEvent(turnAmount(remaining) * direction);
        }
    }
}

/* Let go of everything the player was doing.
 *
 * A key held for a countdown of tics and a turn still closing on its target
 * both outlive the episode that asked for them, and the first tic of the NEXT
 * episode then replays the tail of the last decision of the previous one. It
 * is one unit of movement and one degree of turn, and it is enough: the
 * levels are deterministic, so from the second episode onward every run is a
 * different run. Measured on E1M1, which finishes from its own spawn as the
 * first episode of a process and, as the second, walks into a doorway it
 * wedges in and stands there for the rest of the run - eleven times out of
 * twelve, identically, which is what a determinism bug looks like from
 * outside. */
void API_ReleaseControls(void)
{
    int i;

    for (i = 0; i < NUMKEYS; i++)
    {
        if (keys_down[i] > 0)
        {
            event_t event;

            event.type = ev_keyup;
            event.data1 = i;
            event.data2 = 0;
            D_PostEvent(&event);
        }
        keys_down[i] = -1;
    }
    target_angle = -1;
}

void API_AfterTic()
{
    for (int i = 0; i < NUMKEYS; i++) {
        if (keys_down[i] >= 0) {
            keys_down[i]--;
        }
        if (keys_down[i] == 0) {
            event_t event;
            event.type = ev_keyup;
            event.data1 = i;
            event.data2 = 0;
            D_PostEvent(&event);
        }
    }

    turnPlayer();
}

// Helper methods

float API_FixedToFloat(fixed_t fixed) {
    double val = fixed;
    double ex = 1<<16;
    return val / ex;
}

fixed_t API_FloatToFixed(float val) {
    return (int)(val * (1<<16));
}

cJSON* DescribeMObj(mobj_t *obj)
{
    cJSON *pos;
    cJSON *flags;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", obj->id);
    cJSON_AddItemToObject(root, "position", pos = cJSON_CreateObject());
    cJSON_AddNumberToObject(pos, "x", API_FixedToFloat(obj->x));
    cJSON_AddNumberToObject(pos, "y", API_FixedToFloat(obj->y));
    cJSON_AddNumberToObject(pos, "z", API_FixedToFloat(obj->z));
    cJSON_AddNumberToObject(root, "angle", angleToDegrees(obj->angle));
    cJSON_AddNumberToObject(root, "height", API_FixedToFloat(obj->height));
    cJSON_AddNumberToObject(root, "health", obj->health);
    cJSON_AddNumberToObject(root, "typeId", mobjinfo[obj->type].doomednum);

    // this is... inefficient :(
    for (int i = 0; i < NUMDESCRIPTIONS; i++)
    {
        if (api_descriptors[i].id == mobjinfo[obj->type].doomednum) {
            cJSON_AddStringToObject(root, "type", api_descriptors[i].text);
            break;
        }
    }

    if (obj->target) {
        cJSON_AddNumberToObject(root, "attacking", obj->target->id);
    }

    flags = cJSON_CreateObject();
    if (obj->flags & MF_SPECIAL) cJSON_AddTrueToObject(flags, "MF_SPECIAL");
    if (obj->flags & MF_SOLID) cJSON_AddTrueToObject(flags, "MF_SOLID");
    if (obj->flags & MF_SHOOTABLE) cJSON_AddTrueToObject(flags, "MF_SHOOTABLE");
    if (obj->flags & MF_NOSECTOR) cJSON_AddTrueToObject(flags, "MF_NOSECTOR");
    if (obj->flags & MF_NOBLOCKMAP) cJSON_AddTrueToObject(flags, "MF_NOBLOCKMAP");
    if (obj->flags & MF_AMBUSH) cJSON_AddTrueToObject(flags, "MF_AMBUSH");
    if (obj->flags & MF_JUSTHIT) cJSON_AddTrueToObject(flags, "MF_JUSTHIT");
    if (obj->flags & MF_JUSTATTACKED) cJSON_AddTrueToObject(flags, "MF_JUSTATTACKED");
    if (obj->flags & MF_SPAWNCEILING) cJSON_AddTrueToObject(flags, "MF_SPAWNCEILING");
    if (obj->flags & MF_NOGRAVITY) cJSON_AddTrueToObject(flags, "MF_NOGRAVITY");
    if (obj->flags & MF_DROPOFF) cJSON_AddTrueToObject(flags, "MF_DROPOFF");
    if (obj->flags & MF_PICKUP) cJSON_AddTrueToObject(flags, "MF_PICKUP");
    if (obj->flags & MF_NOCLIP) cJSON_AddTrueToObject(flags, "MF_NOCLIP");
    if (obj->flags & MF_SLIDE) cJSON_AddTrueToObject(flags, "MF_SLIDE");
    if (obj->flags & MF_FLOAT) cJSON_AddTrueToObject(flags, "MF_FLOAT");
    if (obj->flags & MF_TELEPORT) cJSON_AddTrueToObject(flags, "MF_TELEPORT");
    if (obj->flags & MF_MISSILE) cJSON_AddTrueToObject(flags, "MF_MISSILE");
    if (obj->flags & MF_DROPPED) cJSON_AddTrueToObject(flags, "MF_DROPPED");
    if (obj->flags & MF_SHADOW) cJSON_AddTrueToObject(flags, "MF_SHADOW");
    if (obj->flags & MF_NOBLOOD) cJSON_AddTrueToObject(flags, "MF_NOBLOOD");
    if (obj->flags & MF_CORPSE) cJSON_AddTrueToObject(flags, "MF_CORPSE");
    if (obj->flags & MF_INFLOAT) cJSON_AddTrueToObject(flags, "MF_INFLOAT");
    if (obj->flags & MF_COUNTKILL) cJSON_AddTrueToObject(flags, "MF_COUNTKILL");
    if (obj->flags & MF_COUNTITEM) cJSON_AddTrueToObject(flags, "MF_COUNTITEM");
    if (obj->flags & MF_SKULLFLY) cJSON_AddTrueToObject(flags, "MF_SKULLFLY");
    if (obj->flags & MF_NOTDMATCH) cJSON_AddTrueToObject(flags, "MF_NOTDMATCH");
    if (obj->flags & MF_TRANSLATION) cJSON_AddTrueToObject(flags, "MF_TRANSLATION");

    cJSON_AddItemToObject(root, "flags", flags);
    return root;
}

void API_SetHUDMessage(char *msg)
{
    // strncpy(dst, src, sizeof dst) does not terminate when the source is at
    // least as long as the destination, and the result is handed straight to
    // the status-bar drawer.
    M_StringCopy(hud_message, msg, sizeof(hud_message));
    players[consoleplayer].message = hud_message;
}

void API_FlipFlag(int *flags, int mask, boolean on) {
    if (on)
    {
        *flags |= mask;
    }
    else
    {
        *flags &= ~mask;
    }
}

# RESTful-DOOM

An HTTP + JSON API hosted inside the 1993 DOOM engine!

![](http://1amstudios.com/img/restful-doom/header.jpg)

RESTful-DOOM is a version of Doom which hosts a RESTful API! The API allows you to query and manipulate various game objects with standard HTTP requests as the game runs.

There were a few challenges:

- Build an HTTP+JSON RESTful API server in C.
- Run the server code inside the Doom engine, without breaking the game loop.
- Figure out what kinds of things we can manipulate in the game world, and how to interact with them in memory to achieve the desired effect!

RESTFul-DOOM is built on top of the awesome [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) project. I like this project because it aims to stick as close to the original experience as possible, while making it easy to compile and run on modern systems. This was only possible by building on top of their hard work!

### More details in blog post:
http://1amstudios.com/2017/08/01/restful-doom/

## Driving it from an agent

Alongside the human-facing API above there is a surface shaped for something
that decides once per tic. The difference is not cosmetic - a policy needs the
whole observation in one round trip, a game that advances only when it says so,
an episode it can restart reproducibly, and a record of what happened in
between.

| | |
|---|---|
| `GET /api/state` | the whole observation in ONE request: level progress, the player, what is around them sorted by distance, how far there is to WALK in six directions, where the exit is, and the events since this was last read |
| `POST /api/step` | apply actions, run exactly N tics, answer with the state that results |
| `POST /api/episode` | restart a level reproducibly, seed included |
| `GET /api/frame` | the 320x200 framebuffer and the palette in effect |
| `POST /api/hud` | draw the observation, the options and the choice over the frame in the window; `DELETE` clears it |
| `GET /api/map` | the walkable grid and the distance to the exit across it |
| `GET /api/route` | the route's working at the player's own cell, when a bearing and a wall disagree |
| `GET /api/world/movetest` | whether a body could stand at a point, and which of P_TryMove's rules says no |

```
src/restful-doom -iwad doom1.wad -apiport 6666 -apilockstep -noblit     -warp 1 1 -skill 4 -nosound -nomusic
```

- **`-apilockstep`** hands the clock to the agent: `singletics`, and the game
  loop blocks until a step says to advance. Between steps the world is frozen -
  read `/api/state` twice two seconds apart and the tic is the same.
- **`-noblit`** keeps the engine RENDERING into its framebuffer (so
  `/api/frame` still works) while skipping the blit, upscale and present. On
  E1M1 that is most of the cost of a tic.
- **`-apiverbose`** turns the per-request access log back on. It is off by
  default because at one request per tic it is not a log, and writing it was a
  measurable part of a step.

Measured on E1M1: **~0.14ms per tic and ~0.45ms per request**, so a step of four
tics costs about 1.3ms against the 114ms of wall clock the same four tics take
at 35Hz - **85x realtime**.

Events are DERIVED by diffing the player's own counters each tic rather than
hooked into the engine at a dozen call sites: kills, damage, healing, armour,
ammo, weapons, keys, secrets, death and the level exit all leave a trace in
those counters, and a diff cannot miss a call site the way a hook can.

## API Spec

[API spec in RAML 1.0 format](https://github.com/jeff-1amstudios/restful-doom/blob/master/RAML/doom.raml)

## Build

### Building dependencies (needs to be run only once)

Takes care of building and configuring dependencies like SDL. Uses [chocpkg](https://github.com/chocolate-doom/chocpkg).
```
./configure-and-build.sh
```

### Compiling

Run `make` from the src (or root) directory. `src/restful-doom` will be created if the compile succeeds.

## Run

The DOOM engine is open source, but assets (art, maps etc) are not. You'll need to download an appropriate [WAD file](https://en.wikipedia.org/wiki/Doom_WAD) separately.

To run restful-doom on port 6666:
```
src/restful-doom -iwad <path/to/doom1.wad> -apiport 6666 ...
```

## Thanks!
[chocolate-doom](https://github.com/chocolate-doom/chocolate-doom) team  
[cJSON](https://github.com/DaveGamble/cJSON) - JSON parsing / generation  
[yuarel](https://github.com/jacketizer/libyuarel/) - URL parsing  

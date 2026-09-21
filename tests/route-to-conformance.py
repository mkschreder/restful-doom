#!/usr/bin/env python3
"""Can the route be pointed at a place the caller names, round a corner?

The level's distance field floods from the level's own goal, so everything
derived from it answers "which way onward" and nothing answers "which way back
to the thing I walked past". Without that second answer the only way to go
back for an item is a straight line at where it was, which in anything but an
open room is a heading into a wall.

Checks the MECHANISM: a point that a straight line cannot reach, and a route
that reaches it anyway. Passing this with the straight line and the path
agreeing would prove nothing, so the test fails itself if it cannot find a
place where they disagree.
"""
import json, subprocess, time, urllib.request, os, signal, sys, math

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45993
# The IWAD, which is a resource and is never in this repository. Pass a path,
# or keep one where the brain workspace does.
WAD = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "edgeai", "brain", "testdata", "doom", "doom1.wad")
if not os.path.exists(WAD):
    print(f"SKIP: no IWAD at {WAD}; pass one as the second argument")
    sys.exit(0)

def post(p, b):
    r = urllib.request.Request(f"http://localhost:{PORT}{p}", data=json.dumps(b).encode(),
        headers={"Content-Type":"application/json"}, method="POST")
    return json.loads(urllib.request.urlopen(r).read())
def get(p):
    return json.loads(urllib.request.urlopen(f"http://localhost:{PORT}{p}").read())

def where():
    p = get("/api/state")["player"]
    return p["x"], p["y"], p["angle"]

def straight(px, py, angle, x, y):
    """Bearing and distance to a point as a straight line, the agent's convention."""
    rel = math.degrees(math.atan2(y - py, x - px)) - angle
    while rel > 180: rel -= 360
    while rel <= -180: rel += 360
    return round(rel), round(math.hypot(x - px, y - py))

eng = subprocess.Popen(["./src/restful-doom","-iwad",WAD,"-apiport",str(PORT),
    "-apilockstep","-warp","1","1","-skill","3","-nosound","-nomusic","-noblit"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
time.sleep(6)
fails = []
try:
    post("/api/episode", {"episode":1,"map":1,"skill":3,"seed":7,"mapKnowledge":"seen"})
    time.sleep(1)
    # Walk off the start, remembering where we began, turning away from
    # whatever stops us. Where exactly it ends up does not matter; what
    # matters is that walls end up between it and the start.
    start = where()
    was = start
    for _ in range(120):
        post("/api/step", {"tics":8,"actions":[{"type":"forward","amount":8}]})
        now = where()
        if math.hypot(now[0] - was[0], now[1] - was[1]) < 8:
            post("/api/step", {"tics":6,"actions":[{"type":"turn-right","amount":45}]})
        was = now
    px, py, angle = where()
    sb, sd = straight(px, py, angle, start[0], start[1])
    walked = round(math.hypot(px - start[0], py - start[1]))
    print(f"  setup: walked {walked} units from the start, which is now {sd} away "
          f"in a straight line, {sb} degrees off the nose")
    if walked < 300:
        print("  SKIP: the player never left the starting room, so there is no corner")
        sys.exit(2)

    r = post("/api/route", {"to":[{"x":start[0],"y":start[1]}]})["routes"][0]
    print(f"  route back to the start: {r}")
    if not r.get("reachable"):
        fails.append("the route could not find the way back to a place the player walked from")
    else:
        # The whole point: the walk is longer than the line, because the line
        # goes through walls and the walk does not.
        if r["pathDistance"] < sd:
            fails.append(f"path {r['pathDistance']} is shorter than the straight line {sd}, "
                         "which means it is not a path")
        if r["pathDistance"] == sd and r["bearing"] == sb:
            fails.append("the path and the straight line agree exactly, so this level "
                         "cannot tell the two apart and the test proves nothing")

    # A point outside the level is not reachable, and saying so is the
    # difference between "no way there" and a wrong heading.
    off = post("/api/route", {"to":[{"x":1e6,"y":1e6}]})["routes"][0]
    if off.get("reachable"):
        fails.append("a point off the map came back reachable")

    # Where the player is standing is a path of no cells, not a failure.
    here = post("/api/route", {"to":[{"x":px,"y":py}]})["routes"][0]
    if not here.get("reachable") or here.get("pathDistance") != 0:
        fails.append(f"the way to where the player already stands came back {here}")

    # Several points in one call, in order.
    many = post("/api/route", {"to":[{"x":start[0],"y":start[1]},{"x":px,"y":py}]})["routes"]
    if len(many) != 2 or many[1].get("pathDistance") != 0:
        fails.append(f"a two-point request came back {many}")

    for f in fails:
        print("  FAIL:", f)
    print("PASS: the route can be aimed at a named place" if not fails
          else f"FAILED {len(fails)} of the invariants")
    sys.exit(1 if fails else 0)
finally:
    # SIGKILL: the engine does not always go on a polite one, and an engine
    # left holding the port makes the NEXT run measure a stale world while
    # reporting it as a fresh one.
    os.killpg(os.getpgid(eng.pid), signal.SIGKILL)

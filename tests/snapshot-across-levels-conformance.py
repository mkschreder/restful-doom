#!/usr/bin/env python3
"""Does a snapshot survive a change of level?

A snapshot is self-describing: its header carries the skill, episode and map,
and restoring one calls G_InitNew with them, so it stands its OWN level back
up whatever is loaded now. If that holds, a search can keep an archive of
places it has reached across a campaign that rotates maps - and if it does
not, the archive is thrown away almost every episode and the frontier is
rebuilt from the front door each time.

Checks the MECHANISM: take a state on one level, load another, and come back.
"""
import json, subprocess, time, urllib.request, os, signal, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45970
WAD = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "edgeai", "brain", "testdata", "doom", "doom1.wad")
if not os.path.exists(WAD):
    print(f"SKIP: no IWAD at {WAD}; pass one as the second argument")
    sys.exit(0)

def post(p, b):
    r = urllib.request.Request(f"http://localhost:{PORT}{p}", data=json.dumps(b).encode(),
        headers={"Content-Type":"application/json"}, method="POST")
    return urllib.request.urlopen(r).read()
def get(p):
    return json.loads(urllib.request.urlopen(f"http://localhost:{PORT}{p}").read())

eng = subprocess.Popen(["./src/restful-doom","-iwad",WAD,"-apiport",str(PORT),
    "-apilockstep","-warp","1","1","-skill","3","-nosound","-nomusic","-noblit"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
time.sleep(6)
fails = []
try:
    post("/api/episode", {"episode":1,"map":1,"skill":3,"seed":1,"mapKnowledge":"seen"})
    time.sleep(1)
    for _ in range(4):
        post("/api/step", {"tics":8,"actions":[{"type":"forward","amount":8}]})
    on_one = get("/api/state")["player"]
    post("/api/snapshot", {"slot":7})
    print(f"  held E1M1 at ({on_one['x']},{on_one['y']}) in slot 7")

    # Another level entirely, and some play on it.
    post("/api/episode", {"episode":1,"map":3,"skill":3,"seed":1,"mapKnowledge":"seen"})
    time.sleep(1)
    for _ in range(4):
        post("/api/step", {"tics":8,"actions":[{"type":"forward","amount":8}]})
    on_three = get("/api/state")["player"]
    print(f"  then played E1M3 at ({on_three['x']},{on_three['y']})")

    post("/api/snapshot/restore", {"slot":7})
    back = get("/api/state")["player"]
    print(f"  restored slot 7 -> ({back['x']},{back['y']})")
    if (back["x"], back["y"]) != (on_one["x"], on_one["y"]):
        fails.append("the restore did not come back to where it was held")

    # And it really is the other level, not a lookalike position.
    lvl = get("/api/state")["level"]
    if lvl.get("map") != 1:
        fails.append(f"restored onto map {lvl.get('map')}, not the map it was held on")

    # The escape hatch still works.
    post("/api/episode", {"episode":1,"map":1,"skill":3,"seed":1,"forget":True})
    time.sleep(1)
    try:
        post("/api/snapshot/restore", {"slot":7})
        fails.append("a slot survived an episode that asked to forget")
    except urllib.error.HTTPError:
        pass

    for f in fails:
        print("  FAIL:", f)
    print("PASS: a snapshot outlives a change of level" if not fails
          else f"FAILED {len(fails)} of the invariants")
    sys.exit(1 if fails else 0)
finally:
    os.killpg(os.getpgid(eng.pid), signal.SIGKILL)

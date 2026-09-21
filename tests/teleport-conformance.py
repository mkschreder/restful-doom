#!/usr/bin/env python3
"""Does the route know that a teleporter is a way of getting somewhere?

A teleport linedef moves the player instantly, and the route's grid is
geometry: the two cells either side of one are as far apart as the level is
wide. So the route could never AIM at a pad, and from E1M5 onward a good deal
of the shareware episode is meant to be crossed that way.

Checks the MECHANISM: that the jumps are read off the level, that they are not
invented on a level with none, and that the search actually takes one - the
far end of a pad has to be about one step further away than the pad, not the
length of a walk round, and not out of reach.
"""
import json, subprocess, time, urllib.request, os, signal, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45988
WAD = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "edgeai", "brain", "testdata", "doom", "doom1.wad")
BEFORE = sys.argv[3] if len(sys.argv) > 3 else None
if not os.path.exists(WAD):
    print(f"SKIP: no IWAD at {WAD}; pass one as the second argument")
    sys.exit(0)

def get(port, p):
    return json.loads(urllib.request.urlopen(f"http://localhost:{port}{p}").read())
def post(port, p, b):
    r = urllib.request.Request(f"http://localhost:{port}{p}", data=json.dumps(b).encode(),
        headers={"Content-Type":"application/json"}, method="POST")
    return json.loads(urllib.request.urlopen(r).read())

# Each engine gets its own port: one that has just been killed leaves the
# previous one lingering, and a refused connection is the good outcome there -
# the bad one is a stale engine answering as though it were the new one.
used = [0]

def on(binary, map_no, ask, port=None):
    """Start an engine on a level, run `ask`, and always take it down."""
    used[0] += 1
    port = PORT + used[0]
    eng = subprocess.Popen([binary,"-iwad",WAD,"-apiport",str(port),"-apilockstep",
        "-warp","1",str(map_no),"-skill","3","-nosound","-nomusic","-noblit"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
    time.sleep(6)
    try:
        # The level the engine started on, as it stands. The jumps are read
        # off the level's geometry, so nothing here needs an episode set up -
        # and asking for one on a level whose exit cannot be reached on foot
        # gets no route grid at all, which would hide what is being tested.
        return ask(port)
    finally:
        os.killpg(os.getpgid(eng.pid), signal.SIGKILL)

def patch(port, body):
    r = urllib.request.Request(f"http://localhost:{port}/api/player", data=json.dumps(body).encode(),
        headers={"Content-Type":"application/json"}, method="PATCH")
    return json.loads(urllib.request.urlopen(r).read())

fails = []
# E1M1 has no teleporter in it, which is what says the jumps are read off the
# level and not invented. E1M5 and E1M9 are the shareware levels that do.
none = on("./src/restful-doom", 1, lambda p: get(p, "/api/route"))
print(f"  E1M1 has {none.get('teleports')} jumps")
if none.get("teleports"):
    fails.append("a level with no teleporter in it grew some anyway")

def standing_on_a_pad(jumps):
    """Put the player on each pad in turn and ask the way to where it lands.

    Standing there rather than walking there on purpose: whether a pad can be
    reached on foot from the level's spawn is a question about the rest of the
    level, and it is not the question here.
    """
    def ask(p):
        out = []
        for j in jumps:
            patch(p, {"position": {"x": j["fromX"], "y": j["fromY"]}})
            r = post(p, "/api/route", {"to":[{"x": j["x"], "y": j["y"]}]})["routes"][0]
            out.append((j, r))
        return out
    return ask

CELL = 32
crossed, checked, found = 0, 0, 0
for m in (5, 9):
    grid = on("./src/restful-doom", m, lambda p: get(p, "/api/route"))
    jumps = grid.get("teleportsTo", [])
    found += len(jumps)
    print(f"  E1M{m} has {grid.get('teleports')} jumps")
    if not jumps:
        continue
    for j, r in on("./src/restful-doom", m, standing_on_a_pad(jumps)):
        checked += 1
        if not r.get("reachable"):
            fails.append(f"E1M{m}: standing on the pad at {j['fromX']},{j['fromY']}, "
                         "there is no way to where it lands")
        # One step across, with two cells of slack: which cell a pad's centre
        # falls in is an accident of where the level sits on the grid.
        elif r["pathDistance"] <= 2 * CELL:
            crossed += 1
        else:
            fails.append(f"E1M{m}: standing on the pad at {j['fromX']},{j['fromY']}, where "
                         f"it lands is {r['pathDistance']} units of walking away - the search "
                         "is going round it rather than across it")

print(f"  {crossed} of {checked} pads are one step from where they land")
if found == 0:
    fails.append("no level has a teleporter in its route grid")
if checked == 0:
    fails.append("no pad was checked, so the jump itself is untested")

for f in fails[:6]:
    print("  FAIL:", f)
print("PASS: the route can cross a teleporter" if not fails
      else f"FAILED {len(fails)} of the invariants")
sys.exit(1 if fails else 0)

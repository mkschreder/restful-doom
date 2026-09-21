#!/usr/bin/env python3
"""Does a restore actually return the simulation to where it was?

Checks the MECHANISM, not the downstream behaviour. A forty-step replay with
five monsters hunting looked identical whether or not the RNG and the monster
targets came back, so an end-to-end behavioural test cannot tell you.
"""
import json, subprocess, time, urllib.request, os, signal, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45994
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

eng = subprocess.Popen(["./src/restful-doom","-iwad",WAD,"-apiport",str(PORT),
    "-apilockstep","-warp","1","1","-skill","4","-nosound","-nomusic","-noblit"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
time.sleep(6)
fails = []
try:
    for b in (-30, 0, 30, 60):
        post("/api/world/objects", {"type":"IMP","distance":220,"bearing":b})
    for _ in range(4):
        post("/api/step", {"tics":10,"actions":[]})

    before = get("/api/sim")
    hunting = [m for m in before["mobjs"] if m["target"] != -1]
    print(f"  setup: rng=({before['rndindex']},{before['prndindex']}), "
          f"{len(before['mobjs'])} mobjs, {len(hunting)} holding a target, "
          f"{len(before['soundtargets'])} sectors with a sound target")
    if not hunting or before["prndindex"] == 0:
        print("  SKIP: the state does not contain what this test is about")
        sys.exit(2)

    post("/api/snapshot", {"slot":1})
    # Move the world well away from where it was.
    for _ in range(30):
        post("/api/step", {"tics":8,"actions":[{"type":"shoot"}]})
    post("/api/snapshot/restore", {"slot":1})
    after = get("/api/sim")

    def check(name, a, b):
        if a != b:
            fails.append(f"{name}: expected {a}, got {b}")

    check("gameplay RNG cursor", before["rndindex"], after["rndindex"])
    check("presentation RNG cursor", before["prndindex"], after["prndindex"])
    check("object identities", [m["id"] for m in before["mobjs"]],
                               [m["id"] for m in after["mobjs"]])
    check("monster targets", [(m["id"], m["target"]) for m in before["mobjs"]],
                             [(m["id"], m["target"]) for m in after["mobjs"]])
    check("missile tracers", [(m["id"], m["tracer"]) for m in before["mobjs"]],
                             [(m["id"], m["tracer"]) for m in after["mobjs"]])
    check("sector sound targets", before["soundtargets"], after["soundtargets"])

    for f in fails:
        print(f"  FAIL  {f}"[:200])
    print("PASS: the restore returned the simulation to where it was" if not fails
          else f"FAILED {len(fails)} of 6 invariants")
    sys.exit(1 if fails else 0)
finally:
    # SIGKILL: the engine does not always go on a polite one, and an engine
    # left holding the port makes the NEXT run measure a stale world while
    # reporting it as a fresh one.
    os.killpg(os.getpgid(eng.pid), signal.SIGKILL)

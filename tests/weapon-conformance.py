#!/usr/bin/env python3
"""Does asking to shoot actually fire the weapon in hand?

Calling P_FireWeapon directly restarts the attack state whenever asked,
bypassing A_WeaponReady and A_ReFire. For the rocket launcher, whose wind-up
is eight tics, restarting it every four spawns no missile at all - the agent
believes it attacked and nothing happened.
"""
import json, subprocess, time, urllib.request, os, signal, sys
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45989
WAD = "/data/workspace/applications/edgeai/brain/testdata/doom/doom1.wad"
def post(p,b):
    r = urllib.request.Request(f"http://localhost:{PORT}{p}", data=json.dumps(b).encode(),
        headers={"Content-Type":"application/json"}, method="POST")
    return json.loads(urllib.request.urlopen(r).read())
def get(p): return json.loads(urllib.request.urlopen(f"http://localhost:{PORT}{p}").read())

eng = subprocess.Popen(["./src/restful-doom","-iwad",WAD,"-apiport",str(PORT),"-apilockstep",
    "-warp","1","1","-skill","4","-nosound","-nomusic","-noblit"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
time.sleep(6)
try:
    # A target to shoot at, and ammo to spend.
    post("/api/world/objects", {"type":"IMP","distance":180,"bearing":0})
    before = get("/api/state")["player"]["ammo"]
    # Ask to shoot the way the sample does: every four tics.
    for _ in range(12):
        post("/api/step", {"tics":4,"actions":[{"type":"shoot"}]})
    after = get("/api/state")["player"]["ammo"]
    spent = before - after
    print(f"  pistol: {before} -> {after} rounds, {spent} actually fired over 12 requests")
    ok = spent > 0
    print("PASS: asking to shoot fires the weapon" if ok else
          "FAIL: twelve requests to shoot fired nothing")
    sys.exit(0 if ok else 1)
finally:
    os.killpg(os.getpgid(eng.pid), signal.SIGTERM)

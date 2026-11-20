#!/usr/bin/env python3
"""Writes soak.json: three cycles through all three levels (menu -> level select -> level ->
time up -> results -> menu), checking at every menu visit that node, 3D node, asset, animator,
tween and sound counts are back at their baseline, and profiling gameplay against a frame-time
budget. Standard library only:  python sample_projects/crate_rush/playtest/make_soak.py

Run the result with a window (frame times are real):
    crate_rush --playtest sample_projects/crate_rush/playtest/soak.json --vsync off
"""
import json
import os

# counts on the menu, 60 frames after it appears (the first visit included)
BASELINE = {"stat_nodes": 36, "stat_nodes3d": 47, "stat_assets": 21, "stat_animators": 1,
            "stat_tweens": 0, "stat_sounds": 0}
FRAME_BUDGET_MS = 16.7   # p95 of gameplay frames, Debug build, vsync off
CYCLES = 3
LEVELS = ["level1", "level2", "level3"]


def main():
    s = [{"cmd": "log", "message": "Crate Rush soak: %d cycles x %d levels" % (CYCLES, len(LEVELS))},
         {"cmd": "wait", "frames": 60}]
    for cycle in range(CYCLES):
        for index, level in enumerate(LEVELS):
            s.append({"cmd": "log", "message": "cycle %d, %s" % (cycle + 1, level)})
            s.append({"cmd": "expect", "path": "engine:scene", "value": "menu"})
            # jingles and clicks may still be playing: they must finish (a leaked voice never does). With an
            # audio device they play in real time while frames run uncapped, hence the long timeout.
            s.append({"cmd": "wait_until", "path": "game:stat_sounds", "value": 0, "timeout_frames": 5000})
            for key, value in BASELINE.items():
                s.append({"cmd": "expect", "path": "game:" + key, "value": value})
            # level select: LEVELS button, then the level's row
            s += [{"cmd": "action", "name": "nav_down"}, {"cmd": "wait", "frames": 2},
                  {"cmd": "expect", "path": "game:focus", "value": 1},
                  {"cmd": "action", "name": "confirm"}, {"cmd": "wait", "frames": 4},
                  {"cmd": "expect", "path": "levels_panel:visible", "value": True}]
            for _ in range(index):
                s += [{"cmd": "action", "name": "nav_down"}, {"cmd": "wait", "frames": 2}]
            s += [{"cmd": "expect", "path": "game:focus", "value": index},
                  {"cmd": "action", "name": "confirm"},
                  {"cmd": "wait_until", "path": "game:state", "value": "playing", "timeout_frames": 600},
                  {"cmd": "expect", "path": "engine:scene", "value": level},
                  {"cmd": "wait", "frames": 5},
                  {"cmd": "key", "key": "D", "action": "down"},
                  {"cmd": "wait", "frames": 20},
                  {"cmd": "key", "key": "D", "action": "up"},
                  {"cmd": "expect", "path": "*/player:position.x", "op": ">", "value": 0.5},
                  {"cmd": "expect", "path": "game:stat_animators", "op": ">=", "value": 1}]
            if cycle == 0:
                s += [{"cmd": "profile", "frames": 240},
                      {"cmd": "log", "message": "profiled %s" % level},
                      {"cmd": "expect", "path": "profile:frame_ms_p95", "op": "<", "value": FRAME_BUDGET_MS}]
            s += [{"cmd": "set", "path": "game:time_left", "value": 0.5},
                  {"cmd": "wait_until", "path": "engine:scene", "value": "results", "timeout_frames": 400},
                  {"cmd": "expect", "path": "game:stat_nodes3d", "value": 0},
                  {"cmd": "wait", "frames": 40},
                  {"cmd": "action", "name": "back"},
                  {"cmd": "wait_until", "path": "engine:scene", "value": "menu", "timeout_frames": 400},
                  {"cmd": "wait", "frames": 60}]
    s.append({"cmd": "expect", "path": "game:levels_completed", "value": CYCLES * len(LEVELS)})
    s.append({"cmd": "wait_until", "path": "game:stat_sounds", "value": 0, "timeout_frames": 5000})
    for key, value in BASELINE.items():
        s.append({"cmd": "expect", "path": "game:" + key, "value": value})
    s.append({"cmd": "quit"})
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "soak.json")
    with open(path, "w") as f:
        f.write("[\n" + ",\n".join("  " + json.dumps(cmd) for cmd in s) + "\n]\n")
    print("wrote %s (%d steps)" % (path, len(s)))


if __name__ == "__main__":
    main()

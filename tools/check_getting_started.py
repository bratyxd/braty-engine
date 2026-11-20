#!/usr/bin/env python3
"""Builds and runs every step of docs/GETTING_STARTED.md. Standard library only.

    python tools/check_getting_started.py [--only N] [--gpu-driver NAME] [--compiler gcc] [--sdl-prefix DIR]

Each "## N. Title" section's "<!-- file: PATH -->" code blocks are written into a scratch
project (build/getting_started/my_game/), on top of the earlier steps' files, exactly as a reader
would copy them. The guide's asset copies (steps 8 and 15) are done from the repository. From
step 3 on, every step is built (one CMake build directory, so steps rebuild incrementally) and run
with --frames 60 --screenshot build/getting_started/step_N.png. A step that writes a CMakeLists
with braty_install_game is also installed (cmake --install) and the installed game run from
another directory; a step that writes playtest/*.json runs each script with --headless. Exit code
1 if a step fails to build, exits non-zero, or logs an ERROR line. Open the PNGs to check what
each step shows.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUIDE = os.path.join(ROOT, "docs", "GETTING_STARTED.md")
OUT = os.path.join(ROOT, "build", "getting_started")
GAME = os.path.join(OUT, "my_game")

# the shell copies the guide asks for, by the step they come before
COPIES = {
    8: [("sample_projects/star_courier/data/assets/sprites", "data/assets/sprites"),
        ("sample_projects/star_courier/data/assets/sounds", "data/assets/sounds")],
    15: [("sample_projects/crate_rush/data/assets/levels/level1.glb", "data/assets/levels/level1.glb"),
         ("sample_projects/crate_rush/data/assets/models/character", "data/assets/models/character")],
}


def parse_steps(text):
    """[(number, title, [(path, content)])] for every "## N." section."""
    steps = []
    current = None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^## (\d+)\. (.*)", line)
        if m:
            current = (int(m.group(1)), m.group(2), [])
            steps.append(current)
        elif line.startswith("## "):
            current = None
        m = re.match(r"^<!-- file: (\S+) -->", line)
        if m and current is not None:
            i += 1
            assert lines[i].startswith("```"), "a file marker must be followed by a code block"
            body = []
            i += 1
            while not lines[i].startswith("```"):
                body.append(lines[i])
                i += 1
            current[2].append((m.group(1), "\n".join(body) + "\n"))
        i += 1
    return steps


def copy(src, dst):
    src = os.path.join(ROOT, src)
    dst = os.path.join(GAME, dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isdir(src):
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        shutil.copy2(src, dst)


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", type=int, action="append", help="report only these steps (all are applied)")
    ap.add_argument("--gpu-driver", default=None)
    ap.add_argument("--compiler", default="gcc", help="CMAKE_C_COMPILER for the scratch project")
    ap.add_argument("--sdl-prefix", default=os.path.join(ROOT, ".deps", "sdl3"), help="SDL3 install prefix")
    args = ap.parse_args()
    with open(GUIDE, encoding="utf-8") as f:
        steps = parse_steps(f.read())
    if os.path.isdir(GAME):
        shutil.rmtree(GAME)
    os.makedirs(os.path.join(GAME, "data"), exist_ok=True)
    build = os.path.join(OUT, "build")
    configured = False
    failures = 0
    for number, title, files in steps:
        for src, dst in COPIES.get(number, []):
            copy(src, dst)
        for path, content in files:
            target = os.path.join(GAME, path)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "w", encoding="utf-8") as f:
                f.write(content)
        if not os.path.exists(os.path.join(GAME, "src", "main.c")):
            print("step %2d  %-48s files written (nothing to build yet)" % (number, title))
            continue
        if args.only and number not in args.only:
            continue
        if not configured:
            result = run(["cmake", "-S", GAME, "-B", build, "-G", "Ninja", "-DCMAKE_C_COMPILER=" + args.compiler,
                          "-DBRATY_DIR=" + ROOT, "-DCMAKE_PREFIX_PATH=" + args.sdl_prefix])
            if result.returncode != 0:
                print(result.stdout + result.stderr)
                return 1
            configured = True
        result = run(["cmake", "--build", build])
        warnings = [l for l in result.stdout.splitlines() if "my_game" in l and "warning:" in l]
        if result.returncode != 0:
            print("step %2d  %-48s BUILD FAILED\n%s" % (number, title, result.stdout[-3000:]))
            failures += 1
            continue
        exe = os.path.join(build, "my_game.exe" if os.name == "nt" else "my_game")
        shot = os.path.join(OUT, "step_%d.png" % number)
        cmd = [exe, "--frames", "60", "--screenshot", shot]
        if args.gpu_driver:
            cmd += ["--gpu-driver", args.gpu_driver]
        runs = [(cmd, None)]
        errors = []
        if any(path == "CMakeLists.txt" and "braty_install_game" in content for path, content in files):
            # the shipped folder (exe + game.json + assets.brpack), run from another directory
            dist = os.path.join(OUT, "dist")
            shutil.rmtree(dist, ignore_errors=True)
            result = run(["cmake", "--install", build, "--prefix", dist])
            if result.returncode != 0:
                errors.append("cmake --install failed: " + (result.stdout + result.stderr)[-2000:])
            installed = os.path.join(dist, "my_game", os.path.basename(exe))
            shot_installed = os.path.join(OUT, "step_%d_installed.png" % number)
            runs.append(([installed, "--frames", "60", "--screenshot", shot_installed] + cmd[4:], OUT))
        for path, _ in files:
            if path.startswith("playtest/") and path.endswith(".json"):
                runs.append(([exe, "--headless", "--playtest", os.path.join(GAME, path)], GAME))
        ok = not errors
        for run_cmd, cwd in runs:
            try:
                result = run(run_cmd, timeout=120, cwd=cwd)
            except OSError as err:
                errors.append("%s: %s" % (run_cmd[0], err))
                ok = False
                continue
            log = result.stdout + result.stderr
            errors += [l for l in log.splitlines() if "ERROR" in l]
            ok = ok and result.returncode == 0
        ok = ok and not errors and not warnings and os.path.exists(shot)
        print("step %2d  %-48s %s" % (number, title, "ok  " + os.path.relpath(shot, ROOT) if ok else "FAILED"))
        for line in warnings + errors:
            print("         " + line)
        failures += 0 if ok else 1
    print("getting started: %s" % ("all steps build and run" if failures == 0 else "%d step(s) failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

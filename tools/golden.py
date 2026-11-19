#!/usr/bin/env python3
"""Golden-image regression test: runs every example and the sample game through a play-test
script under --deterministic, takes a downsampled screenshot and compares it with
tests/golden/<name>.png. Standard library only.

    python tools/golden.py run     [--build-dir build/windows-gcc] [--gpu-driver direct3d12|vulkan] [--only NAME]
    python tools/golden.py update  [...]      (re-create the goldens; review the PNGs before committing)

run exits 0 when every image matches within the tolerance, 1 on any mismatch (diff images are
written next to the new screenshots in build/golden/), and 77 (CTest "skipped") when no GPU
device or window can be created. docs/TESTING.md says when an update is legitimate.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLDEN_DIR = os.path.join(ROOT, "tests", "golden")
DOWNSAMPLE = 4          # 1280x720 -> 320x180 keeps the PNGs small
CHANNEL_TOLERANCE = 6   # per channel, 0..255, after the 4x4 average
BAD_PIXEL_SHARE = 0.002 # up to 0.2% of pixels may exceed the channel tolerance
SKIP = 77

# name, executable (relative to the build dir), frame of the screenshot, extra arguments, base script
STAR_COURIER = "sample_projects/star_courier/star_courier"
ROUND = os.path.join(ROOT, "sample_projects", "star_courier", "playtest", "round.json")
CASES = [
    ("hello_quad", "hello_quad", 30, [], None),
    ("graphics_demo", "graphics_demo", 30, [], None),
    ("text_demo", "text_demo", 30, [], None),
    ("assets_demo", "assets_demo", 60, [], None),
    ("sprites_demo", "sprites_demo", 60, [], None),
    ("sprites", "sprites", 30, ["--count", "2000"], None),
    ("mesh_demo", "mesh_demo", 60, [], None),
    ("particles_demo", "particles_demo", 60, [], None),
    ("events_demo", "events_demo", 30, [], None),
    ("audio_demo", "audio_demo", 30, [], None),
    ("filters_demo", "filters_demo", 60, [], None),
    ("data_demo", "data_demo", 60, [], None),
    ("animation_demo", "animation_demo", 90, [], None),
    ("profiler_demo", "profiler_demo", 60, [], None),
    ("camera_demo", "camera_demo", 60, [], None),
    ("canvas3d_demo", "canvas3d_demo", 60, [], None),
    ("scenes_demo", "scenes_demo", 190, [], None),
    ("view3d_demo", "view3d_demo", 60, [], None),
    ("gltf_demo", "gltf_demo", 90, [], None),
    ("gltf_scene_demo", "gltf_scene_demo", 90, [], None),
    ("star_courier_title", STAR_COURIER, 60, [], ROUND),
    ("star_courier_play", STAR_COURIER, 700, [], ROUND),
]


# ---- PNG (8-bit RGB / RGBA, non-interlaced: what stb_image_write and this file produce)

def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat = 8, b""
    width = height = channels = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6) or interlace != 0:
                raise ValueError("%s: only 8-bit RGB/RGBA non-interlaced PNGs" % path)
            channels = 4 if color == 6 else 3
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(height * width * 4)
    prev = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if kind == 1:
                row[i] = (row[i] + a) & 255
            elif kind == 2:
                row[i] = (row[i] + b) & 255
            elif kind == 3:
                row[i] = (row[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                row[i] = (row[i] + pred) & 255
        for x in range(width):
            src = x * channels
            dst = (y * width + x) * 4
            out[dst:dst + 3] = row[src:src + 3]
            out[dst + 3] = row[src + 3] if channels == 4 else 255
        prev = row
    return width, height, bytes(out)


def write_png(path, width, height, rgba):
    """RGB, no filter, zlib level 9: about a third smaller than the engine's RGBA screenshots."""
    rgb = bytearray(width * height * 3)
    rgb[0::3] = rgba[0::4]
    rgb[1::3] = rgba[1::4]
    rgb[2::3] = rgba[2::4]
    rows = b"".join(b"\x00" + bytes(rgb[y * width * 3:(y + 1) * width * 3]) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        f.write(chunk(b"IEND", b""))


def compare(golden_path, actual_path, diff_path):
    """Returns (ok, message). Writes a diff image (red = over tolerance) when not ok."""
    gw, gh, g = read_png(golden_path)
    aw, ah, a = read_png(actual_path)
    if (gw, gh) != (aw, ah):
        return False, "size %dx%d, golden %dx%d" % (aw, ah, gw, gh)
    bad = 0
    worst = 0
    diff = bytearray(len(g))
    for i in range(0, len(g), 4):
        d = max(abs(g[i] - a[i]), abs(g[i + 1] - a[i + 1]), abs(g[i + 2] - a[i + 2]))
        worst = max(worst, d)
        if d > CHANNEL_TOLERANCE:
            bad += 1
            diff[i:i + 4] = b"\xff\x00\x00\xff"
        else:
            grey = (g[i] + g[i + 1] + g[i + 2]) // 9  # dimmed golden for orientation
            diff[i:i + 4] = bytes((grey, grey, grey, 255))
    share = bad / (gw * gh)
    message = "%d pixels over tolerance (%.3f%%), worst channel difference %d" % (bad, 100.0 * share, worst)
    if share > BAD_PIXEL_SHARE:
        write_png(diff_path, gw, gh, bytes(diff))
        return False, message
    return True, message


# ---- running the games

def script_for(frame, shot, base):
    """Commands that reach `frame` (following `base`'s input if given), screenshot, quit."""
    commands = []
    if base:
        with open(base) as f:
            source = json.load(f)
        reached = 1  # a script starts running in frame 1
        for cmd in source:
            if cmd["cmd"] == "quit":
                break
            if cmd["cmd"] == "wait":
                frames = cmd.get("frames", 1)
                if reached + frames >= frame:
                    break
                reached += frames
            if cmd["cmd"] not in ("expect", "log"):  # the golden checks pixels; round.json checks state
                commands.append(cmd)
        commands.append({"cmd": "wait", "frames": frame - reached})
    else:
        commands.append({"cmd": "wait", "frames": frame - 1})
    commands.append({"cmd": "wait", "frames": 1})
    commands.append({"cmd": "screenshot", "file": shot, "downsample": DOWNSAMPLE})
    commands.append({"cmd": "quit"})
    return commands


def run_case(case, build_dir, out_dir, driver):
    name, exe, frame, extra, base = case
    exe_path = os.path.join(build_dir, exe + (".exe" if os.name == "nt" else ""))
    shot = os.path.join(out_dir, name + ".png")
    script = os.path.join(out_dir, name + ".script.json")
    with open(script, "w") as f:
        json.dump(script_for(frame, shot, base), f, indent=1)
    if os.path.exists(shot):
        os.remove(shot)
    args = [exe_path, "--playtest", script, "--vsync", "off"] + extra
    if driver:
        args += ["--gpu-driver", driver]
    try:
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300)
    except FileNotFoundError:
        return None, "not built: %s" % exe_path
    except subprocess.TimeoutExpired:
        return None, "timed out"
    log = result.stderr + result.stdout
    if "cannot create GPU device" in log or "cannot create window" in log:
        return SKIP, "no GPU device / window"
    if result.returncode != 0 or not os.path.exists(shot):
        tail = "\n".join(log.strip().splitlines()[-5:])
        return None, "exit code %d\n%s" % (result.returncode, tail)
    return shot, "ok"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["run", "update"])
    parser.add_argument("--build-dir", default=os.path.join(ROOT, "build", "windows-gcc"))
    parser.add_argument("--gpu-driver", default=None, help="direct3d12 (the goldens' driver) or vulkan")
    parser.add_argument("--only", action="append", help="run only these cases (repeatable)")
    parser.add_argument("--out", default=None, help="where new screenshots and diffs go (default build/golden)")
    args = parser.parse_args()
    out_dir = args.out or os.path.join(ROOT, "build", "golden")
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    cases = [c for c in CASES if not args.only or c[0] in args.only]
    failures = 0
    for case in cases:
        name = case[0]
        shot, message = run_case(case, args.build_dir, out_dir, args.gpu_driver)
        if shot == SKIP:
            print("golden: SKIPPED, %s" % message)
            return SKIP
        if shot is None:
            print("FAIL  %-20s %s" % (name, message))
            failures += 1
            continue
        golden = os.path.join(GOLDEN_DIR, name + ".png")
        if args.mode == "update":
            width, height, pixels = read_png(shot)
            write_png(golden, width, height, pixels)
            print("wrote %s" % os.path.relpath(golden, ROOT))
            continue
        if not os.path.exists(golden):
            print("FAIL  %-20s no golden (python tools/golden.py update --only %s)" % (name, name))
            failures += 1
            continue
        ok, message = compare(golden, shot, os.path.join(out_dir, name + ".diff.png"))
        print("%s  %-20s %s" % ("ok  " if ok else "FAIL", name, message))
        failures += 0 if ok else 1
    if args.mode == "run":
        print("golden: %d of %d failed" % (failures, len(cases)) if failures else "golden: all %d match" % len(cases))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

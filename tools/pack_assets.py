#!/usr/bin/env python3
"""Builds a .brpack from a data folder (the format: docs/DATA_FORMATS.md, "Asset packs").

    python tools/pack_assets.py sample_projects/crate_rush/data/assets build/assets.brpack
    python tools/pack_assets.py --list build/assets.brpack

Every file under the folder becomes one entry, its path relative to the folder with forward
slashes (the folder is the game's asset root). The output is deterministic: entries sorted by
path bytes, no timestamps, so the same folder always gives the same bytes. Standard library only.
Refuses names the engine would refuse (control characters, longer than 255 bytes) and two paths
that differ only in case (they collide on Windows and macOS).
"""
import argparse
import os
import struct
import sys

MAGIC = b"BRPK"
VERSION = 1
HEADER = struct.Struct("<4sIII")       # magic, version, entry count, table-of-contents size
ENTRY = struct.Struct("<QQQH")         # offset, size, FNV-1a 64 hash, path length
PATH_MAX = 255


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def collect(folder, skip):
    """[(path bytes, absolute file)] for every file under folder, sorted by path bytes."""
    files = []
    for root, dirs, names in os.walk(folder):
        dirs.sort()
        for name in names:
            full = os.path.join(root, name)
            if os.path.abspath(full) in skip:
                continue
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            path = rel.encode("utf-8")
            if len(path) > PATH_MAX or any(c < 32 for c in path):
                raise SystemExit("pack_assets: bad path (control characters or over %d bytes): %r" % (PATH_MAX, rel))
            files.append((path, full))
    files.sort()
    seen = {}
    for path, _ in files:
        folded = path.lower()
        if folded in seen:
            raise SystemExit("pack_assets: '%s' and '%s' differ only in case" % (seen[folded].decode(), path.decode()))
        seen[folded] = path
    return files


def build(folder, out_path):
    skip = {os.path.abspath(out_path)}
    files = collect(folder, skip)
    blobs = []
    for path, full in files:
        with open(full, "rb") as f:
            blobs.append(f.read())
    toc_size = sum(ENTRY.size + len(path) for path, _ in files)
    offset = HEADER.size + toc_size
    toc = bytearray()
    for (path, _), data in zip(files, blobs):
        toc += ENTRY.pack(offset, len(data), fnv1a64(data), len(path)) + path
        offset += len(data)
    if toc_size > 0xFFFFFFFF or len(files) > 0xFFFFFFFF:
        raise SystemExit("pack_assets: table of contents too large")
    tmp = out_path + ".tmp"
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(tmp, "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, len(files), toc_size))
        f.write(toc)
        for data in blobs:
            f.write(data)
    os.replace(tmp, out_path)
    print("pack_assets: %s: %d entries, %d bytes" % (out_path, len(files), offset))


def list_pack(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, count, toc_size = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        raise SystemExit("pack_assets: %s: not a version %d .brpack" % (path, VERSION))
    at = HEADER.size
    for _ in range(count):
        offset, size, digest, length = ENTRY.unpack_from(data, at)
        at += ENTRY.size
        name = data[at:at + length].decode("utf-8", "replace")
        at += length
        ok = fnv1a64(data[offset:offset + size]) == digest
        print("%10d  %016x %s  %s" % (size, digest, "ok " if ok else "BAD", name))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--list", metavar="PACK", help="print the entries of a pack and check their hashes")
    parser.add_argument("folder", nargs="?", help="data folder (the asset root)")
    parser.add_argument("out", nargs="?", help="output .brpack")
    args = parser.parse_args()
    if args.list:
        list_pack(args.list)
        return 0
    if not args.folder or not args.out:
        parser.error("folder and out are required")
    if not os.path.isdir(args.folder):
        raise SystemExit("pack_assets: '%s' is not a folder" % args.folder)
    build(args.folder, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

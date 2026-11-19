"""Writes tests/data/tiny.gltf: a small glTF with every buffer and image embedded as base64.

Run: python tests/data/make_tiny_gltf.py   (stdlib only; the output is committed)

Contents: one textured triangle (no normals/tangents, so the loader generates them) under a
"Root" node with extras, a node given as a matrix, a camera, a point light, a material using
KHR_texture_transform + KHR_materials_emissive_strength, an unknown optional extension, and a
linear translation animation "Move" (0 s -> 1 s: x 0 -> 2).
"""
import base64
import json
import os
import struct
import zlib


def png_2x2():
    rows = [b"\x00" + bytes([255, 0, 0, 255, 0, 255, 0, 255]), b"\x00" + bytes([0, 0, 255, 255, 255, 255, 255, 255])]
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
uvs = struct.pack("<6f", 0, 1, 1, 1, 0, 0)
indices = struct.pack("<3H", 0, 1, 2) + b"\x00\x00"
times = struct.pack("<2f", 0, 1)
moves = struct.pack("<6f", 0, 0, 0, 2, 0, 0)
blob = positions + uvs + indices + times + moves
o_uv = len(positions)
o_idx = o_uv + len(uvs)
o_time = o_idx + len(indices)
o_move = o_time + len(times)

doc = {
    "asset": {"version": "2.0", "generator": "make_tiny_gltf.py"},
    "extensionsUsed": ["KHR_texture_transform", "KHR_materials_emissive_strength", "KHR_lights_punctual", "EXT_tiny_unknown"],
    "extensions": {"KHR_lights_punctual": {"lights": [{"type": "point", "color": [1, 0.5, 0.25], "intensity": 3, "range": 10}]}},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [
        {"name": "Root", "children": [1, 2, 3, 4], "extras": {"components": {"health": {"hp": 5}}}},
        {"name": "Tri", "mesh": 0, "translation": [1, 2, 3]},
        {"name": "Turned", "matrix": [0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 4, 5, 6, 1]},
        {"name": "Cam", "camera": 0},
        {"name": "Lamp", "extensions": {"KHR_lights_punctual": {"light": 0}}},
    ],
    "cameras": [{"type": "perspective", "perspective": {"yfov": 0.8, "znear": 0.1, "zfar": 50, "aspectRatio": 1.5}}],
    "meshes": [{"name": "TriMesh", "primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
    "materials": [{
        "name": "Red",
        "pbrMetallicRoughness": {"baseColorFactor": [1, 0.5, 0.5, 1], "metallicFactor": 0.25, "roughnessFactor": 0.75,
                                 "baseColorTexture": {"index": 0, "extensions": {"KHR_texture_transform": {"offset": [0.5, 0], "scale": [2, 2]}}}},
        "emissiveFactor": [1, 1, 1],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4}},
        "alphaMode": "MASK", "alphaCutoff": 0.3, "doubleSided": True,
    }],
    "textures": [{"source": 0, "sampler": 0}],
    "samplers": [{"magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071}],
    "images": [{"uri": "data:image/png;base64," + base64.b64encode(png_2x2()).decode()}],
    "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(positions)},
        {"buffer": 0, "byteOffset": o_uv, "byteLength": len(uvs)},
        {"buffer": 0, "byteOffset": o_idx, "byteLength": 6},
        {"buffer": 0, "byteOffset": o_time, "byteLength": len(times)},
        {"buffer": 0, "byteOffset": o_move, "byteLength": len(moves)},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
        {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
        {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"},
        {"bufferView": 3, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
        {"bufferView": 4, "componentType": 5126, "count": 2, "type": "VEC3"},
    ],
    "animations": [{"name": "Move", "samplers": [{"input": 3, "output": 4, "interpolation": "LINEAR"}],
                    "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]}],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tiny.gltf")
with open(out, "w", newline="\n") as f:
    json.dump(doc, f, indent=1)
    f.write("\n")
print("wrote", out)

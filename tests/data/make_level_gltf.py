"""Writes the glTF level fixtures (stdlib only; the outputs are committed):

    tests/data/level.gltf            the test level (JSON + base64 buffer), with a "BadValues" node
    examples/gltf_scene/level.glb    the same level as a binary .glb, without the bad node (demo)
    examples/gltf_scene/kit.glb      a prop kit: one root node per prop (Tree, Rock, Crate, Lamp_Post)

Run from the repository root: python tests/data/make_level_gltf.py

The level is what a Blender export of a small authored level looks like: a ground with a
"-col" name suffix (collider), eight repeated static trees (batched automatically), six rocks in
one EXT_mesh_gpu_instancing node, four spinning coins whose gameplay data is node extras (flat
"spin.speed" keys as Blender custom properties export them, and one nested "components" object),
a node with every component field type, a trigger and a "no render" helper, a node on layer 2,
a windmill whose blades are animated ("Spin", autoplayed from the scene extras), two cameras and
two KHR_lights_punctual lights, materials using emissive strength, unlit and transmission.
"""
import base64
import json
import math
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# ---- geometry: (positions, normals, indices) lists, CCW front faces, y up

def box(sx, sy, sz, cy=0.0):
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    pos, nor, idx = [], [], []
    for n, u, v in faces:
        base = len(pos)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [n[k] * (hx, hy, hz)[k] + su * u[k] * (hx, hy, hz)[k] + sv * v[k] * (hx, hy, hz)[k] for k in range(3)]
            p[1] += cy
            pos.append(p)
            nor.append(list(n))
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nor, idx


def cylinder(r_bottom, r_top, h, segments=12, cy=0.0):
    """Side + caps along y from cy to cy + h (r_top 0 = cone)."""
    pos, nor, idx = [], [], []
    slope = (r_bottom - r_top) / h
    for i in range(segments + 1):
        a = 2 * math.pi * i / segments
        c, s = math.cos(a), math.sin(a)
        ln = math.sqrt(1 + slope * slope)
        n = [c / ln, slope / ln, s / ln]
        pos.append([c * r_bottom, cy, s * r_bottom])
        nor.append(n)
        pos.append([c * r_top, cy + h, s * r_top])
        nor.append(n)
    for i in range(segments):
        a, b, c2, d = 2 * i, 2 * i + 1, 2 * i + 2, 2 * i + 3
        idx += [a, b, d, a, d, c2]
    for y, r, ny in ((cy, r_bottom, -1), (cy + h, r_top, 1)):
        if r <= 0:
            continue
        center = len(pos)
        pos.append([0, y, 0])
        nor.append([0, ny, 0])
        for i in range(segments + 1):
            a = 2 * math.pi * i / segments
            pos.append([math.cos(a) * r, y, math.sin(a) * r])
            nor.append([0, ny, 0])
        for i in range(segments):
            if ny > 0:
                idx += [center, center + 2 + i, center + 1 + i]
            else:
                idx += [center, center + 1 + i, center + 2 + i]
    return pos, nor, idx


# ---- quaternions

def quat_axis(axis, degrees):
    a = math.radians(degrees) / 2
    s = math.sin(a)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(a)]


def quat_look(eye, target):
    """Rotation whose -Z axis points from eye to target (y up), as [x, y, z, w]."""
    f = [target[i] - eye[i] for i in range(3)]
    fl = math.sqrt(sum(v * v for v in f))
    z = [-v / fl for v in f]                     # camera +Z points backwards
    up = [0, 1, 0]
    x = [up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]]
    xl = math.sqrt(sum(v * v for v in x))
    x = [v / xl for v in x]
    y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]]
    m00, m11, m22 = x[0], y[1], z[2]
    trace = m00 + m11 + m22
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        return [(y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s, s / 4]
    if m00 > m11 and m00 > m22:
        s = math.sqrt(1 + m00 - m11 - m22) * 2
        return [s / 4, (y[0] + x[1]) / s, (z[0] + x[2]) / s, (y[2] - z[1]) / s]
    if m11 > m22:
        s = math.sqrt(1 + m11 - m00 - m22) * 2
        return [(y[0] + x[1]) / s, s / 4, (z[1] + y[2]) / s, (z[0] - x[2]) / s]
    s = math.sqrt(1 + m22 - m00 - m11) * 2
    return [(z[0] + x[2]) / s, (z[1] + y[2]) / s, s / 4, (x[1] - y[0]) / s]


# ---- document builder

class Doc:
    def __init__(self):
        self.blob = bytearray()
        self.d = {"asset": {"version": "2.0", "generator": "make_level_gltf.py"},
                  "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [],
                  "accessors": [], "bufferViews": [], "buffers": []}

    def view(self, data):
        while len(self.blob) % 4:
            self.blob.append(0)
        self.d["bufferViews"].append({"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)})
        self.blob += data
        return len(self.d["bufferViews"]) - 1

    def accessor(self, values, kind, components):
        flat = [v for item in values for v in (item if isinstance(item, list) else [item])]
        acc = {"bufferView": self.view(struct.pack("<%df" % len(flat), *flat)), "componentType": 5126,
               "count": len(values), "type": kind}
        if kind == "VEC3" and components == 3:
            acc["min"] = [min(v[k] for v in values) for k in range(3)]
            acc["max"] = [max(v[k] for v in values) for k in range(3)]
        if kind == "SCALAR":
            acc["min"] = [min(values)]
            acc["max"] = [max(values)]
        self.d["accessors"].append(acc)
        return len(self.d["accessors"]) - 1

    def indices(self, idx):
        self.d["accessors"].append({"bufferView": self.view(struct.pack("<%dI" % len(idx), *idx)),
                                    "componentType": 5125, "count": len(idx), "type": "SCALAR"})
        return len(self.d["accessors"]) - 1

    def material(self, name, color, **extra):
        m = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": color, "metallicFactor": 0.0,
                                                   "roughnessFactor": extra.pop("roughness", 0.8)}}
        m.update(extra)
        self.d["materials"].append(m)
        return len(self.d["materials"]) - 1

    def mesh(self, name, parts):
        prims = []
        for (pos, nor, idx), mat in parts:
            prims.append({"attributes": {"POSITION": self.accessor(pos, "VEC3", 3), "NORMAL": self.accessor(nor, "VEC3", 3)},
                          "indices": self.indices(idx), "material": mat})
        self.d["meshes"].append({"name": name, "primitives": prims})
        return len(self.d["meshes"]) - 1

    def node(self, name, parent=None, **fields):
        n = {"name": name}
        n.update(fields)
        self.d["nodes"].append(n)
        index = len(self.d["nodes"]) - 1
        if parent is None:
            self.d["scenes"][0]["nodes"].append(index)
        else:
            self.d["nodes"][parent].setdefault("children", []).append(index)
        return index

    def use(self, ext):
        used = self.d.setdefault("extensionsUsed", [])
        if ext not in used:
            used.append(ext)

    def gltf_text(self):
        d = dict(self.d)
        d["buffers"] = [{"byteLength": len(self.blob),
                         "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(self.blob)).decode()}]
        return json.dumps(d, indent=1)

    def glb_bytes(self):
        d = dict(self.d)
        d["buffers"] = [{"byteLength": len(self.blob)}]
        text = json.dumps(d, separators=(",", ":")).encode()
        while len(text) % 4:
            text += b" "
        blob = bytes(self.blob)
        while len(blob) % 4:
            blob += b"\0"
        total = 12 + 8 + len(text) + 8 + len(blob)
        return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(text), b"JSON") + text +
                struct.pack("<I4s", len(blob), b"BIN\0") + blob)


def build_level(with_bad_node):
    doc = Doc()
    grass = doc.material("Grass", [0.25, 0.55, 0.2, 1])
    bark = doc.material("Bark", [0.35, 0.2, 0.1, 1])
    leaves = doc.material("Leaves", [0.1, 0.45, 0.15, 1])
    stone = doc.material("Stone", [0.5, 0.5, 0.52, 1], roughness=0.95)
    gold = doc.material("Gold", [1.0, 0.75, 0.2, 1], roughness=0.3, emissiveFactor=[1.0, 0.6, 0.1],
                        extensions={"KHR_materials_emissive_strength": {"emissiveStrength": 0.6}})
    wood = doc.material("Wood", [0.75, 0.6, 0.4, 1])
    sail = doc.material("Sail", [0.95, 0.95, 0.9, 1], extensions={"KHR_materials_unlit": {}})
    glass = doc.material("Glass", [0.6, 0.85, 1.0, 1], roughness=0.1,
                         extensions={"KHR_materials_transmission": {"transmissionFactor": 0.8}})
    for ext in ("KHR_materials_emissive_strength", "KHR_materials_unlit", "KHR_materials_transmission",
                "KHR_lights_punctual", "EXT_mesh_gpu_instancing"):
        doc.use(ext)

    ground_mesh = doc.mesh("GroundMesh", [(box(24, 0.4, 24, -0.2), grass)])
    tree_mesh = doc.mesh("TreeMesh", [(cylinder(0.18, 0.14, 1.0), bark), (cylinder(0.8, 0.0, 2.0, 12, 0.8), leaves)])
    rock_mesh = doc.mesh("RockMesh", [(box(0.8, 0.5, 0.7, 0.25), stone)])
    coin_mesh = doc.mesh("CoinMesh", [(cylinder(0.35, 0.35, 0.08, 16, -0.04), gold)])
    tower_mesh = doc.mesh("TowerMesh", [(cylinder(0.9, 0.5, 4.0), wood)])
    blade_mesh = doc.mesh("BladeMesh", [(box(0.3, 3.6, 0.05), sail), (box(3.6, 0.3, 0.05), sail)])
    gate_mesh = doc.mesh("GateMesh", [(box(2, 2, 0.2, 1), glass)])
    window_mesh = doc.mesh("WindowMesh", [(box(1.2, 1.2, 0.05, 0.6), glass)])

    doc.d["extensions"] = {"KHR_lights_punctual": {"lights": [
        {"name": "Sun", "type": "directional", "color": [1.0, 0.95, 0.85], "intensity": 2.2},
        {"name": "Lamp", "type": "point", "color": [1.0, 0.7, 0.4], "intensity": 4.0, "range": 8.0}]}}
    doc.d["cameras"] = [
        {"name": "MainCam", "type": "perspective", "perspective": {"yfov": 0.75, "znear": 0.1, "zfar": 200, "aspectRatio": 1.7778}},
        {"name": "TopCam", "type": "orthographic", "orthographic": {"xmag": 16, "ymag": 9, "znear": 0.1, "zfar": 100}}]

    doc.node("Ground-col", mesh=ground_mesh)
    tree_spots = [(-8, -6), (-5, -8), (-9, 1), (-6, 6), (7, -7), (9, -2), (8, 5), (4, 8)]
    for i, (x, z) in enumerate(tree_spots):
        doc.node("Tree_%02d" % (i + 1), mesh=tree_mesh, translation=[x, 0, z], scale=[1 + 0.1 * (i % 3)] * 3,
                 rotation=quat_axis((0, 1, 0), 37 * i))
    rocks = [[-3, 0, -4], [-2.2, 0, -4.6], [3, 0, 3.5], [3.8, 0, 2.8], [-4, 0, 4], [5.5, 0, -4]]
    rock_t = doc.accessor(rocks, "VEC3", 3)
    rock_r = doc.accessor([quat_axis((0, 1, 0), 50 * i) for i in range(len(rocks))], "VEC4", 4)
    rock_s = doc.accessor([[1 + 0.2 * (i % 3)] * 3 for i in range(len(rocks))], "VEC3", 3)
    doc.node("Rocks", mesh=rock_mesh,
             extensions={"EXT_mesh_gpu_instancing": {"attributes": {"TRANSLATION": rock_t, "ROTATION": rock_r, "SCALE": rock_s}}})

    coins = [(-2, 2), (2, 2), (2, -2), (-2, -2)]
    for i, (x, z) in enumerate(coins):
        if i == 0:
            extras = {"components": {"spin": {"speed": 3.0}, "pickup": {"value": 25, "kind": "big"}}}
        else:
            extras = {"spin.speed": 1.5 + 0.5 * i, "pickup.value": 10, "pickup.kind": "small"}
        doc.node("Coin_%02d" % (i + 1), mesh=coin_mesh, translation=[x, 1.0, z],
                 rotation=quat_axis((1, 0, 0), 90), extras=extras)

    windmill = doc.node("Windmill", mesh=tower_mesh, translation=[0, 0, -7])
    blades = doc.node("Blades", parent=windmill, mesh=blade_mesh, translation=[0, 3.4, 0.75])
    doc.node("Window", parent=windmill, mesh=window_mesh, translation=[0, 1.2, 0.72])

    doc.node("SpawnPoint", translation=[0, 0, 4], extras={"spawn_point": True})
    doc.node("Props", translation=[1, 0, 1], extras={"components": {"all": {
        "b": True, "i": 7, "f": 1.5, "v2": [1, 2], "v3": [1, 2, 3], "c": "#ff0000", "s": "hello",
        "a": "tiny.gltf", "n": "Tree_03"}}})
    doc.node("FlatProps", extras={"all.b": 1, "all.i": 3, "all.f": 0.25, "all.v2": [5, 6], "all.v3": [7, 8, 9],
                                  "all.c": [0, 1, 0], "all.s": "flat", "all.a": "level.gltf", "all.n": "Windmill",
                                  "blender_only_prop": "ignored"})
    doc.node("Gate-trigger", mesh=gate_mesh, translation=[0, 0, 6])
    doc.node("Helper-norender", mesh=rock_mesh, translation=[0, 0, 10])
    doc.node("Layered", mesh=rock_mesh, translation=[6, 0, 0], extras={"layer": 2, "cast_shadows": False})
    doc.node("Hidden", mesh=rock_mesh, translation=[-6, 0, 0], extras={"visible": False})
    doc.node("Loose_01", mesh=rock_mesh, translation=[-7, 0, -1], extras={"batch": False})
    doc.node("Loose_02", mesh=rock_mesh, translation=[-7.5, 0, 0.5], extras={"batch": False})
    if with_bad_node:
        doc.node("BadValues", extras={"all.i": "seven", "all.v3": [1, 2], "all.n": "NoSuchNode", "all.nosuch": 1,
                                      "spin": 5, "components": {"not_registered": {"x": 1}}})

    eye = [0, 9, 15]
    doc.node("MainCam", translation=eye, rotation=quat_look(eye, [0, 0.5, 0]), camera=0)
    top = [0, 20, 0.01]
    doc.node("TopCam", translation=top, rotation=quat_look(top, [0, 0, 0]), camera=1)
    sun_pos = [-6, 12, 8]
    doc.node("Sun", translation=sun_pos, rotation=quat_look(sun_pos, [0, 0, 0]),
             extensions={"KHR_lights_punctual": {"light": 0}}, extras={"cast_shadows": True})
    doc.node("Lamp", translation=[0, 2.5, -5.5], extensions={"KHR_lights_punctual": {"light": 1}})

    times = [0.0, 1.0, 2.0, 3.0, 4.0]
    keys = [quat_axis((0, 0, 1), -90 * i) for i in range(5)]
    doc.d["animations"] = [{"name": "Spin", "channels": [{"sampler": 0, "target": {"node": blades, "path": "rotation"}}],
                            "samplers": [{"input": doc.accessor(times, "SCALAR", 1),
                                          "output": doc.accessor(keys, "VEC4", 4), "interpolation": "LINEAR"}]}]
    doc.d["scenes"][0]["extras"] = {"autoplay": "Spin", "background": "#8fb8e0", "msaa": 4}
    return doc


def build_kit():
    doc = Doc()
    bark = doc.material("Bark", [0.35, 0.2, 0.1, 1])
    leaves = doc.material("Leaves", [0.2, 0.55, 0.15, 1])
    stone = doc.material("Stone", [0.55, 0.53, 0.5, 1], roughness=0.95)
    crate = doc.material("Crate", [0.8, 0.55, 0.25, 1])
    metal = doc.material("Metal", [0.2, 0.2, 0.22, 1], roughness=0.4)
    light = doc.material("Bulb", [1, 0.9, 0.6, 1], emissiveFactor=[1, 0.8, 0.4])
    tree = doc.mesh("Tree", [(cylinder(0.15, 0.1, 0.8), bark), (cylinder(0.6, 0.0, 1.6, 10, 0.6), leaves),
                             (cylinder(0.45, 0.0, 1.2, 10, 1.5), leaves)])
    rock = doc.mesh("Rock", [(box(0.9, 0.6, 0.8, 0.3), stone)])
    box_mesh = doc.mesh("Crate", [(box(0.8, 0.8, 0.8, 0.4), crate)])
    post = doc.mesh("Post", [(cylinder(0.06, 0.06, 2.0), metal)])
    bulb = doc.mesh("Bulb", [(box(0.3, 0.3, 0.3), light)])
    # parts laid out side by side, as an artist keeps a kit file; "kit.glb#Crate" places one
    doc.node("Tree", mesh=tree, translation=[-3, 0, 0])
    doc.node("Rock", mesh=rock, translation=[-1, 0, 0])
    doc.node("Crate", mesh=box_mesh, translation=[1, 0, 0])
    lamp = doc.node("Lamp_Post", mesh=post, translation=[3, 0, 0])
    doc.node("Bulb", parent=lamp, mesh=bulb, translation=[0, 2.1, 0])
    return doc


def main():
    with open(os.path.join(ROOT, "tests", "data", "level.gltf"), "w", newline="\n") as f:
        f.write(build_level(True).gltf_text())
    out = os.path.join(ROOT, "examples", "gltf_scene")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "level.glb"), "wb") as f:
        f.write(build_level(False).glb_bytes())
    with open(os.path.join(out, "kit.glb"), "wb") as f:
        f.write(build_kit().glb_bytes())


if __name__ == "__main__":
    main()

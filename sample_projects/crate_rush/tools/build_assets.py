#!/usr/bin/env python3
"""Builds Crate Rush's game files from the Kenney sources in art/ (fetch_assets.py puts them there).
Standard library only; deterministic (the same sources give byte-identical outputs).

    python sample_projects/crate_rush/tools/build_assets.py

Writes into data/assets/:
  ui/ui.json + ui.png              UI pack pieces packed into one atlas (TexturePacker JSON hash,
                                   nine-slice "borders" per frame)
  glyphs/glyphs.json + glyphs.png  input prompts, frames named the way br_input_glyph names them
                                   (key_space, xbox_a, ps_cross, switch_b, mouse_left...)
  models/kit/kit.glb               every platformer-kit piece as a root node named after the piece
                                   (sub-assets: "models/kit/kit.glb#crate")
  levels/level1.glb ...            the levels, composed the way a Blender export of them would look:
                                   kit meshes placed as nodes, node extras carrying components
                                   (spawn_point, crate, hazard, sign), "-colonly" collider empties,
                                   a camera, a sun, and level settings in the scene extras.

The level layouts are the LEVELS table below (one character per 2 x 2 tile).
"""
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ART = os.path.join(GAME, "art")
ASSETS = os.path.join(GAME, "data", "assets")


# ---- PNG (8-bit and palette/grey at 1-8 bits, every colour type, non-interlaced)

def read_png(path):
    """Returns (width, height, rgba bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat = 8, b""
    palette, trns = None, None
    width = height = depth = color = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if interlace != 0 or depth == 16:
                raise ValueError("%s: interlaced / 16-bit PNGs are not handled" % path)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    bits = channels * depth
    stride = (width * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = zlib.decompress(idat)
    rows = []
    prev = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                row[i] = (row[i] + a) & 255
            elif kind == 2:
                row[i] = (row[i] + b) & 255
            elif kind == 3:
                row[i] = (row[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(row)
        prev = row
    out = bytearray(width * height * 4)
    for y, row in enumerate(rows):
        for x in range(width):
            if depth < 8:
                per = 8 // depth
                v = (row[x // per] >> ((per - 1 - x % per) * depth)) & ((1 << depth) - 1)
                samples = [v]
            else:
                samples = row[x * channels:(x + 1) * channels]
            if color == 3:
                idx = samples[0]
                r, g, b = palette[idx * 3:idx * 3 + 3]
                a = trns[idx] if trns is not None and idx < len(trns) else 255
            elif color == 0:
                v = samples[0] * 255 // ((1 << depth) - 1)
                r = g = b = v
                a = 255
            elif color == 4:
                r = g = b = samples[0]
                a = samples[1]
            elif color == 2:
                r, g, b = samples
                a = 255
            else:
                r, g, b, a = samples
            o = (y * width + x) * 4
            out[o:o + 4] = bytes((r, g, b, a))
    return width, height, bytes(out)


def write_png(path, width, height, rgba):
    rows = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        f.write(chunk(b"IEND", b""))


# ---- atlases

def pack_atlas(images, out_png, out_json, borders=None, width=1024, padding=2):
    """images: [(frame name, png path)]. Shelf packing, tallest first. Writes a TexturePacker
    JSON hash; frames listed in `borders` get nine-slice borders [left, top, right, bottom]."""
    borders = borders or {}
    loaded = []
    for name, path in images:
        w, h, px = read_png(path)
        loaded.append((name, w, h, px))
    order = sorted(loaded, key=lambda item: (-item[2], item[0]))
    x = y = shelf = padding
    places = {}
    for name, w, h, _ in order:
        if x + w + padding > width:
            x = padding
            y += shelf + padding
            shelf = 0
        places[name] = (x, y)
        x += w + padding
        shelf = max(shelf, h)
    height = 1
    while height < y + shelf + padding:
        height *= 2
    atlas = bytearray(width * height * 4)
    frames = {}
    for name, w, h, px in loaded:
        ox, oy = places[name]
        for row in range(h):
            dst = ((oy + row) * width + ox) * 4
            atlas[dst:dst + w * 4] = px[row * w * 4:(row + 1) * w * 4]
        frame = {"frame": {"x": ox, "y": oy, "w": w, "h": h}, "rotated": False, "trimmed": False,
                 "spriteSourceSize": {"x": 0, "y": 0, "w": w, "h": h}, "sourceSize": {"w": w, "h": h}}
        if name in borders:
            left, top, right, bottom = borders[name]
            frame["borders"] = {"left": left, "top": top, "right": right, "bottom": bottom}
        frames[name] = frame
    write_png(out_png, width, height, bytes(atlas))
    doc = {"frames": dict(sorted(frames.items())),
           "meta": {"app": "crate_rush/tools/build_assets.py", "image": os.path.basename(out_png),
                    "size": {"w": width, "h": height}, "scale": "1", "format": "RGBA8888"}}
    with open(out_json, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print("%s: %d frames, %d x %d" % (os.path.relpath(out_png, GAME), len(frames), width, height))


# nine-slice borders of the UI pieces (pixels of the source PNG)
UI_BORDERS = {
    "yellow_button": [16, 14, 16, 20], "yellow_button_pressed": [16, 14, 16, 16],
    "grey_button": [16, 14, 16, 20], "grey_button_pressed": [16, 14, 16, 16],
    "green_button": [16, 14, 16, 20], "blue_button": [16, 14, 16, 20], "red_button": [16, 14, 16, 20],
    "grey_square": [16, 14, 16, 20], "blue_square": [16, 14, 16, 20],
    "panel": [20, 20, 20, 20], "panel_outline": [20, 20, 20, 20],
    "bar_fill": [8, 6, 8, 6], "bar_back": [8, 6, 8, 6], "divider": [4, 0, 4, 0],
}


def build_atlases():
    ui_dir = os.path.join(ART, "ui-pack")
    images = [(n[:-4], os.path.join(ui_dir, n)) for n in sorted(os.listdir(ui_dir)) if n.endswith(".png")]
    os.makedirs(os.path.join(ASSETS, "ui"), exist_ok=True)
    pack_atlas(images, os.path.join(ASSETS, "ui", "ui.png"), os.path.join(ASSETS, "ui", "ui.json"), UI_BORDERS)
    glyph_dir = os.path.join(ART, "input-prompts")
    images = [(n[:-4], os.path.join(glyph_dir, n)) for n in sorted(os.listdir(glyph_dir)) if n.endswith(".png")]
    os.makedirs(os.path.join(ASSETS, "glyphs"), exist_ok=True)
    pack_atlas(images, os.path.join(ASSETS, "glyphs", "glyphs.png"), os.path.join(ASSETS, "glyphs", "glyphs.json"))


# ---- glTF: read kit pieces, write composed .glb files

def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    json_len = struct.unpack("<I", data[12:16])[0]
    doc = json.loads(data[20:20 + json_len])
    pos = 20 + json_len
    bin_len = struct.unpack("<I", data[pos:pos + 4])[0]
    return doc, data[pos + 8:pos + 8 + bin_len]


def quat_yaw(degrees):
    import math
    a = math.radians(degrees) * 0.5
    return [0.0, round(math.sin(a), 6), 0.0, round(math.cos(a), 6)]


def quat_look(direction):
    """Rotation turning the node's -Z axis toward `direction` (cameras and lights look down -Z),
    keeping +Y up."""
    import math
    fx, fy, fz = direction
    n = math.sqrt(fx * fx + fy * fy + fz * fz)
    f = (fx / n, fy / n, fz / n)
    z = (-f[0], -f[1], -f[2])                                   # the node's +Z
    up = (0.0, 1.0, 0.0)
    x = (up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0])
    n = math.sqrt(sum(c * c for c in x))
    x = (x[0] / n, x[1] / n, x[2] / n)
    y = (z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0])
    m00, m01, m02 = x[0], y[0], z[0]
    m10, m11, m12 = x[1], y[1], z[1]
    m20, m21, m22 = x[2], y[2], z[2]
    trace = m00 + m11 + m22
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        q = [(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s]
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2
        q = [0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s]
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2
        q = [(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s]
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2
        q = [(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s]
    return [round(c, 6) for c in q]


class GltfBuilder:
    """Collects kit meshes (copied once each, sharing one embedded colormap texture) and nodes."""

    def __init__(self, kit_dir):
        self.kit_dir = kit_dir
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "crate_rush/tools/build_assets.py"},
                    "buffers": [], "bufferViews": [], "accessors": [], "meshes": [], "materials": [],
                    "textures": [], "images": [], "samplers": [{"magFilter": 9729, "minFilter": 9987}],
                    "nodes": []}
        self.pieces = {}       # piece name -> (source doc, [our mesh index per source mesh])
        self.colors = {}       # flat colour materials by hex
        with open(os.path.join(kit_dir, "Textures", "colormap.png"), "rb") as f:
            view = self.add_view(f.read())
        self.doc["images"].append({"name": "colormap", "mimeType": "image/png", "bufferView": view})
        self.doc["textures"].append({"sampler": 0, "source": 0})
        self.doc["materials"].append({"name": "colormap", "doubleSided": True,
                                      "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                                               "metallicFactor": 0.0, "roughnessFactor": 0.85}})

    def add_view(self, data, target=None, stride=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        if stride:
            view["byteStride"] = stride
        self.bin += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def piece(self, name):
        if name in self.pieces:
            return self.pieces[name]
        src, src_bin = read_glb(os.path.join(self.kit_dir, name + ".glb"))
        view_map, acc_map, meshes = {}, {}, []

        def accessor(i):
            if i in acc_map:
                return acc_map[i]
            a = dict(src["accessors"][i])
            v = a["bufferView"]
            if v not in view_map:
                sv = src["bufferViews"][v]
                off = sv.get("byteOffset", 0)
                view_map[v] = self.add_view(src_bin[off:off + sv["byteLength"]], sv.get("target"), sv.get("byteStride"))
            a["bufferView"] = view_map[v]
            self.doc["accessors"].append(a)
            acc_map[i] = len(self.doc["accessors"]) - 1
            return acc_map[i]

        for m in src["meshes"]:
            prims = []
            for p in m["primitives"]:
                prim = {"attributes": {k: accessor(v) for k, v in sorted(p["attributes"].items())},
                        "indices": accessor(p["indices"]), "material": 0}
                prims.append(prim)
            self.doc["meshes"].append({"name": m.get("name", name), "primitives": prims})
            meshes.append(len(self.doc["meshes"]) - 1)
        self.pieces[name] = (src, meshes)
        return self.pieces[name]

    def node(self, name, mesh=None, translation=None, rotation=None, scale=None, extras=None, children=None):
        n = {"name": name}
        if mesh is not None:
            n["mesh"] = mesh
        if translation is not None:
            n["translation"] = [round(c, 4) for c in translation]
        if rotation is not None:
            n["rotation"] = rotation
        if scale is not None:
            n["scale"] = [round(c, 4) for c in scale]
        if extras:
            n["extras"] = extras
        if children:
            n["children"] = children
        self.doc["nodes"].append(n)
        return len(self.doc["nodes"]) - 1

    def place(self, piece, name, translation, yaw=0.0, scale=None, extras=None):
        """A kit piece (with its child nodes) as a node of this file."""
        src, meshes = self.piece(piece)

        def copy(i, rename):
            sn = src["nodes"][i]
            kids = [copy(c, None) for c in sn.get("children", [])]
            return self.node(rename or sn.get("name", piece), meshes[sn["mesh"]] if "mesh" in sn else None,
                             sn.get("translation"), sn.get("rotation"), sn.get("scale"), None, kids)

        root_index = src["scenes"][src.get("scene", 0)]["nodes"][0]
        n = copy(root_index, name)
        node = self.doc["nodes"][n]
        node["translation"] = [round(c, 4) for c in translation]
        if yaw:
            node["rotation"] = quat_yaw(yaw)
        if scale is not None:
            node["scale"] = [scale, scale, scale] if isinstance(scale, (int, float)) else list(scale)
        if extras:
            node["extras"] = extras
        return n

    def quad(self, name, size, color, translation, extras=None):
        """A flat square (water) with its own colour material."""
        if color not in self.colors:
            r, g, b = (int(color[i:i + 2], 16) / 255.0 for i in (1, 3, 5))
            lin = [round(((c + 0.055) / 1.055) ** 2.4 if c > 0.04045 else c / 12.92, 4) for c in (r, g, b)]
            self.doc["materials"].append({"name": "flat_" + color[1:], "pbrMetallicRoughness": {
                "baseColorFactor": lin + [1.0], "metallicFactor": 0.0, "roughnessFactor": 0.8}})
            self.colors[color] = len(self.doc["materials"]) - 1
        h = size * 0.5
        pos = struct.pack("<12f", -h, 0, -h, -h, 0, h, h, 0, h, h, 0, -h)
        nrm = struct.pack("<12f", *([0.0, 1.0, 0.0] * 4))
        idx = struct.pack("<6H", 0, 1, 2, 0, 2, 3)
        acc = self.doc["accessors"]
        acc.append({"bufferView": self.add_view(pos, 34962), "componentType": 5126, "count": 4, "type": "VEC3",
                    "min": [-h, 0, -h], "max": [h, 0, h]})
        acc.append({"bufferView": self.add_view(nrm, 34962), "componentType": 5126, "count": 4, "type": "VEC3"})
        acc.append({"bufferView": self.add_view(idx, 34963), "componentType": 5123, "count": 6, "type": "SCALAR"})
        self.doc["meshes"].append({"name": name, "primitives": [{"attributes": {"POSITION": len(acc) - 3, "NORMAL": len(acc) - 2},
                                                                "indices": len(acc) - 1, "material": self.colors[color]}]})
        return self.node(name, len(self.doc["meshes"]) - 1, translation, extras=extras)

    def write(self, path, roots, scene_extras=None, extensions=None):
        doc = dict(self.doc)
        doc["scene"] = 0
        scene = {"name": os.path.basename(path)[:-4], "nodes": roots}
        if scene_extras:
            scene["extras"] = scene_extras
        doc["scenes"] = [scene]
        if extensions:
            doc.update(extensions)
        while len(self.bin) % 4:
            self.bin.append(0)
        doc["buffers"] = [{"byteLength": len(self.bin)}]
        text = json.dumps(doc, separators=(",", ":"), sort_keys=True).encode()
        while len(text) % 4:
            text += b" "
        total = 12 + 8 + len(text) + 8 + len(self.bin)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<I4s", len(text), b"JSON") + text)
            f.write(struct.pack("<I4s", len(self.bin), b"BIN\x00") + bytes(self.bin))
        print("%s: %d nodes, %d meshes, %d KB" % (os.path.relpath(path, GAME), len(doc["nodes"]), len(doc["meshes"]),
                                                  total // 1024))


def build_kit():
    """kit.glb: every piece once, side by side, each a root node named after the piece."""
    kit_dir = os.path.join(ART, "platformer-kit")
    b = GltfBuilder(kit_dir)
    names = sorted(n[:-4] for n in os.listdir(kit_dir) if n.endswith(".glb"))
    roots = [b.place(name, name, [i * 2.5, 0.0, 0.0]) for i, name in enumerate(names)]
    b.write(os.path.join(ASSETS, "models", "kit", "kit.glb"), roots)


# ---- levels
#
# One cell per 2 x 2 tile, two characters: ground, then object.
#   ground: '.' water  '#' ground block (top at y = 1)  'T' tall block (top at y = 2)  'p' wooden rafts
#   object: '.' none  'S' spawn  'c' crate  's' floor spikes  'x' spike block  'w' saw (moves along x)
#           'v' saw (moves along z)  't' tree  'f' flowers  'g' grass  'r' rocks  'm' mushrooms
#           'b' barrel  'n' start sign  'k' flag
TILE = 2.0
THEMES = {
    "grass": {"block": "block-grass-large", "tall": "block-grass-large-tall", "tree": "tree"},
    "snow": {"block": "block-snow-large", "tall": "block-snow-large-tall", "tree": "tree-pine-snow"},
}
LEVELS = [
    {"file": "level1", "theme": "grass", "water": "#2c7cc0",
     "scene": {"background": "#3f86c0", "sky": "#bfe3ff", "ground": "#6f8f5a", "ambient": 0.45, "msaa": 4,
               "level": {"title": "Meadow Isle", "time_limit": 60, "next": "level2",
                         "footsteps": "sounds/impact/footstep_grass", "kill_y": 0.0, "index": 1}},
     "sign": {"sign.text": "MEADOW ISLE", "sign.hint": "{jump} to jump onto the hill", "sign.radius": 2.6},
     "map": [
         ".. #t #. #f #. #t ..",
         "#r #c #. Tc T. #. #.",
         "#. #. #g #. #. #c #m",
         "#f #c #. .. #s #. #.",
         ".. #. #. #c #. #f ..",
         ".. #. #n #S #. #c ..",
     ]},
    {"file": "level2", "theme": "snow", "water": "#2a6aa8",
     "scene": {"background": "#5b86ad", "sky": "#dfeeff", "ground": "#7a8698", "ambient": 0.45, "msaa": 4,
               "level": {"title": "Frost Ridge", "time_limit": 75, "next": "level3",
                         "footsteps": "sounds/impact/footstep_snow", "kill_y": 0.0, "index": 2}},
     "sign": {"sign.text": "FROST RIDGE", "sign.hint": "Mind the spikes!", "sign.radius": 2.6},
     "map": [
         "#t #c #. .. T. Tc #t",
         "#. #s #x .. #. #s #.",
         "#c #. #. #. #. #. #c",
         ".. #. #s .. #c #x ..",
         "#c #. #. .. #. #. #t",
         "#t #. #n #S #. #c #.",
     ]},
    {"file": "level3", "theme": "grass", "water": "#2a78a8",
     "scene": {"background": "#c9814a", "sky": "#ffd9a8", "ground": "#6a5a48", "ambient": 0.45, "msaa": 4,
               "level": {"title": "Sawmill Bay", "time_limit": 80, "next": "",
                         "footsteps": "sounds/impact/footstep_wood", "kill_y": 0.0, "index": 3}},
     "sign": {"sign.text": "SAWMILL BAY", "sign.hint": "Saws! Time your run", "sign.radius": 2.6},
     "map": [
         "#t #c pw pc #c #b ..",
         "#. #. p. p. #. #. #t",
         "#c #w #. #c #. #w #c",
         ".. p. p. .. p. pc ..",
         "#b #. #. #v #. #. #c",
         "#t #c #n #S #. #b #t",
     ]},
]


def build_level(spec):
    kit_dir = os.path.join(ART, "platformer-kit")
    theme = THEMES[spec["theme"]]
    b = GltfBuilder(kit_dir)
    roots = []
    rows = [line.split() for line in spec["map"]]
    depth, width = len(rows), len(rows[0])
    x0 = -(width - 1) * TILE * 0.5
    z0 = -(depth - 1) * TILE * 0.5
    counts = {}
    spawn = None
    seed = [12345]

    def jitter(amount):
        seed[0] = (seed[0] * 1103515245 + 12345) & 0x7fffffff
        return ((seed[0] >> 8) / float(1 << 23) - 0.5) * 2.0 * amount

    def name(kind):
        counts[kind] = counts.get(kind, 0) + 1
        return "%s_%02d" % (kind, counts[kind])

    def top(ground):
        return {"#": 1.0, "T": 2.0, "p": 1.0}.get(ground, 0.0)

    # ground and its colliders: one box per run of equal ground in a row ("-colonly" empties)
    for r, row in enumerate(rows):
        c = 0
        while c < width:
            g = row[c][0]
            run = 1
            while c + run < width and rows[r][c + run][0] == g:
                run += 1
            if g != ".":
                cx = x0 + (c + (run - 1) * 0.5) * TILE
                cz = z0 + r * TILE
                if g == "p":
                    roots.append(b.node(name("Raft") + "-colonly", translation=[cx, 0.9, cz], scale=[run * TILE, 0.2, TILE]))
                else:
                    h = top(g)
                    roots.append(b.node(name("Ground") + "-colonly", translation=[cx, h * 0.5, cz],
                                        scale=[run * TILE + 0.08, h, TILE + 0.08]))
            c += run
        for c, cell in enumerate(row):
            g = cell[0]
            x, z = x0 + c * TILE, z0 + r * TILE
            # tiny height steps on a checkerboard so the overlapping block lips never z-fight
            lift = 0.004 if (r + c) % 2 else 0.0
            if g == "#":
                roots.append(b.place(theme["block"], name("Block"), [x, lift, z]))
            elif g == "T":
                roots.append(b.place(theme["tall"], name("Tall"), [x, lift, z]))
            elif g == "p":
                for dx in (-0.5, 0.5):
                    for dz in (-0.5, 0.5):
                        roots.append(b.place("platform-fortified", name("Raft"), [x + dx, 0.8 + lift, z + dz]))

    # objects
    for r, row in enumerate(rows):
        for c, cell in enumerate(row):
            g, o = cell[0], cell[1]
            x, z = x0 + c * TILE, z0 + r * TILE
            y = top(g)
            if o == "S":
                spawn = (x, y, z)
                roots.append(b.node("Spawn", translation=[x, y, z], extras={"spawn_point": {"yaw": 180.0}}))
            elif o == "c":
                roots.append(b.place("crate", name("Crate"), [x + jitter(0.3), y, z + jitter(0.3)], yaw=jitter(40.0),
                                     extras={"crate.value": 10}))
            elif o == "s":
                roots.append(b.place("trap-spikes", name("Spikes"), [x, y, z], scale=1.6,
                                     extras={"hazard.radius": 0.5}))
            elif o == "x":
                roots.append(b.place("spike-block", name("SpikeBlock") + "-col", [x, y, z],
                                     extras={"hazard.radius": 0.65}))
            elif o in "wv":
                axis = [1.6, 0.0, 0.0] if o == "w" else [0.0, 0.0, 1.6]
                roots.append(b.place("saw", name("Saw"), [x, y + 0.18, z], yaw=0.0 if o == "w" else 90.0, scale=1.3,
                                     extras={"hazard": {"radius": 0.5, "move": axis, "speed": 1.7, "spin": 9.0,
                                                        "phase": (r * 7 + c) * 0.37}}))
            elif o == "t":
                tree = theme["tree"]
                roots.append(b.place(tree, name("Tree"), [x + jitter(0.25), y, z + jitter(0.25)], yaw=jitter(180.0),
                                     scale=1.4))
                roots.append(b.node(name("TreeCol") + "-colonly", translation=[x, y + 1.0, z], scale=[0.8, 2.0, 0.8]))
            elif o == "b":
                roots.append(b.place("barrel", name("Barrel"), [x + 0.4, y, z - 0.4], scale=1.4))
                roots.append(b.node(name("BarrelCol") + "-colonly", translation=[x + 0.4, y + 0.35, z - 0.4],
                                    scale=[0.7, 0.7, 0.7]))
            elif o in "fgrm":
                piece = {"f": "flowers", "g": "grass", "r": "rocks", "m": "mushrooms"}[o]
                for k in range(3):
                    roots.append(b.place(piece, name("Decor"), [x + jitter(0.7), y, z + jitter(0.7)],
                                         yaw=jitter(180.0), scale=1.2))
            elif o == "n":
                roots.append(b.place("sign", name("Sign"), [x - 0.5, y, z + 0.3], yaw=20.0, scale=1.6,
                                     extras=dict(spec["sign"])))
            elif o == "k":
                roots.append(b.place("flag", name("Flag"), [x, y, z]))
    if spawn is None:
        raise ValueError(spec["file"] + ": no spawn")

    # water, sun, camera
    roots.append(b.quad("Water", 120.0, spec["water"], [0.0, 0.35, 0.0], extras={"cast_shadows": False}))
    lights = {"KHR_lights_punctual": {"lights": [{"type": "directional", "name": "Sun", "color": [1.0, 0.96, 0.9],
                                                  "intensity": 1.7}]}}
    sun = b.node("Sun", translation=[-6.0, 12.0, -8.0], rotation=quat_look([0.35, -1.0, 0.5]),
                 extras={"cast_shadows": True})
    b.doc["nodes"][sun]["extensions"] = {"KHR_lights_punctual": {"light": 0}}
    roots.append(sun)
    eye = [spawn[0], spawn[1] + 5.2, spawn[2] + 6.4]
    target = [spawn[0], spawn[1] + 0.6, spawn[2]]
    cam = b.node("Camera", translation=eye, rotation=quat_look([target[i] - eye[i] for i in range(3)]))
    b.doc["nodes"][cam]["camera"] = 0
    b.doc["cameras"] = [{"type": "perspective", "name": "Camera",
                         "perspective": {"yfov": 0.8727, "znear": 0.1, "zfar": 200.0}}]
    roots.append(cam)
    extensions = {"extensionsUsed": ["KHR_lights_punctual"], "extensions": lights}
    b.write(os.path.join(ASSETS, "levels", spec["file"] + ".glb"), roots, spec["scene"], extensions)


def main():
    build_atlases()
    build_kit()
    for spec in LEVELS:
        build_level(spec)
    return 0


if __name__ == "__main__":
    sys.exit(main())

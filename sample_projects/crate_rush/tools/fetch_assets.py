#!/usr/bin/env python3
"""Downloads the Kenney CC0 packs Crate Rush uses and copies the selected files into the game.
Standard library only. Run from anywhere:

    python sample_projects/crate_rush/tools/fetch_assets.py [--cache DIR]

Files go to two places:
  data/assets/...   used by the game as they are (character, fonts, sounds)
  art/...           sources that tools/build_assets.py turns into game files (kit pieces -> kit.glb
                    and the level .glb files, UI PNGs -> ui atlas, input prompts -> glyph atlas)
Every destination folder gets the pack's License.txt (all packs are CC0 1.0).

kenney.nl hands out a signed, expiring zip link per pack (the page carries a download id, and
https://kenney.nl/download-link/<id> returns the link), so the stable thing recorded is the pack
page. tools/assets.lock.json records, per pack, the page, the sha256 of the zip that was used and
of every copied file. A re-run compares against it and reports differences (Kenney re-zips packs
now and then); the lock is then rewritten.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(GAME))
LOCK = os.path.join(HERE, "assets.lock.json")
UA = "Mozilla/5.0 (crate_rush asset fetch script)"

KIT = "Models/GLB format/"
KIT_PIECES = [
    "block-grass-large", "block-grass-large-tall", "block-grass", "block-grass-low",
    "block-snow-large", "block-snow-large-tall", "block-snow", "block-snow-low",
    "platform", "platform-fortified", "crate", "crate-strong", "barrel",
    "tree", "tree-pine", "tree-pine-snow", "tree-snow", "flowers", "grass", "rocks", "stones",
    "mushrooms", "plant", "fence-straight", "sign", "flag", "trap-spikes", "spike-block", "saw",
    "coin-gold", "chest",
]

UI = "PNG/"
UI_FILES = [  # (pack path, file name in art/ui-pack)
    ("Yellow/Default/button_rectangle_depth_flat.png", "yellow_button.png"),
    ("Yellow/Default/button_rectangle_flat.png", "yellow_button_pressed.png"),
    ("Grey/Default/button_rectangle_depth_flat.png", "grey_button.png"),
    ("Grey/Default/button_rectangle_flat.png", "grey_button_pressed.png"),
    ("Green/Default/button_rectangle_depth_flat.png", "green_button.png"),
    ("Blue/Default/button_rectangle_depth_flat.png", "blue_button.png"),
    ("Red/Default/button_rectangle_depth_flat.png", "red_button.png"),
    ("Grey/Default/button_square_depth_flat.png", "grey_square.png"),
    ("Blue/Default/button_square_depth_flat.png", "blue_square.png"),
    ("Extra/Default/input_square.png", "panel.png"),
    ("Extra/Default/input_outline_square.png", "panel_outline.png"),
    ("Grey/Default/check_square_grey.png", "check_off.png"),
    ("Green/Default/check_square_color_checkmark.png", "check_on.png"),
    ("Yellow/Default/star.png", "star.png"),
    ("Grey/Default/star_outline_depth.png", "star_empty.png"),
    ("Yellow/Default/slide_horizontal_color.png", "bar_fill.png"),
    ("Grey/Default/slide_horizontal_grey.png", "bar_back.png"),
    ("Yellow/Default/arrow_basic_e.png", "arrow_e.png"),
    ("Yellow/Default/arrow_basic_w.png", "arrow_w.png"),
    ("Yellow/Default/icon_checkmark.png", "icon_check.png"),
    ("Yellow/Default/icon_cross.png", "icon_cross.png"),
    ("Extra/Default/divider.png", "divider.png"),
]

PROMPTS = {  # engine glyph frame (docs/agents/input.md naming) -> Kenney Input Prompts file
    "key_space": "Keyboard & Mouse/Default/keyboard_space.png",
    "key_return": "Keyboard & Mouse/Default/keyboard_enter.png",
    "key_escape": "Keyboard & Mouse/Default/keyboard_escape.png",
    "key_backspace": "Keyboard & Mouse/Default/keyboard_backspace.png",
    "key_tab": "Keyboard & Mouse/Default/keyboard_tab.png",
    "key_left_shift": "Keyboard & Mouse/Default/keyboard_shift.png",
    "key_left_ctrl": "Keyboard & Mouse/Default/keyboard_ctrl.png",
    "key_up": "Keyboard & Mouse/Default/keyboard_arrow_up.png",
    "key_down": "Keyboard & Mouse/Default/keyboard_arrow_down.png",
    "key_left": "Keyboard & Mouse/Default/keyboard_arrow_left.png",
    "key_right": "Keyboard & Mouse/Default/keyboard_arrow_right.png",
    "mouse_left": "Keyboard & Mouse/Default/mouse_left.png",
    "mouse_right": "Keyboard & Mouse/Default/mouse_right.png",
    "mouse_middle": "Keyboard & Mouse/Default/mouse_scroll.png",
    "xbox_a": "Xbox Series/Default/xbox_button_color_a.png",
    "xbox_b": "Xbox Series/Default/xbox_button_color_b.png",
    "xbox_x": "Xbox Series/Default/xbox_button_color_x.png",
    "xbox_y": "Xbox Series/Default/xbox_button_color_y.png",
    "xbox_start": "Xbox Series/Default/xbox_button_menu.png",
    "xbox_back": "Xbox Series/Default/xbox_button_view.png",
    "xbox_leftshoulder": "Xbox Series/Default/xbox_lb.png",
    "xbox_rightshoulder": "Xbox Series/Default/xbox_rb.png",
    "xbox_lefttrigger": "Xbox Series/Default/xbox_lt.png",
    "xbox_righttrigger": "Xbox Series/Default/xbox_rt.png",
    "xbox_leftstick": "Xbox Series/Default/xbox_stick_l.png",
    "xbox_rightstick": "Xbox Series/Default/xbox_stick_r.png",
    "xbox_dpup": "Xbox Series/Default/xbox_dpad_up.png",
    "xbox_dpdown": "Xbox Series/Default/xbox_dpad_down.png",
    "xbox_dpleft": "Xbox Series/Default/xbox_dpad_left.png",
    "xbox_dpright": "Xbox Series/Default/xbox_dpad_right.png",
    "ps_cross": "PlayStation Series/Default/playstation_button_color_cross.png",
    "ps_circle": "PlayStation Series/Default/playstation_button_color_circle.png",
    "ps_square": "PlayStation Series/Default/playstation_button_color_square.png",
    "ps_triangle": "PlayStation Series/Default/playstation_button_color_triangle.png",
    "ps_start": "PlayStation Series/Default/playstation5_button_options.png",
    "ps_back": "PlayStation Series/Default/playstation5_button_create.png",
    "ps_leftshoulder": "PlayStation Series/Default/playstation_trigger_l1.png",
    "ps_rightshoulder": "PlayStation Series/Default/playstation_trigger_r1.png",
    "ps_lefttrigger": "PlayStation Series/Default/playstation_trigger_l2.png",
    "ps_righttrigger": "PlayStation Series/Default/playstation_trigger_r2.png",
    "ps_leftstick": "PlayStation Series/Default/playstation_stick_l.png",
    "ps_rightstick": "PlayStation Series/Default/playstation_stick_r.png",
    "ps_dpup": "PlayStation Series/Default/playstation_dpad_up.png",
    "ps_dpdown": "PlayStation Series/Default/playstation_dpad_down.png",
    "ps_dpleft": "PlayStation Series/Default/playstation_dpad_left.png",
    "ps_dpright": "PlayStation Series/Default/playstation_dpad_right.png",
    "switch_a": "Nintendo Switch/Default/switch_button_a.png",
    "switch_b": "Nintendo Switch/Default/switch_button_b.png",
    "switch_x": "Nintendo Switch/Default/switch_button_x.png",
    "switch_y": "Nintendo Switch/Default/switch_button_y.png",
    "switch_start": "Nintendo Switch/Default/switch_button_plus.png",
    "switch_back": "Nintendo Switch/Default/switch_button_minus.png",
    "switch_leftshoulder": "Nintendo Switch/Default/switch_button_l.png",
    "switch_rightshoulder": "Nintendo Switch/Default/switch_button_r.png",
    "switch_leftstick": "Nintendo Switch/Default/switch_stick_l.png",
    "switch_dpup": "Nintendo Switch/Default/switch_dpad_up.png",
    "switch_dpdown": "Nintendo Switch/Default/switch_dpad_down.png",
}
for _c in "abcdefghijklmnopqrstuvwxyz0123456789":  # any letter or digit can be bound in the settings
    PROMPTS["key_" + _c] = "Keyboard & Mouse/Default/keyboard_%s.png" % _c

SOUNDS_IMPACT = (["footstep_%s_%03d.ogg" % (kind, i) for kind in ("grass", "snow", "wood") for i in range(5)] +
                 ["impactWood_light_002.ogg", "impactPunch_medium_000.ogg", "impactSoft_heavy_000.ogg",
                  "impactGeneric_light_000.ogg"])
SOUNDS_INTERFACE = ["confirmation_002.ogg", "error_006.ogg", "select_002.ogg", "click_002.ogg", "back_002.ogg",
                    "toggle_002.ogg", "tick_002.ogg", "pluck_002.ogg", "drop_003.ogg", "maximize_006.ogg"]
JINGLES = ["jingles_PIZZI01.ogg", "jingles_STEEL07.ogg", "jingles_PIZZI09.ogg"]

# slug -> list of (path inside the zip, destination relative to the game folder)
PACKS = {
    "mini-characters": [
        (KIT + "character-worker.glb", "data/assets/models/character/character-worker.glb"),
        (KIT + "Textures/colormap.png", "data/assets/models/character/Textures/colormap.png"),
    ],
    "platformer-kit": [(KIT + p + ".glb", "art/platformer-kit/%s.glb" % p) for p in KIT_PIECES] + [
        (KIT + "Textures/colormap.png", "art/platformer-kit/Textures/colormap.png"),
    ],
    "ui-pack": [(UI + src, "art/ui-pack/" + dst) for src, dst in UI_FILES] + [
        ("Font/Kenney Future.ttf", "data/assets/fonts/kenney_future.ttf"),
        ("Font/Kenney Future Narrow.ttf", "data/assets/fonts/kenney_future_narrow.ttf"),
        ("Sounds/click-a.ogg", "data/assets/sounds/ui/click-a.ogg"),
        ("Sounds/switch-a.ogg", "data/assets/sounds/ui/switch-a.ogg"),
        ("Sounds/tap-a.ogg", "data/assets/sounds/ui/tap-a.ogg"),
    ],
    "input-prompts": [(src, "art/input-prompts/%s.png" % frame) for frame, src in sorted(PROMPTS.items())],
    "impact-sounds": [("Audio/" + n, "data/assets/sounds/impact/" + n) for n in SOUNDS_IMPACT],
    "interface-sounds": [("Audio/" + n, "data/assets/sounds/interface/" + n) for n in SOUNDS_INTERFACE],
    "music-jingles": [("Audio/Pizzicato jingles/" + n if "PIZZI" in n else "Audio/Steel jingles/" + n,
                       "data/assets/sounds/jingles/" + n) for n in JINGLES],
}
# derived folders (written by build_assets.py) also carry the licence of the pack they come from
DERIVED_LICENSES = {
    "platformer-kit": ["data/assets/models/kit", "data/assets/levels"],
    "ui-pack": ["data/assets/ui"],
    "input-prompts": ["data/assets/glyphs"],
}


def http_get(url, referer=None):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    if referer:
        req.add_header("Referer", referer)
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def pack_page(slug):
    return "https://kenney.nl/assets/" + slug


def download(slug, cache):
    path = os.path.join(cache, slug + ".zip")
    if os.path.exists(path):
        return path
    page = pack_page(slug)
    html = http_get(page).decode("utf-8", "replace")
    marker = "data-download-id='"
    if marker not in html:
        raise RuntimeError("%s: no download id on the page (pack renamed?)" % page)
    pack_id = html.split(marker, 1)[1].split("'", 1)[0]
    link = json.loads(http_get("https://kenney.nl/download-link/" + pack_id, referer=page))["url"]
    data = http_get(link, referer=page)
    os.makedirs(cache, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print("downloaded %s (%d KB)" % (slug, len(data) // 1024))
    return path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write(rel, data):
    path = os.path.join(GAME, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.path.join(ROOT, "build", "kenney_cache"),
                    help="where the zips are kept (default build/kenney_cache)")
    args = ap.parse_args()
    old = {}
    if os.path.exists(LOCK):
        with open(LOCK) as f:
            old = json.load(f)
    lock = {}
    failed = []
    for slug, files in PACKS.items():
        try:
            zip_path = download(slug, args.cache)
        except Exception as err:  # keep going: report every pack that failed
            print("FAILED %s: %s" % (slug, err))
            failed.append(slug)
            continue
        with open(zip_path, "rb") as f:
            zip_bytes = f.read()
        entry = {"page": pack_page(slug), "licence": "CC0 1.0", "zip_sha256": sha256(zip_bytes), "files": {}}
        z = zipfile.ZipFile(zip_path)
        license_text = z.read("License.txt")
        dirs = set()
        for src, dst in files:
            data = z.read(src)
            write(dst, data)
            entry["files"][dst] = sha256(data)
            dirs.add(os.path.dirname(dst) if "/Textures/" not in dst else os.path.dirname(os.path.dirname(dst)))
        dirs.update(DERIVED_LICENSES.get(slug, []))
        for d in sorted(dirs):
            write(d + "/License.txt", license_text)
        prev = old.get(slug)
        if prev and prev.get("zip_sha256") != entry["zip_sha256"]:
            print("note: %s zip differs from the lock (re-zipped upstream?)" % slug)
        for dst, digest in entry["files"].items():
            if prev and prev["files"].get(dst) not in (None, digest):
                print("note: %s changed since the lock" % dst)
        lock[slug] = entry
        print("%s: %d files" % (slug, len(files)))
    if failed:
        print("failed packs (nothing written for them, lock unchanged): " + ", ".join(failed))
        return 1
    with open(LOCK, "w") as f:
        json.dump(lock, f, indent=1, sort_keys=True)
        f.write("\n")
    print("next: python sample_projects/crate_rush/tools/build_assets.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

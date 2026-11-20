"""test_playtest_tool - tools/playtest.py's short-command parser (no game needed): every command
parses, and trailing or unknown words are rejected before a game starts (API_REVIEW item 40)."""
import io
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import playtest  # noqa: E402


def rejected(text):
    try:
        playtest.parse_short(text)
    except SystemExit:
        return True
    return False


def main():
    p = playtest.parse_short
    assert p("step 60") == {"cmd": "step", "frames": 60}
    assert p("key Up") == {"cmd": "key", "key": "Up", "action": "tap"}
    assert p("key Up down") == {"cmd": "key", "key": "Up", "action": "down"}
    assert p("key Up tap 40") == {"cmd": "key", "key": "Up", "action": "tap", "frames": 40}
    assert p("action jump up 0.5") == {"cmd": "action", "name": "jump", "action": "up", "value": 0.5}
    assert p("gamepad connect xbox 1") == {"cmd": "gamepad", "action": "connect", "type": "xbox", "slot": 1}
    assert p("gamepad tap a 0 5") == {"cmd": "gamepad", "action": "tap", "button": "a", "slot": 0, "frames": 5}
    assert p("gamepad axis leftx -1") == {"cmd": "gamepad", "action": "axis", "axis": "leftx", "value": -1.0,
                                          "slot": 0}
    assert p("get */player:position") == {"cmd": "get", "path": "*/player:position"}
    assert p('set game:score 5') == {"cmd": "set", "path": "game:score", "value": 5}
    assert p('text "hello world"') == {"cmd": "text", "text": "hello world"}
    assert p("window 1280 720") == {"cmd": "window", "width": 1280, "height": 720}
    assert p("quit") == {"cmd": "quit"}
    assert p('{"cmd": "key", "key": "Up", "frames": 40}')["frames"] == 40
    for bad in ("key Up frames=40", "key Up down 3 extra", "key", "step 60 frames", "step ten",
                "action jump hold", "click a b", "get a b", "screenshot a.png b.png", "window 10",
                "window 1 2 3", "quit now", "stats 1", "profile 10 20", "gamepad press a",
                "gamepad connect xbox 1 2", "gamepad axis leftx", "set game:score", "frobnicate", ""):
        assert rejected(bad), bad

    # replies are UTF-8 whatever the console code page is (a German text came back as "Ã¼")
    seen = {}

    class FakePopen:
        def __init__(self, argv, **kwargs):
            seen.update(kwargs)
            self.stdout = io.StringIO('{"ok": true, "value": "J\\u00fcrgen"}\n')
            self.stdin = io.StringIO()

        def poll(self):
            return 0

        def wait(self):
            return 0

    real = playtest.subprocess.Popen
    playtest.subprocess.Popen = FakePopen
    try:
        game = playtest.Playtest("game.exe")
        assert seen.get("encoding") == "utf-8", seen
        assert game.send("get loc:x")["value"] == "Jürgen"
    finally:
        playtest.subprocess.Popen = real
    print("test_playtest_tool: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())

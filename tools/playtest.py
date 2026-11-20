#!/usr/bin/env python3
"""Drive a Braty game interactively (--playtest-stdin): step frames, press keys and actions,
click nodes by label, take screenshots, read and write state. Standard library only.

As a module:

    from playtest import Playtest
    with Playtest("build/windows-gcc/sample_projects/star_courier/star_courier.exe") as game:
        game.step(60)
        game.action("start")
        game.step(30)
        game.screenshot("build/shot.png")
        print(game.get("ship:position"))

From a shell (each command is one argument; replies are printed one JSON line each):

    python tools/playtest.py GAME.exe "step 60" "action start" "step 30" \
        "screenshot build/shot.png" "get ship:position" quit

Short commands: step [N], key NAME [down|up|tap], action NAME [tap|down|up [VALUE]],
gamepad connect TYPE [SLOT] | disconnect [SLOT] | tap|down|up BUTTON [SLOT] |
axis AXIS VALUE [SLOT], click LABEL,
text "...", screenshot FILE, get PATH, set PATH JSON, find LABEL, dump [FILE], window W H,
stats, profile [N], logs [N], help, quit. Anything starting with "{" is sent as raw JSON.
A failed command raises PlaytestError with the engine's message; the game process is ended when
the with-block exits, on an exception, and at interpreter exit.
Full reference: docs/PLAYTESTING.md.
"""
import argparse
import atexit
import json
import os
import shlex
import subprocess
import sys


class PlaytestError(Exception):
    """A command replied {"ok": false, ...} (the message is the engine's "error"), or the game
    went away; .reply holds the whole reply."""

    def __init__(self, reply):
        super().__init__(reply.get("error", "command failed"))
        self.reply = reply


class Playtest:
    """One game process in --playtest-stdin mode. Every method sends one command and returns
    the reply (a dict); failed commands raise PlaytestError unless check=False."""

    def __init__(self, exe, args=(), log=None, check=True):
        self.check = check
        self._log = open(log, "w") if log else subprocess.DEVNULL
        self.proc = subprocess.Popen(
            [os.path.abspath(exe), "--playtest-stdin", *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._log,  # the engine logs to stderr; stdout carries only replies
            text=True,
            bufsize=1,
        )
        atexit.register(self.kill)  # no orphan game, whatever happens to this script

    def send(self, cmd):
        """Sends one command (dict or JSON text) and returns its reply."""
        line = cmd if isinstance(cmd, str) else json.dumps(cmd)
        try:
            self.proc.stdin.write(line.replace("\n", " ") + "\n")  # one command = one line
            self.proc.stdin.flush()
        except OSError:
            raise PlaytestError({"ok": False, "error": "the game exited (exit code %s)" % self.proc.poll()}) from None
        while True:
            out = self.proc.stdout.readline()
            if out == "":
                raise PlaytestError({"ok": False, "error": "the game exited (exit code %s)" % self.proc.poll()})
            out = out.strip()
            if out.startswith("{"):
                break  # anything else on stdout is not ours (a game printing directly)
        try:
            reply = json.loads(out)
        except ValueError:
            raise PlaytestError({"ok": False, "error": "not a JSON reply: %r" % out}) from None
        if self.check and not reply.get("ok"):
            raise PlaytestError(reply)
        return reply

    def help(self):
        """The command list: [{"cmd", "args" ("name:type", optional "name?:type"), "doc"}]."""
        return self.send({"cmd": "help"})["commands"]

    # ---- input and time
    def step(self, frames=1):
        return self.send({"cmd": "step", "frames": frames})

    def key(self, name, action="tap", frames=None):
        cmd = {"cmd": "key", "key": name, "action": action}
        if frames is not None:
            cmd["frames"] = frames
        return self.send(cmd)

    def action(self, name, action="tap", value=None, frames=None):
        """Drives an input action: tap (default), down / up; value 0..1 for analogue actions."""
        cmd = {"cmd": "action", "name": name, "action": action}
        if value is not None:
            cmd["value"] = value
        if frames is not None:
            cmd["frames"] = frames
        return self.send(cmd)

    # ---- gamepads (slot = player 0..7)
    def gamepad_connect(self, type="generic", slot=None):
        """Attaches a virtual pad ("xbox", "playstation", "switch", "ps5"...); returns its slot."""
        cmd = {"cmd": "gamepad", "action": "connect", "type": type}
        if slot is not None:
            cmd["slot"] = slot
        return self.send(cmd)["slot"]

    def gamepad_disconnect(self, slot=0):
        return self.send({"cmd": "gamepad", "action": "disconnect", "slot": slot})

    def gamepad_button(self, button, action="tap", slot=0, frames=None):
        """SDL button names: a (south), b, x, y, start, back, dpup, leftshoulder...; tap, down or up."""
        cmd = {"cmd": "gamepad", "action": action, "button": button, "slot": slot}
        if frames is not None:
            cmd["frames"] = frames
        return self.send(cmd)

    def gamepad_axis(self, axis, value, slot=0):
        """leftx, lefty, rightx, righty (-1..1), lefttrigger, righttrigger (0..1); held until changed."""
        return self.send({"cmd": "gamepad", "action": "axis", "axis": axis, "value": value, "slot": slot})

    def click(self, label):
        return self.send({"cmd": "click", "label": label})

    def window(self, width, height):
        """Resizes the window (headless: the screen size cameras see)."""
        return self.send({"cmd": "window", "width": width, "height": height})

    def mouse(self, action, x=None, y=None, button=1):
        cmd = {"cmd": "mouse", "action": action, "button": button}
        if x is not None:
            cmd["x"], cmd["y"] = x, y
        return self.send(cmd)

    def text(self, text):
        return self.send({"cmd": "text", "text": text})

    # ---- looking at the game
    def screenshot(self, path):
        return self.send({"cmd": "screenshot", "file": path})

    def get(self, path):
        return self.send({"cmd": "get", "path": path})["value"]

    def set(self, path, value):
        return self.send({"cmd": "set", "path": path, "value": value})

    def find(self, label):
        return self.send({"cmd": "find", "label": label})

    def dump(self, path=None):
        cmd = {"cmd": "dump"}
        if path:
            cmd["file"] = path
        reply = self.send(cmd)
        return reply.get("dump", reply)

    def stats(self):
        return self.send({"cmd": "stats"})["stats"]

    def profile(self, frames=300):
        """Runs `frames` frames and returns the profiler summary of them (frame_ms_p95, cpu_ms_avg,
        gpu_ms_avg, fps, verdict, top_scopes...); afterwards "profile:<key>" paths read it."""
        return self.send({"cmd": "profile", "frames": frames})["profile"]

    def scene(self):
        """Name of the active scene ("" when none)."""
        return self.get("engine:scene")

    def logs(self, count=20):
        return self.send({"cmd": "logs", "count": count})["lines"]

    def expect(self, path, value, op="=="):
        return self.send({"cmd": "expect", "path": path, "op": op, "value": value})

    def wait_until(self, path, value, op="==", timeout_frames=600):
        return self.send({"cmd": "wait_until", "path": path, "op": op, "value": value,
                          "timeout_frames": timeout_frames})

    def quit(self, timeout=30):
        """Ends the game; returns its exit code (1 if any command failed). A game that does not
        exit within `timeout` seconds is killed."""
        if self.proc.poll() is None:
            try:
                self.send({"cmd": "quit"})
            except (PlaytestError, OSError):
                pass  # it already quit (a "quit" sent before), or it crashed: the exit code says which
        try:
            self.proc.stdin.close()  # a closed stdin also ends the game
        except OSError:
            pass
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            code = self.kill()
        if self._log is not subprocess.DEVNULL:
            self._log.close()
        return code

    def kill(self):
        """Stops the game process now (no-op if it already exited); returns its exit code."""
        if self.proc.poll() is None:
            self.proc.kill()
        return self.proc.wait()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, *exc):
        if exc_type is None:
            self.quit()
        else:
            self.kill()  # an exception: do not wait for a game that may be stuck


def parse_gamepad(rest):
    """gamepad connect TYPE [SLOT] | disconnect [SLOT] | tap|down|up BUTTON [SLOT] | axis AXIS VALUE [SLOT]"""
    action = rest[0]
    if action == "connect":
        cmd = {"cmd": "gamepad", "action": action, "type": rest[1] if len(rest) > 1 else "generic"}
        if len(rest) > 2:
            cmd["slot"] = int(rest[2])
        return cmd
    if action == "disconnect":
        return {"cmd": "gamepad", "action": action, "slot": int(rest[1]) if len(rest) > 1 else 0}
    if action == "axis":
        return {"cmd": "gamepad", "action": action, "axis": rest[1], "value": float(rest[2]),
                "slot": int(rest[3]) if len(rest) > 3 else 0}
    return {"cmd": "gamepad", "action": action, "button": rest[1], "slot": int(rest[2]) if len(rest) > 2 else 0}


def parse_short(text):
    """'step 60' -> {"cmd": "step", "frames": 60}; raw JSON passes through."""
    if text.lstrip().startswith("{"):
        return json.loads(text)
    try:
        return parse_words(text, shlex.split(text))
    except (IndexError, ValueError) as error:  # "key" without a name, "step ten"
        raise SystemExit("bad short command %r: %s" % (text, error))


def parse_words(text, words):
    """The short form split into words."""
    name, rest = words[0], words[1:]
    if name in ("step", "wait"):
        return {"cmd": name, "frames": int(rest[0]) if rest else 1}
    if name == "key":
        return {"cmd": "key", "key": rest[0], "action": rest[1] if len(rest) > 1 else "tap"}
    if name == "action":
        cmd = {"cmd": "action", "name": rest[0], "action": rest[1] if len(rest) > 1 else "tap"}
        if len(rest) > 2:
            cmd["value"] = float(rest[2])
        return cmd
    if name == "gamepad":
        return parse_gamepad(rest)
    if name in ("click", "find"):
        return {"cmd": name, "label": rest[0]}
    if name == "text":
        return {"cmd": "text", "text": " ".join(rest)}
    if name == "screenshot":
        return {"cmd": "screenshot", "file": rest[0]}
    if name == "get":
        return {"cmd": "get", "path": rest[0]}
    if name == "set":
        return {"cmd": "set", "path": rest[0], "value": json.loads(" ".join(rest[1:]))}
    if name == "dump":
        return {"cmd": "dump", "file": rest[0]} if rest else {"cmd": "dump"}
    if name == "logs":
        return {"cmd": "logs", "count": int(rest[0]) if rest else 20}
    if name == "profile":
        return {"cmd": "profile", "frames": int(rest[0]) if rest else 300}
    if name == "window":
        return {"cmd": "window", "width": int(rest[0]), "height": int(rest[1])}
    if name in ("stats", "quit", "help"):
        return {"cmd": name}
    raise SystemExit("unknown short command %r (use raw JSON for the others)" % text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("exe", help="the game executable")
    parser.add_argument("commands", nargs="*", help='short commands ("step 60") or raw JSON')
    parser.add_argument("--arg", action="append", default=[], help="extra game argument (repeatable)")
    parser.add_argument("--log", help="write the game's log (stderr) to this file")
    args = parser.parse_args()
    commands = [parse_short(text) for text in args.commands]  # bad short commands fail before the game starts
    with Playtest(args.exe, args.arg, log=args.log, check=False) as game:
        failed = False
        for cmd in commands:
            reply = game.send(cmd)
            print(json.dumps(reply))
            failed = failed or not reply.get("ok")
            if reply.get("ok") and cmd["cmd"] == "quit":
                break
        code = game.quit()
    return 1 if failed or code != 0 else 0


if __name__ == "__main__":
    sys.exit(main())

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

Short commands: step [N], key NAME [down|up|tap], action NAME, click LABEL,
text "...", screenshot FILE, get PATH, set PATH JSON, find LABEL, dump [FILE],
stats, logs [N], quit. Anything starting with "{" is sent as raw JSON.
Full reference: docs/PLAYTESTING.md.
"""
import argparse
import json
import os
import shlex
import subprocess
import sys


class PlaytestError(Exception):
    """A command replied {"ok": false, ...}; .reply holds the whole reply."""

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

    def send(self, cmd):
        """Sends one command (dict or JSON text) and returns its reply."""
        line = cmd if isinstance(cmd, str) else json.dumps(cmd)
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        while True:
            out = self.proc.stdout.readline()
            if out == "":
                raise PlaytestError({"ok": False, "error": "the game exited (exit code %s)" % self.proc.poll()})
            out = out.strip()
            if out.startswith("{"):
                break  # anything else on stdout is not ours (a game printing directly)
        reply = json.loads(out)
        if self.check and not reply.get("ok"):
            raise PlaytestError(reply)
        return reply

    # ---- input and time
    def step(self, frames=1):
        return self.send({"cmd": "step", "frames": frames})

    def key(self, name, action="tap", frames=None):
        cmd = {"cmd": "key", "key": name, "action": action}
        if frames is not None:
            cmd["frames"] = frames
        return self.send(cmd)

    def action(self, name):
        return self.send({"cmd": "action", "name": name})

    def click(self, label):
        return self.send({"cmd": "click", "label": label})

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

    def logs(self, count=20):
        return self.send({"cmd": "logs", "count": count})["lines"]

    def expect(self, path, value, op="=="):
        return self.send({"cmd": "expect", "path": path, "op": op, "value": value})

    def wait_until(self, path, value, op="==", timeout_frames=600):
        return self.send({"cmd": "wait_until", "path": path, "op": op, "value": value,
                          "timeout_frames": timeout_frames})

    def quit(self):
        """Ends the game; returns its exit code (1 if any command failed)."""
        if self.proc.poll() is None:
            try:
                self.send({"cmd": "quit"})
            except (PlaytestError, OSError):
                pass  # it already quit (a "quit" sent before), or it crashed: the exit code says which
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        code = self.proc.wait(timeout=30)
        if self._log is not subprocess.DEVNULL:
            self._log.close()
        return code

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.quit()


def parse_short(text):
    """'step 60' -> {"cmd": "step", "frames": 60}; raw JSON passes through."""
    if text.lstrip().startswith("{"):
        return json.loads(text)
    words = shlex.split(text)
    name, rest = words[0], words[1:]
    if name in ("step", "wait"):
        return {"cmd": name, "frames": int(rest[0]) if rest else 1}
    if name == "key":
        return {"cmd": "key", "key": rest[0], "action": rest[1] if len(rest) > 1 else "tap"}
    if name == "action":
        return {"cmd": "action", "name": rest[0]}
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
    if name in ("stats", "quit"):
        return {"cmd": name}
    raise SystemExit("unknown short command %r (use raw JSON for the others)" % text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("exe", help="the game executable")
    parser.add_argument("commands", nargs="*", help='short commands ("step 60") or raw JSON')
    parser.add_argument("--arg", action="append", default=[], help="extra game argument (repeatable)")
    parser.add_argument("--log", help="write the game's log (stderr) to this file")
    args = parser.parse_args()
    game = Playtest(args.exe, args.arg, log=args.log, check=False)
    failed = False
    for text in args.commands:
        reply = game.send(parse_short(text))
        print(json.dumps(reply))
        failed = failed or not reply.get("ok")
        if reply.get("ok") and parse_short(text)["cmd"] == "quit":
            break
    code = game.quit()
    return 1 if failed or code != 0 else 0


if __name__ == "__main__":
    sys.exit(main())

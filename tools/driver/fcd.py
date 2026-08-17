#!/usr/bin/env python3
"""
fcd - Flycast Driver client.

Talks to a driver-enabled Flycast over its loopback control port. Every
command is frame-synchronised inside the emulator, so results are
deterministic rather than dependent on window focus or wall-clock timing.

Usage:
    fcd.py status
    fcd.py exec "tap 1 start" "advance 60" "screenshot C:/tmp/shot.png"
    fcd.py shot out.png
    fcd.py settle --shot out.png
    fcd.py launch <flycast.exe> [game] [--port 6510]
    fcd.py eval "return flycast.state.gameId"

Commands accepted by `exec` (see README for the full list):
    ping | status | frames
    press <player> <buttons> | release <player> <buttons>
    tap <player> <buttons> [polls]
    axis <player> <axis> <value> | neutral [player]
    advance <frames> | waitfor <timeoutFrames> <luaExpr>
    eval <luaChunk>
    read <8|16|32> <addr> | write <8|16|32> <addr> <value>
    savestate [slot] | loadstate [slot]
    screenshot <path>

Buttons are names, combinable with '+': a b c d x y z start up down left right.
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import time

DEFAULT_PORT = int(os.environ.get("FLYCAST_DRIVER_PORT") or 6510)
SCRATCH = os.environ.get("FCD_SHOT_DIR") or os.path.join(
    os.path.expanduser("~"), "AppData", "Local", "Temp", "fcd")


class DriverError(RuntimeError):
    pass


class Driver:
    """A single connection to a running Flycast instance."""

    def __init__(self, port=DEFAULT_PORT, host="127.0.0.1", timeout=30.0):
        self.port = port
        self.host = host
        self.timeout = timeout
        self.sock = None
        self._buf = b""

    # ---------------------------------------------------------- connection

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        return self

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # ------------------------------------------------------------ protocol

    def send(self, command):
        """Send one command, return its result. Raises DriverError on failure."""
        if self.sock is None:
            self.connect()
        # the wire protocol is one request per line, so escape any newlines
        line = command.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "")
        self.sock.sendall((line + "\n").encode("utf-8"))

        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise DriverError("connection closed by Flycast")
            self._buf += chunk
        raw, self._buf = self._buf.split(b"\n", 1)

        try:
            reply = json.loads(raw.decode("utf-8"))
        except ValueError:
            raise DriverError("bad reply: %r" % raw[:200])
        if not reply.get("ok"):
            raise DriverError(reply.get("error", "unknown error"))
        return reply.get("result")

    # ------------------------------------------------------- conveniences

    def status(self):
        return self.send("status")

    def tap(self, buttons, player=1, polls=2):
        return self.send("tap %d %s %d" % (player, buttons, polls))

    def press(self, buttons, player=1):
        return self.send("press %d %s" % (player, buttons))

    def release(self, buttons, player=1):
        return self.send("release %d %s" % (player, buttons))

    def advance(self, frames=1):
        return self.send("advance %d" % frames)

    def eval(self, chunk):
        return self.send("eval " + chunk)

    def waitfor(self, expr, timeout_frames=600):
        return self.send("waitfor %d %s" % (timeout_frames, expr))

    def screenshot(self, path):
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.send("screenshot " + path.replace("\\", "/"))
        return path

    def settle(self, max_frames=900, chunk=20, stable_samples=3):
        """Advance until the rendered frame stops changing.

        Useful for waiting out boot sequences and load screens without
        round-tripping a screenshot to the caller each time. Returns the number
        of frames advanced. Animated screens never converge, so this always
        returns by max_frames.
        """
        tmp = os.path.join(SCRATCH, "_settle.png")
        os.makedirs(SCRATCH, exist_ok=True)
        last, same, advanced = None, 0, 0
        while advanced < max_frames:
            self.advance(chunk)
            advanced += chunk
            try:
                self.screenshot(tmp)
                with open(tmp, "rb") as f:
                    digest = hashlib.sha1(f.read()).hexdigest()
            except (DriverError, OSError):
                continue
            same = same + 1 if digest == last else 0
            last = digest
            if same >= stable_samples:
                break
        return advanced


# -------------------------------------------------------------------- launch

def wait_for_port(port, timeout=60.0, host="127.0.0.1"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return True
        except OSError:
            time.sleep(0.25)
    return False


def launch(exe, game=None, port=DEFAULT_PORT, extra_args=None, wait=True):
    """Start Flycast with the driver listening on `port`.

    The child is detached so it outlives this process. Without that, a Flycast
    started from inside an automation harness dies when the harness tears down
    the shell's process tree, which looks exactly like a crash but leaves no
    trace in the log.
    """
    env = dict(os.environ)
    env["FLYCAST_DRIVER_PORT"] = str(port)
    cmd = [exe]
    if game:
        cmd.append(game)
    if extra_args:
        cmd.extend(extra_args)
    cwd = os.path.dirname(os.path.abspath(exe))

    kwargs = {"env": env, "cwd": cwd, "close_fds": True}
    if os.name == "nt":
        detach = (subprocess.CREATE_NEW_PROCESS_GROUP
                  | subprocess.DETACHED_PROCESS
                  | subprocess.CREATE_BREAKAWAY_FROM_JOB)
        try:
            proc = subprocess.Popen(cmd, creationflags=detach, **kwargs)
        except OSError:
            # the job object may forbid breakaway; fall back to plain detach
            proc = subprocess.Popen(
                cmd,
                creationflags=(subprocess.CREATE_NEW_PROCESS_GROUP
                               | subprocess.DETACHED_PROCESS),
                **kwargs)
    else:
        proc = subprocess.Popen(cmd, start_new_session=True, **kwargs)

    if wait and not wait_for_port(port):
        raise DriverError("Flycast did not open port %d within 60s" % port)
    return proc


# ----------------------------------------------------------------------- CLI

def main():
    # --port is accepted both before and after the subcommand; putting it only
    # on the top-level parser is a needless trap
    # SUPPRESS matters: with a real default, the subparser would overwrite a
    # --port given before the subcommand with its own default
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", type=int, default=argparse.SUPPRESS)

    ap = argparse.ArgumentParser(
        parents=[common],
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="action", required=True)

    sub.add_parser("status", parents=[common])

    p = sub.add_parser("exec", parents=[common],
                       help="run one or more driver commands in order")
    p.add_argument("commands", nargs="+")

    p = sub.add_parser("eval", parents=[common], help="evaluate a Lua chunk")
    p.add_argument("chunk")

    p = sub.add_parser("shot", parents=[common], help="capture a screenshot")
    p.add_argument("path", nargs="?", default=os.path.join(SCRATCH, "shot.png"))

    p = sub.add_parser("settle", parents=[common],
                       help="advance until the screen stops changing")
    p.add_argument("--max-frames", type=int, default=900)
    p.add_argument("--shot", help="capture to this path once settled")

    p = sub.add_parser("launch", parents=[common],
                       help="start Flycast with the driver enabled")
    p.add_argument("exe")
    p.add_argument("game", nargs="?")
    p.add_argument("--no-wait", action="store_true")

    args = ap.parse_args()
    args.port = getattr(args, "port", DEFAULT_PORT)

    if args.action == "launch":
        proc = launch(args.exe, args.game, args.port, wait=not args.no_wait)
        # print the pid so callers can later stop exactly this instance --
        # never kill flycast by image name, other instances may be running
        print("pid=%d port=%d" % (proc.pid, args.port))
        return 0

    try:
        with Driver(port=args.port) as d:
            if args.action == "status":
                print(json.dumps(d.status(), indent=2))
            elif args.action == "eval":
                print(json.dumps(d.eval(args.chunk)))
            elif args.action == "shot":
                print(d.screenshot(args.path))
            elif args.action == "settle":
                frames = d.settle(max_frames=args.max_frames)
                print("settled after %d frames" % frames)
                if args.shot:
                    print(d.screenshot(args.shot))
            elif args.action == "exec":
                for cmd in args.commands:
                    result = d.send(cmd)
                    print("%s -> %s" % (cmd, json.dumps(result)))
    except DriverError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    except OSError as e:
        print("error: cannot reach Flycast on port %d (%s)" % (args.port, e),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
End-to-end check against a running driver-enabled Flycast.

    python selftest.py [--port 6510] [--shot-dir DIR]

Exercises every command and verifies the emulator actually responds to input
(rather than just accepting the request). Exits non-zero on the first failure.
"""
import argparse
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fcd import Driver, DriverError  # noqa: E402

results = []


def check(name, fn):
    try:
        value = fn()
        results.append((True, name, value))
        print("  PASS  %-28s %s" % (name, value))
        return value
    except Exception as e:
        results.append((False, name, str(e)))
        print("  FAIL  %-28s %s" % (name, e))
        return None


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()[:12]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6510)
    ap.add_argument("--shot-dir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "shots"))
    args = ap.parse_args()
    os.makedirs(args.shot_dir, exist_ok=True)

    print("connecting to 127.0.0.1:%d" % args.port)
    with Driver(port=args.port) as d:
        print("\n-- basics --")
        check("ping", lambda: d.send("ping"))
        status = check("status", lambda: d.status())
        check("frames", lambda: d.send("frames"))

        if not status or not status.get("running"):
            print("\nNo game running - start one, then re-run. "
                  "Input and memory tests need the emulation thread.")
            return 1

        print("\n-- frame timing --")
        before = d.send("frames")
        check("advance 30", lambda: d.advance(30))
        after = d.send("frames")
        check("vblank advanced >= 30",
              lambda: (after["vblank"] - before["vblank"] >= 30)
              or _fail("only advanced %d" % (after["vblank"] - before["vblank"])))
        check("maple polled during advance",
              lambda: (after["maplePoll"] - before["maplePoll"] > 0)
              or _fail("no maple polls - is the game reading the controller?"))

        print("\n-- input --")
        check("neutral", lambda: d.send("neutral"))
        check("press a", lambda: d.press("a"))
        check("release a", lambda: d.release("a"))
        check("tap start", lambda: d.tap("start"))
        check("tap combo", lambda: d.tap("a+start"))
        check("axis joyx", lambda: d.send("axis 1 joyx 20000"))
        check("axis reset", lambda: d.send("axis 1 joyx 0"))
        check("bad button rejected", lambda: _expect_error(d, "press 1 nope"))
        check("bad player rejected", lambda: _expect_error(d, "press 9 a"))

        print("\n-- lua --")
        check("eval arithmetic", lambda: d.eval("return 6*7"))
        check("eval gameId", lambda: d.eval("return flycast.state.gameId"))
        check("eval kcode", lambda: d.eval("return flycast.input.getButtons(1)"))
        check("waitfor true", lambda: d.waitfor("true", 60))
        check("waitfor false times out", lambda: d.waitfor("false", 5))
        check("lua error reported", lambda: _expect_error(d, "eval return ((("))

        print("\n-- memory --")
        check("read32 ram", lambda: d.send("read 32 0x8c010000"))
        check("read8 ram", lambda: d.send("read 8 0x8c010000"))

        print("\n-- screenshots --")
        a = os.path.join(args.shot_dir, "selftest_a.png")
        check("screenshot", lambda: d.screenshot(a))
        check("png non-empty", lambda: os.path.getsize(a) > 1000
              or _fail("file is %d bytes" % os.path.getsize(a)))
        check("png is a PNG", lambda: open(a, "rb").read(8).startswith(b"\x89PNG")
              or _fail("bad magic"))

        b = os.path.join(args.shot_dir, "selftest_b.png")
        d.advance(120)
        check("screenshot again", lambda: d.screenshot(b))
        check("frame changed over 120f",
              lambda: (digest(a) != digest(b)) or "identical (static screen?)")

    failed = [r for r in results if not r[0]]
    print("\n%d/%d passed" % (len(results) - len(failed), len(results)))
    if failed:
        print("failures: " + ", ".join(r[1] for r in failed))
    return 1 if failed else 0


def _fail(msg):
    raise AssertionError(msg)


def _expect_error(d, command):
    try:
        result = d.send(command)
    except DriverError as e:
        return "rejected: %s" % str(e)[:60]
    raise AssertionError("should have failed, returned %r" % result)


if __name__ == "__main__":
    sys.exit(main())

# Driving Flycast as an agent

Operating manual for an automated agent using the driver. `README.md` next to
this file explains the design and lists the full protocol; this file is about
using it well.

## Rules that matter before anything else

**Never kill Flycast by process name.** No `Get-Process flycast |
Stop-Process`, no `taskkill /IM flycast.exe`, no `pkill flycast`. Several
instances are routinely running — different builds, netplay pairs, the user's
own work. Capture the pid at launch (`fcd.py launch` prints `pid=NNNNN`) and
stop only that pid. If you did not launch it, do not touch it.

**Give every instance its own working directory.** Two instances sharing a
directory share `emu.cfg`, `data/` and the log, and one of them will die.
Copy `flycast.exe`, `emu.cfg` and `data/` into `run1/`, `run2/`, … and launch
each from its own. This is verified behaviour, not a precaution.

## Quickstart

```
python tools/driver/fcd.py launch <path-to>/flycast.exe <game.gdi> --port 6510
    -> pid=21092 port=6510
python tools/driver/fcd.py status --port 6510
python tools/driver/fcd.py shot shots/now.png --port 6510
```

Then `Read` the PNG to see the screen. That is the whole loop: act, capture,
look.

`launch` waits until the port answers, so the next command is safe to send
immediately. `--port` works before or after the subcommand.

`launch` deliberately detaches the child. If you start Flycast yourself with a
plain `subprocess.Popen` from inside an automation harness, it will be killed
when the harness tears down the shell's process tree — which looks exactly like
a crash but leaves nothing in `flycast.log`. Use `launch`, or detach yourself
(`CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS` on Windows, `start_new_session`
elsewhere).

## Spend as few turns as possible

Every tool call costs a turn, and waiting dominates emulator automation. Three
habits do most of the work:

**Batch a whole sequence into one `exec`.** Commands run in order on one
connection:

```
python tools/driver/fcd.py exec "tap 1 start" "advance 60" \
    "screenshot H:/tmp/menu.png" --port 6510
```

**Push waits into the emulator instead of polling from outside.** If you know
an address to watch, one round trip replaces a poll loop:

```
waitfor 1800 flycast.memory.read32(0x8c1a2f04) == 2
```

**When you don't know an address, use `settle`** — it advances until the
rendered frame stops changing:

```
python tools/driver/fcd.py settle --shot shots/loaded.png --port 6510
```

`settle` never converges on an animated screen and returns at `--max-frames`
(default 900). That is a bound, not a failure — check the screenshot.

## Do not trust your eyes on screenshots

This is the single easiest way to draw a wrong conclusion. Emulated UIs use low
contrast, and a selected menu item may differ from its neighbours only by a
shade. Measure instead:

```
python tools/driver/inspect_img.py diff before.png after.png
python tools/driver/inspect_img.py bands screen.png 10 --channel blue --y0 115 --y1 405
```

`diff` reports how many pixels changed and the bounding box, so you can tell a
real state change from a scrolling background. `bands` splits a region into N
rows and ranks them — for a menu whose selected entry renders white and the
rest yellow, the blue channel identifies the selection unambiguously.

Worked example from Sega Rally 2: three `tap 1 down` presses looked like they
had moved the highlight four positions, which read as doubled input. `bands`
showed each tap moved exactly one *selectable* position — the disabled
`2PLAYER BATTLE` row is skipped by the game and sits at a constant intensity in
every frame. The driver was correct; the eyeball reading was not.

Corollary: before concluding an input was dropped, check whether the control is
simply inert. On the SELECT YEAR screen `right` does nothing because years 2-10
are locked, while `a` advances normally.

## Reading errors

Errors are honest about which layer failed:

| Message | Meaning |
| --- | --- |
| `no game running (command needs the emulation thread)` | Nothing is loaded, or the emulator is paused / in the GUI menu. `ping`, `status` and `screenshot` still work. |
| `emulation thread stalled for Nms` | Vblanks stopped mid-command — usually the GUI menu opened. |
| `no frame available (is a game running?)` | `screenshot` with no rendered frame yet. |
| `unknown button 'x'` / `player must be 1..4` | Bad argument; nothing was sent to the guest. |
| `cannot reach Flycast on port N` | The instance is gone, or you have the wrong port. |

A command never hangs indefinitely: it fails fast with no game, and aborts if
the emulation thread stops pumping for 5s.

## Input model, in one paragraph

`tap` holds a button for N **maple polls** (default 4) and blocks until the
release, so when it returns the guest has provably sampled the press. Do not
substitute `press` + `advance` + `release` and count frames: the guest polls
the controller at a rate the game chooses, roughly once per four frames while
loading on SR2, so a frame-counted hold can be missed entirely. If a game seems
to need a longer press, raise the poll count (`tap 1 a 10`) rather than adding
`advance`s.

`press`/`release` are for genuinely held inputs (accelerator, a charge move).
Always `neutral` afterwards — held buttons persist until released.

## Recipes

Boot to a known screen and confirm:

```
fcd.py launch run/flycast.exe game.gdi --port 6510
fcd.py settle --shot shots/title.png --port 6510
# Read shots/title.png
```

Navigate a menu one step at a time, verifying each move:

```
fcd.py exec "tap 1 down" "advance 20" "screenshot shots/a.png" \
             "tap 1 down" "advance 20" "screenshot shots/b.png" --port 6510
fcd.py ... ; inspect_img.py diff shots/a.png shots/b.png
```

Save a known state so later runs skip the boot entirely:

```
fcd.py exec "savestate 1" --port 6510
fcd.py exec "loadstate 1" --port 6510      # in a later session
```

Read guest memory while playing:

```
fcd.py exec "read 32 0x8c010000" --port 6510
fcd.py eval "return flycast.memory.read8(0x8c210044)" --port 6510
```

Two instances for netplay, each in its own directory:

```
fcd.py launch run1/flycast.exe game.gdi --port 6510   -> pid=A
fcd.py launch run2/flycast.exe game.gdi --port 6511   -> pid=B
```

## Known limits

- The maple hook is in `core/hw/maple/maple_if.cpp`. Naomi and SystemSP have
  their own `ggpo::getInput` call sites in `core/hw/naomi/systemsp.cpp` that
  are not hooked, so poll-timed holds are Dreamcast-only for now.
- `eval` and `waitfor` need a Lua-enabled build; they return a clear error
  otherwise.
- The control port is unauthenticated and binds loopback only. Do not forward
  it.
- `screenshot` captures the rendered frame, so it needs a running game and a
  live renderer.

# Flycast Driver

A control surface for driving Flycast programmatically: frame-accurate input,
memory access, screenshots and Lua evaluation over a loopback TCP port.

Intended for automated testing and scripted playthroughs — anything that needs
a reliable see → decide → act loop against a real emulated Dreamcast without
depending on window focus or wall-clock timing.

## Why not just synthesise input events?

| Approach | Why it fails |
| --- | --- |
| `SendInput` / `keybd_event` | Needs window focus; timing is unrelated to emulated frames, so presses land in the wrong frame or get dropped. |
| Virtual gamepad driver | Needs a kernel driver install, and still gives no screenshot, no memory access, no frame determinism. |
| The Lua API on its own | Has input and memory, but no transport (base Lua has no sockets) and no framebuffer capture. |

This feature keeps the existing Lua API and adds the two things it lacks: a
synchronous transport and screenshot capture. `eval` forwards arbitrary chunks
to the usual `flycast.*` namespace, so memory, savestates and maple device
config come along unchanged.

## The reliability problem it solves

`kcode[]` is persistent global state modified by SDL events on the **UI
thread**. The guest only observes buttons when it polls the **maple bus**, on
the **emulation thread**, at a rate the *game* chooses — not once per vblank.

Measured on Sega Rally 2:

| State | vblanks | maple polls | ratio |
| --- | --- | --- | --- |
| Booting / loading | 601 | 144 | ~1 poll per 4 frames |
| In menus | 61 | 62 | ~1:1 |

During boot the game reads the controller a quarter as often as it renders, so
holding a button for "N frames" is a race. Button holds are therefore timed in
**maple polls**, and `tap` blocks until the release — when it returns, the
guest has provably sampled the press.

## Building

Lua is required for `eval` and `waitfor`. If no system Lua is found, the build
falls back to the copy vendored under
`core/deps/luabridge/Tests/Lua/lua-5.4.4`, so no extra dependency is needed.

The control server is compiled in via `USE_DRIVER` (on by default).

## Running

The server binds `127.0.0.1` only, and is disabled when the port is 0.

```
set FLYCAST_DRIVER_PORT=6510
flycast.exe game.gdi
```

Separate ports let several instances be driven at once, e.g. for netplay tests.

```
python tools/driver/fcd.py status
python tools/driver/fcd.py exec "tap 1 start" "advance 60" "screenshot menu.png"
python tools/driver/fcd.py settle --shot menu.png
python tools/driver/selftest.py
```

## Protocol

One request per line, one JSON response per line:
`{"ok":true,"result":...}` or `{"ok":false,"error":"..."}`.
Newlines inside an argument are escaped as `\n`.

| Command | Notes |
| --- | --- |
| `ping`, `status`, `frames` | Answered without the emulation thread, so they work with no game loaded. |
| `press <p> <buttons>` | Sets buttons and returns; stays held. |
| `release <p> <buttons>` | |
| `tap <p> <buttons> [polls]` | Holds for N maple polls (default 4), then releases. Blocks until released. |
| `axis <p> <axis> <value>` | `joyx joyy joyrx joyry joy3x joy3y` (−32768..32767), `lt rt lt2 rt2` (0..65535). |
| `neutral [p]` | Release everything. |
| `advance <frames>` | Blocks for N vblanks. |
| `waitfor <timeoutFrames> <luaExpr>` | Evaluates the expression each vblank until true. Returns `{matched:bool}`. |
| `eval <luaChunk>` | Returns the first result. |
| `read <8\|16\|32> <addr>` / `write ...` | Guest memory. |
| `savestate [slot]` / `loadstate [slot]` | |
| `screenshot <path>` | PNG, captured on the UI thread. |

Players are 1–4. Buttons are names combinable with `+`:
`a b c d x y z start up down left right up2 down2 left2 right2 reload card`,
or a raw numeric mask.

### Waiting efficiently

Waiting dominates emulator automation. Rather than polling screenshots from the
host, push the condition into the emulator:

```
waitfor 1800 flycast.memory.read32(0x8c1a2f04) == 2
```

`fcd.py settle` is the screenshot-based equivalent when no address is known: it
advances until the rendered frame stops changing.

## Implementation notes

- Jobs execute from the vblank event on the emulation thread. Nothing runs from
  inside a maple DMA transfer, which would be re-entrant; `onMaplePoll()` only
  counts polls and ages holds.
- `screenshot` runs on the UI thread via `gui_runOnUiThread` and is synchronous.
- A client is never left hanging: commands needing the emulation thread fail
  immediately when no game is running, and abort if the emulation thread stops
  pumping for 5s (paused, or in the menu).

## Caveats

- The maple hook lives in `maple_if.cpp`. Naomi/SystemSP have their own
  `ggpo::getInput` call sites in `hw/naomi/systemsp.cpp`, which are not hooked,
  so poll-timed holds are Dreamcast-only for now.
- `settle` never converges on an animated screen; it returns at `max_frames`.
- The control port is unauthenticated. It is loopback-only by design.

## Verified

`selftest.py`: 28/28 against Sega Rally 2 (HDR0010). Driven end to end with no
window focus: title → Start → MODE SELECT → navigate → 10YEAR CHAMPIONSHIP →
SELECT YEAR → A → SELECT CAR → carousel rotates. Menu navigation was measured
with `inspect_img.py bands` rather than eyeballed: every `tap` moved the
highlight exactly one selectable position.

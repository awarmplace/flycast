# This repo, and what every branch in it is for

**This is the primary modified Flycast.** Dreamcast online-revival work — Sega Rally 2,
Power Stone 2, and whatever follows — builds from here and nowhere else.

The directory is still called `flycast-redwango-fix`, which is now a misleading name: the
redwango fix it was created for has been superseded by upstream (see `dwango-serial-bridge`
below). Treat the name as historical.

**The one exception:** `H:/proj/rez` keeps its own heavily instrumented Flycast for
recompilation research. That fork carries a recomp runtime and an autoplay bot and is not
kept in step with upstream. It is a different tool for a different job — do not merge the
two, and do not build online work there.

Retired rule: an earlier standing instruction said *never build in `flycast-redwango-fix`*.
That was correct while it was a scratch tree and `sr2-bba/flycast` was the build. It is
deliberately reversed as of 2026-08-19 — this repo is now the build, and `sr2-bba/flycast`
is the one to leave alone.

---

## Upstream (github.com/flyinghead/flycast)

| branch | what it is |
|---|---|
| `upstream/dev` | **where flyinghead actually develops, and what we track.** ~83 commits ahead of master. New work lands here first - including `rawmodem`, the raw modem byte pipe added for Sega Rally 2 (commit `2550d109`). |
| `upstream/master` | the release branch. Lags `dev` considerably. Tracking it means waiting months for work that already exists. |
| everything else | `crackindj-s4`, `dreamlink`, `fh/rec-doublefp`, `l10n_dev`, `playstore`, `wip/sh-block-graphs` - flyinghead's feature and experiment branches. Not ours; ignore unless chasing something specific. |

**Track `dev`, not `master`.** That is not the usual convention and it is easy to get wrong.

## Ours (github.com/awarmplace/flycast)

| branch | ahead | status | what is on it |
|---|---|---|---|
| `flycast-driver` | 3 | **LIVE - the base we build from** | The TCP control server that makes the emulator scriptable: frame-accurate input timed in maple polls, guest memory read/write, Lua eval, screenshots, savestates. Plus detached launch with `--port`, and the agent operating guide in `tools/driver/`. This is the instrument that has settled more questions than every other technique combined. |
| `dev-instrumentation` | 2 | to be folded in | The SH-4 write watch and execution trap, ported from the research fork, on top of the serial bridge. The tracing half is still wanted; the serial-bridge half underneath it is not (see below). |
| `dwango-serial-bridge` | 1 | **SUPERSEDED - do not merge** | The original `MODEMBRIDGE` serial bridge: a raw byte pipe for games that speak neither PPP nor IP. Flyinghead has now implemented the same idea upstream as `RawModemService` in `rawmodem.cpp`, with the identical `writeModem` / `readModem` / `modemAvailable` interface. Carrying this branch would mean maintaining a parallel implementation of a feature upstream now ships. Keep the branch as history; do not build from it. |
| `sr2-redwango-build` | 5 | historical | The Sega Rally 2 REDWANGO test build, a libretro link fix for `ModemBridge`, and a framebuffer-emulation default. Point-in-time build branch, kept for reference. |
| `master` | 0 | mirror | Plain upstream master. Useful only for comparison. |

---

## What is being assembled, and why

`flycast-driver` rebased onto `upstream/dev`, plus two things carried over:

1. **The SH-4 tracing** - read watches, write watches, execution traps, and the per-PC
   summary sidecar that survives a hard kill. Currently living in `sr2-bba/flycast`'s
   `core/debug/sh4_trace.cpp`, which has grown well beyond what `dev-instrumentation` holds.
   This is what found the Power Stone 2 button bug.
2. **Game-ID selection for the raw modem.** Upstream activates `RawModemService` when the
   game ID matches Sega Rally 2 JP. Power Stone 2 needs adding alongside it, and we need a
   local endpoint rather than flyinghead's hosted `dcnet.flyca.st:7657`, because our server
   is on localhost during development and on our own host in the field.

Explicitly NOT carried over:

- **`bbabridge`** - the dumb ethernet frame pipe. That is Broadband Adapter work and belongs
  to Sega Rally 2. Power Stone 2 is modem-only end to end: it dials an ISP, negotiates PPP,
  then places a second modem call for the match link. `BBABRIDGE` appears nowhere in the
  Power Stone 2 runbook or findings. If the BBA work resumes it can be ported then.
- **`serialbridge`** - replaced by upstream's `rawmodem`.

## Practical notes

- The driver is **committed here**, not applied as an overlay. There is no `apply.py` step
  after a rebase, which removes a whole class of "the build silently lacks the driver"
  mistakes.
- Build with DirectX 9 **off** (`-DUSE_DX9=OFF`), always.
- `SH4TRACE_*` and `SH4TRAP` only work on the interpreter, so they need `Dynarec.Enabled = no`.
- After any rebase, check that the driver's hooks still sit where they expect - upstream is
  active in the SH4 and modem code, and `sh4_trace.cpp` is the most likely thing to conflict.

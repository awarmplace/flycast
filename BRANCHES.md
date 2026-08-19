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

Prefixed `awp/` so it is obvious at a glance which branches are ours and which are
flyinghead's. All of them sit on `upstream/dev`.

| branch | ahead | status | what is on it |
|---|---|---|---|
| `awp/main` | 6 | **the branch we build from** | Everything below, integrated, plus this document. This is the one to check out. |
| `awp/driver` | 3 | live, single-purpose | The TCP control server that makes the emulator scriptable: input timed in maple polls, guest memory read/write, Lua eval, screenshots, savestates; detached launch with `--port`; the agent guide in `tools/driver/`. |
| `awp/sh4-trace` | 1 | live, single-purpose | The SH-4 watches and traps - execution trap, polled value watch, range read and write watches, and the sidecar summary that survives a hard kill. Self-contained, ~300 lines, no recompiler or census scaffolding. |
| `awp/rawmodem-local` | 1 | live, **the PR candidate** | `MODEMBRIDGE=host:port` to point a console at a server on this machine, and Power Stone 2's `T1218M` alongside Sega Rally's. Small, general, and inert when the variable is unset - the most plausible thing to offer upstream. |
| `master`, `dev` | 0 | mirrors | Plain upstream. Never commit to these; they exist so we can diff and rebase against a known-clean reference. |
| `archive/*` | - | history, do not build | `dwango-serial-bridge` (superseded by upstream's rawmodem), `dev-instrumentation` (its tracing is now `awp/sh4-trace`, on a cleaner base), `sr2-redwango-build` (a point-in-time build). Kept because the serial bridge is what flyinghead built the Sega Rally support from. |

**Why the single-purpose branches exist at all:** each is a potential contribution. Keeping
them unmixed means offering one upstream is a cherry-pick rather than an archaeology
exercise. `awp/main` is where they live together for our own builds.

## What was assembled, and why

Done on 2026-08-19: `awp/main` is `upstream/dev` plus the driver, and two things carried over:

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

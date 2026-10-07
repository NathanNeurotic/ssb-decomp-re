# PR #21 rollback and performance investigation

Checkpoint: 2026-10-07. The immediate hardware report is that PR #21's
`fd7b7473088ff7af900ed0f695453ee99563141a` completes platform boot from UDPFS
but stays black before the N64 logo. The user requested a substantial rollback
of the accumulated fixes and commits to the existing PR for continuity.

## Current candidate

- PR: <https://github.com/NathanNeurotic/ssb-decomp-re/pull/21>
- Head branch: `feat/ps2-igr-riptopl-mass`; base: `main` at
  `749f66598371d56b048696dc14156434b4f86ceb`.
- Rollback commit: `19082666db47297b9723cfd92506e91fb18a188b`.
- The rollback commit's complete tree equals
  `377230090e3bd73b5fcf66c079a5847eb50caa66`: the six original IGR/deadzone
  commits before the mass experiments. Later checkpoints add only this handoff
  and a host regression probe, without changing that runtime baseline.
- Only five runtime files differ from main: the input/platform declarations,
  pad handling, IGR implementation, and GS quiesce hook. Storage, saves, audio,
  game startup, thread/message code and scheduling match main exactly.
- PR #20 was closed as superseded. History is retained; no force push was used.

**Next required hardware check:** use the rollback artifact on the same UDPFS
setup and confirm N64 logo, gameplay, then IGR. Do not restore the discarded
mass/startup stack as a group. The earlier USB/mass boot problem is unresolved.
The original libmc save and IGR paths are restored too; this is a controlled
baseline, not a claim that every inherited path is correct.

## Confirmed source regression removed by the rollback

Commit `3931c2f2643f5339944c42411a7eac7cd7e404af` moved thread bookkeeping
after `StartThread`, `ResumeThread` and `SignalSema` in
`ps2/src/ultra/thread.c`. These operations can immediately preempt the caller.

A higher-priority target can run, call `osStopThread(NULL)`, set its state to
STOPPED and park in `WaitSema` before the waking call returns. The changed code
then overwrites that state with RUNNABLE. A subsequent `osStartThread` skips
the STOPPED branch and never signals the parked coroutine. This supplies a
concrete game-start freeze mechanism independent of UDPFS or mass recovery.

The restored code publishes state before the kernel wake, so a target's later
state change is preserved. The host test extracts and compiles the actual
`osStartThread` and `osStopThread` functions from Git revisions. Windows fibers
simulate the relevant immediate-preemption sequence.

```powershell
python ps2/tests/check_thread_activation.py
# Optional additional known-good revision:
python ps2/tests/check_thread_activation.py fd7b74730 HEAD '3931c2f26^'
```

Requirements: Windows, Python 3, Git, Windows GCC on PATH. Test output belongs
under ignored `ps2/build/thread-activation-check/`. Expected result: all three
activation cases fail on the discarded head and pass on HEAD and the parent
of the introducing commit. This is a host regression result, not proof that
this race is the console's only failure.

## Validation evidence

- Full local native `ps2build` build: 661 steps completed, ELF linked/stripped.
  Readelf confirms ELF32, little-endian, executable, R5900 flags. Existing
  baseline compiler warnings remain; this was not a warning-free build.
- Host test: first start, resume suspended, and signal stopped each fail on
  `fd7b74730`; each passes on `377230090` and `3931c2f26^`.
- Rollback CI: [Build PS2 run #323](https://github.com/NathanNeurotic/ssb-decomp-re/actions/runs/37675162841)
  succeeded for `19082666db47297b9723cfd92506e91fb18a188b`.
- PR diff against main passes `git diff --check`. Restoring exact baseline
  text reintroduces five existing whitespace lines relative to the discarded
  head; those lines are unchanged relative to main.
- Real-console validation of this rollback is pending. No local PCSX2 run
  was performed. Check CI on the actual latest PR head for later checkpoints.

## Original slowdown report versus Cooper

The user clarified that the comparison is with johnson-cooper's PS2 port,
not the N64 original. Verified Cooper main:
`3b47e02583b07c6fdc415d2d16e4aa577e4af33b`. The published fork nightly is
`749f66598371d56b048696dc14156434b4f86ceb`. The screenshot alone does not
identify the tested ELF, scene, mode or measured frame rate.

The strongest released-code performance suspect is the clamp/mirror texture
fix: `bind_texture` in `ps2/src/renderer/gbi.c` now requests the full legal
clamped tile, and `make_resident` in `texcache.c` expands, converts and uploads
it. Larger power-of-two allocations and distinct clamp extents can increase
VRAM pressure. Eviction triggers another CPU conversion and upload, and
wrapping the 1 MiB staging ring calls `ps2_pkt_finish`. This is a plausible
steady gameplay cost **only if the scene repeatedly misses/evicts textures**;
it is not a measured regression. Reverting it would restore visual defects.

Secondary candidates are four floating-point divisions per texture rectangle
in the signed-STQ sprite fix, and changed blocking audio-sample upload traffic
(8,128-byte chunks versus 32 KiB upstream). Regular audio register batches
are asynchronous now, so the audio rewrite is not automatically slower.
Sample uploads would more naturally explain intermittent first-use stalls.

The released fork's timing, VI, renderer worker, game scheduler/task manager
and libultra thread code are unchanged from Cooper. GS mode setup is also
unchanged; a new PAL/30 Hz switch is not supported by this source comparison.
Cooper's own status file recorded about 31 FPS and 17-18 ms graphics time in
one PCSX2 Hyrule match. Those are historical upstream observations, not current
measurements or a hardware baseline.

Once UDPFS reaches gameplay again, compare the same scene/fighters/storage,
DAT and video mode. Briefly sample both builds with Select + R3: FPS, graphics
time, texture conversions/upload bytes, and the asset-read counter. Use the
same overlay state in each comparison; the overlay itself adds rendering work.
Persistent texture uploads/conversions after warm-up would support the first
suspect. Do not optimize or revert visual fixes solely on the anecdotal report.

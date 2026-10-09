# SSB64 PS2 ELF runtime audit — 2026-10-09

Scope: **compiled PS2 ELF, runtime modules and actual in-game behavior**. ROM/DAT generation tools, browser packer, converted asset contents and website are excluded. The reader may assume the existing DAT is valid. No release/merge decision can be inferred from passing host tests or a linked ELF alone.

## Baselines and method

- Current integration: draft PR [#24](https://github.com/NathanNeurotic/ssb-decomp-re/pull/24) into main, carrying the later graphics/performance restoration plus narrow fixes. The PR is intentionally the **only forward-moving consolidation PR**.
- Known real-console baseline: [PS2_BASELINE_CHECKPOINT.md](PS2_BASELINE_CHECKPOINT.md), candidate 3a6d842. The Cooper v1.0 GS/VI/presentation rollback eliminated severe gameplay lag; subsequent tested Mario/Pikachu texture fixes were retained. Do **not** pull entire old recovery PR #21 or #23 over newer graphics.
- Inspection passes: module startup/device policy, file-stream lifetime, libultra threading/message/VI, GS/GBI/texture residency, native audio/RPC, PAD/rumble, SRAM, IGR, EE memory/crash diagnostics, IOP mini-services, build/link & CI. Then repeated review of fixes for new failure paths.
- Regression instrumentation now uses host C programs **extracted from actual source functions**, compiling under gcc with controlled fake IOP/filesystem timeouts. Results test logic, not live PS2 hardware.

## Verified faults and repairs applied on PR #24

| Subsystem | Observed source defect | Correction | Validation boundary |
|---|---|---|---|
| Inherited IOP/fileXio | Loading duplicate fileXio over the launcher's live mounted mass/BDM stack can fail before EE RPC binding; SDK's fileXioInit itself retries indefinitely when service missing | Probe live RPC with bounded SIF bind, reuse inherited server, load fileXio only when absent, fail visibly if no RPC | Compile + inherited mock; actual launcher compatibility still console-only |
| IGR | Old branch probed BOOT.ELF over blocking mcSync during exit; GS could black before a hung handoff | Exit to OSDSYS without card probes, quiesce controller, best-effort bounded SPU key-off/mute | Console return/audio-latch test still needed |
| SRAM dirty tracking | Original cleared dirty before card write; early recovery PR used VBlank timestamp, losing second writes in same VBlank | Dirty cleared only after successful card write/close and unchanged monotonic write generation | Host same-VBlank generation; card pull/IGR test outstanding |
| SRAM RPC | Blocking mcSync could hang startup; after bounded timeout, issuing another card RPC would queue behind a potentially stuck operation | Bound mcSync; stop card I/O after timeout including after mkdir/read/write; retain dirty SRAM | Device reconnect without ELF restart not supported after timeout, intentionally safe |
| SRAM bounds | Offset+length arithmetic could overflow; read that crossed SRAM boundary left output tail unchanged | Bounds via remaining length and zero-fill read tail | Host range test |
| Inherited mass selection | Explicit massN: path searched other volumes with matching filename | Literal slot uses only named volume, while generic mass: can search | Host exact-slot and generic alias test |
| UDPFS file close | Core ignored transport failure, malformed reply type and server error, and the IOP wrapper unconditionally reported success | Validate response length/type/result and propagate close status while releasing local slot | Source-extracted UDPFS mock protocol test; real network recovery outstanding |
| UDPFS disconnect race | Core sets receive app-header pointer to stack-local reply before calling recv; low-level recv returns early on already-disconnected socket without clearing pointers | Clear buffer and app-header registrations on recv error before returning | Source-extracted disconnect/send-failure regression; real unplug/network recovery outstanding |
| Audio RPC | Async per-frame batch completion wait was unbounded; on timeout buffer ownership uncertain | Bounded drain; no buffer reuse while RPC could still own DMA data | Host permanently-busy RPC test, actual sound quality unchanged |
| SPU sample tracking | Failed upload could still be marked resident; out-of-range voice sample ID accepted | Do not register after failure; validate sample IDs, sample count, and initial async batch status | ELF build, audio playback on console needed |
| Audio table | Equal-power lookup indices derived from audio commands were unchecked; FXAMT_ALT could use index 128 for a 128-element table | Clamp outside table; values **inside** [0,127] remain byte-identical | Host table boundaries |
| GS texcache | Oversized staging allocation could overflow its 1MiB ring; failed entry could remain live; VRAM interval allocator lacked a terminal capacity check | Check buffer/VRAM bounds; abandon oversized conversions; release failed entries | Host stage test; verify scenes/menus/texture fidelity on console |
| GBI | Encoded VTX count with end smaller than count produced negative destination and out-of-bounds vertex writes | Reject invalid base/count before touching vertex array; ordinary valid path unchanged | Native build; pathological-display-list harness desirable |
| Renderer startup | Job semaphore/thread creation failures were ignored | Check and panic at failure | Host/CI build |
| Runtime queue/event | Zero/negative queue capacity could cause bad modulo; negative OS event index was unchecked | Validate queue and unsigned event range | CI build |
| Input | Controller disconnections retained stale buttons/sticks/raw held combos | Clear stale state on failed pad read/disconnect; diagnose negative padInit result | Reconnect with multitaps/DS2 on hardware |
| Boot essentials | Required PAD/MTAP/SIO2 module-load errors were silently ignored | Panic visibly on mandatory services; sound/memory-card services degrade with log | Launcher compatibility unproven |
| Fatal reporting | Forced printf could talk to stale inherited IOP stdout after it was deliberately disabled | Respect sConsole on panic | Native build |
| Boot service setup | Asset stream semaphore, VBlank semaphore/interrupt handler, native PAD init and IOP audio thread startup lacked return checks | Check and fail visibly; free audio thread handle on StartThread failure | CI and hardware startup |

CI pipeline runs `test_runtime_safety.py`, `test_inherited_transports.py`, `test_renderer_coordinates.py`, builds the PS2 ELF, checks ELF header, and publishes its artifact. Native tests are **necessary, not sufficient** for performance/timing correctness.

## Remaining risks — not silently "fixed"

### P0: Can still cause a hard freeze or loss of game access

1. **SIF/IOP reset handoff.** `SifIopReset` and `SifIopSync` spin until success. USB/BDM inherited paths are specially preserved because resetting a mounted launcher stack loses transport registration. Unknown launchers may leave incompatible SIF state. Do not add global reset retries or duplicate stack loads without a hardware isolation case.
2. **PAD/MULTITAP RPC bind.** Current PS2SDK `padInit` and `mtapInit` implementations may loop waiting for a registered server, even before they can return an error. Checking a return code catches explicit failures, not an unresponsive server. Proper containment requires a trusted equivalent client/probe or a service-level timeout contract and testing with launchers.
3. **GS FINISH/GIF DMA.** `dmaKit_wait` and GS FINISH polling are unbounded. Replacing them with a timeout inside the working renderer risks misdiagnosing slow DMA or corrupting frame presentation. Instrument with a watchdog/last-stage capture before making a functional timing change.
4. **Synchronous audio RPC.** Per-frame async batches are bounded now, but sample-upload and initialization calls are synchronous `sceSifCallRpc` operations. A wedged IOP audio server can still block. A future nonblocking request/reply state machine would be a major architecture change; do not insert an arbitrary timeout around a *blocking* call that cannot return.
5. **Mid-game DAT transport loss.** Streaming still uses blocking newlib `read/lseek` and a persistent descriptor. The reopen loop recovers completed short reads/errors but cannot rescue an IOP syscall that never returns. Hardware fault injection is required per backend.
6. **MMCE card-side stream promotion.** The card-side descriptor is extracted via ioctl2, MMCEMAN stack reset, then MMCEDRV + stream bridge install with a second IOP reset; handoff assumes card-side descriptor survival, correct MMCEDRV exports by index, SIO2 hook compatibility, and eventual completion. Existing historical tests differ across commits; current consolidated path is not hardware validated. Do not duplicate MMCEMAN/MMCEDRV hooks.

### P1: Compatibility/correctness with performance consequences

7. **Generic `mass:` discovery** scans many slot/path combinations repeatedly using POSIX open, even when devices are unmounted. Explicit massN is now isolated; generic aliases still need safe low-cost root mount probe validated against USB, MX4SIO, ATA, iLink and inherited launcher stacks. Don't replace transport mid-stream.
8. **Mismatched launch/data origin.** `argv[0]` and `--data` can name different transports; preserving IOP is based on the *data* location. Ensure next test matrix includes memory-card ELF + USB DAT and network launch + other data device. A launcher reset may invalidate the original ELF filesystem but that is fine only after ELF is fully loaded.
9. **Memory-card persistence.** After timeout the session intentionally stops IOP card calls and keeps dirty SRAM. Powering off before restarting loses recent progress; surface a clear save-disabled state in future UI. Double-slot format and CRC are preserved; no save format changes in this PR.
10. **Texture cache source fingerprint** hashes first 32 texture bytes and first 16 palette bytes. A mutable texture or palette changed outside those leading bytes could remain incorrectly cached. Expanding the hash every bind could reintroduce the severe prior frame-rate regression. Establish low-cost dirty/content generation tracking before changing it.
11. **GBI unsupported features** include some RDP combiner/blender semantics, triangle/rectangle edge rules, invalid DL pointers/recursion and timing-sensitive framebuffer swaps. Mario fireballs and intro light blending remain specifically unconfirmed. Preserve Cooper GS/VI baseline until a screenshot + minimal repro can establish whether a pixel-level correction is safe.
12. **RDP vertex/draw scheduling** runs under an asynchronous renderer but posts both SP and DP events together after completion. This is not cycle-accurate RCP overlap. Do not change SP/DP messaging without comparing scheduler queue state and hardware frame times.
13. **Audio IOP batch RPC performance**: callback completion and DMA ownership now safer, but repeated sample residency uploads and synchronous RPCs can still contribute to frame spikes. Profile calls per second, upload sizes, blocked time, and audible underruns on console, not Windows host microbenchmarks.
14. **Timing pseudo clock** maps EE bus counter to libultra clock; `ps2_delay_vblanks` is a delay approximation rather than a true VSYNC semaphore. A speculative clock rewrite risks reintroducing lag/half-speed scheduler regressions. Collect frame-rate and task-latency traces before optimizing.

### P2: Diagnostics, footprint, cleanup

15. **Static framebuffer and staging reservations** include two 768KiB GIF packet buffers and a 1MiB texture staging ring; these allocations are intentional to decouple EE packing and DMA. Changing pool sizes or forcing more `ps2_pkt_finish` calls may increase stalls.
16. **IOP module provenance** is mixed: PS2SDK embedded IRXs, vendored MMCE and dedicated audio server. Module name tests can differ among launcher versions. Dump discovered module names and versions on a failing console before broad “reuse everything” policies.
17. **Crash path** disables interrupts and attempts to suspend many thread IDs, then re-enables interrupts to write a log to the failing backend. Save/log attempts in a storage-origin crash may hang; the on-screen crash record remains primary.
18. **Build warnings**: the decompiled original game has numerous legacy C pointer/integer warnings in the PS2 target build. These are not automatically harmless, but blanket -Werror or sweeping cast replacement can break intentional N64 linker symbol arithmetic. Triage only diagnostics in running PS2-specific changes, then use targeted type tests.
19. **Unsupported output conditions**: native 240p may not be accepted by some capture devices; this is separate from actual performance defects.
20. **Save-location semantics and pack location**: scripts and docs sometimes describe “next to ELF” while `--data` is permitted; runtime logs should always show the resolved data directory and backend, not assume it is ELF's directory.

## Hardware validation matrix (do not reduce to repeated blind builds)

- **Provenance**: record PS2 model, launcher name and version, exact ELF SHA, DAT path, launch path, and IOP logs. Check the *same* branch build across devices.
- **USB / BDM:** launch through inherited `mass0:`, explicit `mass1:` with a competing DAT on mass0, `usb:` alias, and memory-card ELF plus mass DAT. At least one cold/warm boot and immediate controller response.
- **MX4SIO / MMCE / UDPFS / UDPBD / APA / iLink:** smoke boot when hardware is available; ensure no duplicate IOP reset, hooks or transport replacement. MMCE especially requires sustained read + IGR.
- **Gameplay/perf:** splash, title, character-select, battle, item/particle scenes, transition to next battle; compare gameplay frame cadence, renderer microseconds/GBI, SPU RPC blocked time, texture evictions and audio interruptions against the console-good checkpoint.
- **Visual fidelity:** Mario cap, Pikachu face, fireballs, translucent intro light, sprite edges, masked/clamped textures, framebuffer tears/white flashes; preserve screenshots.
- **IGR:** combo hold >=60 VBlanks, OSDSYS return, muted audio after exit, no controller lock, no storage access after exit begins.
- **Saves:** load both alternating SRAM slots; two writes in one VBlank; remove card during flush; invalid CRC/partial write; full card; restart without overwriting last good slot. Confirm no bogus success reporting.
- **Recovery:** unplug/replug pad, delay USB mount, missing DAT, failed audio driver, stalled memory-card RPC. Fatal errors must not print through stale stdout or spin forever during diagnostics.

## Explicit non-goals and exclusions

- No DAT/browser generator, manifest, sample conversion, packaging, website, or filename changes.
- No wholesale replacement of renderer/presentation with unproven optimizations, no silent frame-rate tradeoffs, no code removed just because it looks unused in one backend.
- No refactor that makes explicit numeric massN silently search or switch data devices.
- Do not merge PR #24 until a native PS2 artifact boots on at least USB and its audio, input, save, and IGR can be verified. Keep the last known hardware-good revision and current branch independently recoverable.

## Recommended next investigation sequence

1. Validate latest PR #24 ELF starts reliably via inherited USB and reaches gameplay, not just the title screen.
2. Capture one frame time histogram and streaming-read latency distribution over a fixed battle scene on console; if frame-rate regresses, bisect recent runtime-only commits against the checkpoint.
3. Exercise one device at a time; isolate SIF/PAD/GS hangs from file open/read hangs with the last displayed boot stage and log.
4. Address actual measured hot spots via limited patches and deterministic guard tests, then repeat source/CI/hardware review.

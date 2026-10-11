# PS2 performance continuation

Current base: cdcc6de4862ce49ad2b1c4ac823c99cc91d2952e.

## Recovered Kimi state

The workspace kimihandoff.zip contains session_e27be64e-e6ad-4d68-8c78-d3ac3fbd39ee, exported October 7, 2026 (Los Angeles). Its final turns stopped at a provider usage limit. Kimi completed the baseline rollback and six IGR/deadzone cherry-picks and built that tree. Combiner strength reduction, culling optimization, and performance measurement remained unfinished.

The current graphics-performance branch additionally contains the graphics, inherited BDM-manager/service preservation, and audio RPC-buffer ownership corrections. Continue here without repeating the older rollback.

## Completed continuation

- Decode combiner selectors when building a draw mode instead of for each vertex.
- Reduce finite (A-A)*C+D and (A-B)*0+D expressions to D. Keep two-cycle combined inputs, fog alpha, and dynamic texel-alpha decal pass values.
- Compute texture-size reciprocals once per binding. GS dimensions are powers of two, so signed coordinate normalization retains its result.
- Leave clipping and culling decisions unchanged in this candidate.

## Evidence

Native PS2 compile/link passed. Host C differential checks compared 200,000 random one/two-cycle configurations with the prior evaluator, including fog and both decal-alpha values; all eight output floats compared numerically equal. Signed texture normalization also compared equal for all GS texture sizes from 8 through 1024.

The local host SHADE microbenchmark for 1,000,000 vertices measured 26 ms for the reference and 11 ms for the compiled evaluator. This is a host evaluator measurement, not PS2 frame rate or a whole-renderer benchmark. The existing PS2_PORT_STATUS.md figure of approximately 31 FPS and 17-18 ms GBI time is an older PCSX2 observation, not a current console measurement.

Local standalone comparison harness: ../out/combiner-test.c. Existing renderer coordinate tests also remain applicable.

## Remaining

- Console result for the candidate: gameplay lag and graphics, plus MX4SIO inherited-mass boot.
- Use measured frame/GBI results to choose the next bottleneck.
- Culling division removal was Kimi's next proposed optimization; it has not been implemented because winding decisions near degenerate geometry require separate numerical coverage.

## Cooper release comparison and controlled renderer restoration

User reports Cooper original release runs smoothly on the same console while bb113e8 remains severely laggy. Verified johnson-cooper/ssb-decomp-re v1.0 is be273112ebbdf2d5dd4f0da41d8fbe58512af3b3. Restore the seven renderer/VI files directly from that exact release, retaining only ps2_gs_prepare_exec for current IGR. This restores early DISPFB queuing, upstream GBI/texture semantics and static GS initialization, and removes the unproven combiner optimization. Storage, input and current audio transport stay intact. This isolates renderer differences; it does not establish the root cause or a console performance pass. If severe lag persists, compare the custom audio RPC transport against release SDR next, preserving hardware boot compatibility.

## Console feedback and texture follow-up

User confirms c17dce07f eliminates lag, but Mario hat, Pikachu face, Mario fireballs and intro light remain visually wrong. Preserve the Cooper GS/VI baseline. Reapply only masked/clamped tile materialization and signed STQ rectangle coordinates from 7e409f7b2; retain upstream combiner, diagnostics and presentation. Coordinate harness passes 8,388,608 cases. Intro light alpha/blending cause remains unresolved and requires further evidence; no console graphics or performance pass claimed for this follow-up.

## MMCE severe gameplay lag follow-up (2026-10-09)

User reports MMCE gameplay is effectively unplayable and recalls an earlier
working revision. No specific original binary has been identified yet.
Source history confirms 5336b065 deliberately disabled MMCEDRV promotion;
the current game retains the persistent MMCEMAN descriptor. Do not reconnect
the dormant reset/handoff experiment as a speculative performance repair.

5e9bc6eec removed MMCEMAN's 2 KiB read quantum and 500 us inter-chunk yield,
leaving 16 KiB synchronous bursts through shared SIO2. Restore that narrow
policy from 00b4a3a6b, preserving retries and other transport read limits.
This is a scheduling regression candidate, not proof of the reported FPS
cause; smaller requests and yields can increase total loading time.

The existing SELECT+R3 performance overlay now reports asset I/O wait ms/s
alongside total bytes and renderer time. It counts synchronous seek/read,
retry and reopen elapsed time, excludes lock acquisition and memory-resident
copies, and includes scheduling preemption during a read. Interpret it as
wall time in the asset I/O path, not pure card transfer time. If bytes stop
increasing and I/O wait is zero during severe lag, continue renderer/audio
investigation rather than further changing MMCE transfer sizes.

Source-extracted host regression covers bounded reads, partial reads, retries,
and unchanged non-MMCE policy. Native ELF compile/link passed. Actual MMCE
FPS, audio, input and IGR acceptance remains pending on the candidate.

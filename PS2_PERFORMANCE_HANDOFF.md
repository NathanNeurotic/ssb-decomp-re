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

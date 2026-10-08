# Reliable PS2 baseline ? 2026-10-08

Console-tested candidate: `3a6d84211d6ca3bfe6fe4a65aa347d52d3003a38`.
Artifact: https://github.com/NathanNeurotic/ssb-decomp-re/actions/runs/37826998331/artifacts/11571831659
SSB64.ELF SHA256: `a1960e1c52a1843992629e929292f8ba86e0fbeb72961657c2e68b22e95f068c`.

## Confirmed by the user on console

- Severe lag is gone following the Cooper v1.0 renderer/VI restoration (`c17dce07f`).
- Mario hat and Pikachu face textures are fixed in the texture follow-up (`3a6d84211`).
- USB launches work.

## Remaining hardware review

- Mario fireballs were reported malformed; no follow-up pass yet.
- Intro light was reported as a translucent box; cause and correction remain unresolved.
- Updated MX4SIO and other mass backends still need explicit confirmation.
- IGR behavior still needs confirmation for this baseline.

## Source provenance and boundaries

Cooper release v1.0 is `be273112ebbdf2d5dd4f0da41d8fbe58512af3b3`.
The baseline restores that renderer and VI presentation, retaining the IGR exit hook.
The texture follow-up adds masked/clamped tile materialization and signed STQ rectangle coordinates.
Storage/service preservation, input and custom audio transport remain in the fork.
Host coordinate checks, native build and exact-candidate CI pass; those do not replace hardware results.

Main synchronization preserves existing main history through an explicit ours-strategy merge of its four divergent commits. Their older runtime tree is deliberately superseded by the console-tested baseline. Checkpoint documentation is the only change after the tested candidate before that merge.

Use this baseline for further narrow fixes. Do not restore the superseded speculative renderer changes.

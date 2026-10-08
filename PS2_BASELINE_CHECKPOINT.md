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

## DAT webpage preservation

Restored docs/ asset builder from 61eb916f8, including the hardware DAT post-operations for stale CI texel/TLUT classification and Pikachu accessory costume animation streams. The expected fixed browser DAT hash is 151b492025ac780c5114b8a842b157c73ed56208e7ddfff1731ad74882eb809e. Browser generation still differs from compiler generation in pre-existing file 199/200 text regions; this restoration does not claim full pack parity. No runtime code or ELF input changed.

## USB boot failure follow-up

User subsequently reports USB hangs on a purple/pink screen; prior USB success is not a universal launch pass. Candidate restores native-launch stdout RPC suppression (present in pre-baseline main but lost during runtime rollback) and adds distinct IOP/GS markers. The probable stage is IOP setup; exact blocking operation remains unconfirmed. Preserve renderer/VI/texture behavior.

USB follow-up screenshot shows teal. This could be fileXio binding or the old dark-cyan pre-video marker, so colour alone does not uniquely identify the operation. Remove the unnecessary fileXio dependency for inherited BDM mounts; POSIX access keeps the launcher file I/O client instead. Fresh stacks still initialize fileXio and check module load errors. Pre-video marker changed to dark red to disambiguate. Console test remains required.

# Clean PS2 restart provenance

This branch descends directly from the **johnson-cooper/ssb-decomp-re PS2 v1.0 release**,
tag `v1.0` / branch `fix/ps2-hardware-iop-boot`, exact commit
`be273112ebbdf2d5dd4f0da41d8fbe58512af3b3` (2026-09-30).

The only source-code changes carried over from the later NathanNeurotic port are
DAT-generation corrections:

- `ps2/tools/build_assets.py`: avoid swapping texture data as a palette after
  a runtime-selected MObj texture-image source replaces the last static one.
- `src/relocData/341_PikachuModel.c`: correctly type Pikachu's two accessory
  material-animation script references rather than treating them as TLUTs.
- `docs/core/build.js`: keep the browser-generated DAT aligned with those
  hardware asset fixes, including explicit conversion post-operations and its
  expected verified output hash.

Source for these three files: NathanNeurotic/ssb-decomp-re `0aeed9e48d7dd93be50709c25f33bef56c8a5ce2`.

**Not carried over:** later IOP resets, BDM/USB rescue, device pickers,
MMCE experiments, generic streaming or rendering changes, SPU/IGR, extra PS2
runtime modules, or PR #24's expanded asset loader. This is intentionally
not a claim of working gameplay or of improved performance.

The old branches and PRs remain intact. Build the release baseline and
verify the DAT compiler/browser outputs against the known-good inputs
before introducing any new PS2 runtime architecture.

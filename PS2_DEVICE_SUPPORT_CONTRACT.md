# SSB64 PS2 device-support implementation contract

Baseline: `restart/cooper-v1.0-dat-only`, originally `c228be88907ec5d2075b9dd329062d627683f2e7`, based directly on johnson-cooper PS2 v1.0 `be273112ebbdf2d5dd4f0da41d8fbe58512af3b3`.

This document records source-audited requirements and the progress made in PR #25. **Native builds do not establish retail-console device compatibility.** Preserve the DAT fixes in `ps2/tools/build_assets.py`, `src/relocData/341_PikachuModel.c`, and `docs/core/build.js`, as well as the website.

## Current isolated implementation (PR #25)

- Based on Cooper PS2 v1.0 and the DAT-only correction branch, not the experimental #24 runtime.
- One compiled `ssb64` target creates three distributed files: standard ELF, filename-triggered 240p copy, and unstripped ELF with debugging symbols. They share the same device code.
- `--data=<absolute-device-directory>` or `--data=<absolute-device-path-to-SSB64.DAT>` selects the **specific** DAT file without searching drives. Example: `--data=mass1:/SSB64/SSB64.DAT`; to override from another device use `--data=udpfs:/SSB64/` or `--data=hdd0:+OPL:pfs:/SSB64/`.
- **Typed `usbN:` and shared `massN:` are distinct real PS2SDK BDMFS namespaces.** The former selects partitions belonging to the USB block-device family; the latter is global enumeration order and can belong to USB, ATA, MX4SIO, i.LINK or another BDM source. Never automatically translate `usb1:` into `mass1:`. launcHER may explicitly normalize its own launch paths *before ExecPS2*, but that convention is not universal. Preserve the exact supplied namespace and number. `bdmN:` remains ambiguous and is rejected.
- Supports inherited fileXio preservation and reuse of known resident IOP services; fresh IOP boot has device-specific typed USB, ATA/exFAT BDM, MX4SIO, i.LINK, UDPBD/UDPFS (from valid `IPCONFIG.DAT`), APA/PFS, and MMCE initialization. **Shared `massN:` cannot identify which physical transport owned that index after an IOP reset, so refuse blind USB reinitialization.** Inherit the live namespace when possible, or supply a transport-typed absolute DAT path.
- Pins the MMCE source already present in the fork, initializes its SIO2 hook before PAD/MC clients for fresh IOP, and retries **only the exact named APA partition** for a bounded interval.
- When loading the DAT, the USB/BDM startup probe holds open the exact successful descriptor for the asset manager, removing the close/reopen race. The fatal screen reports the exact DAT pathname and failed-open errno, or an exact bad-header/version, incomplete-copy or read-failure reason once the DAT opens. An absent mount and a missing DAT can produce the same open error; do not claim to distinguish them via an extra potentially blocking USB root-directory RPC. Host tests execute production asset bootstrap with synthetic good/bad DATs.
- Native ELF builds and host boot-path tests pass. Retail console startup, multi-drive enumeration, physical transport mount names, sustained MMCE stream/controls, IOP-reset behavior, network interoperability, and audio coexistence **are not yet proven**.
- Some driver families expose files under `massN:` rather than a transport-specific path. Supplying a physical transport label such as `mx4sio:` does not itself prove the corresponding filesystem name is valid. The caller should use the actual mounted path and can explicitly request the driver via that path only where that device namespace is supported.

## Original v1.0 baseline limitations (historical; being addressed)

- `ps2/src/platform/iop.c:ps2_iop_init` always resets the IOP when the boot-device type is not HOST. This destroys inherited BDM, USB, network, and other launcher services. The current bootstrap then always loads iomanX/fileXio rather than negotiating with already-resident services.
- `ps2/src/platform/iop.c:ps2_iop_load_boot_device_drivers` loads USB modules for MASS and UNKNOWN, mmceman for MMCE, and has an explicit unimplemented HDD branch. There is no generic UDPFS/MX4SIO/i.LINK/exFAT setup in this baseline.
- `ps2/src/storage/bootpath.c` preserves literal massN paths but rewrote some storage aliases to unindexed `mass:`, without preserving verified device identity. Treat that as an unsafe ambiguity; do not map the first matching DAT to a different physical device.
- `ps2/src/storage/assets.c` opens `SSB64.DAT` next to the ELF and then uses `ps2_file_*`; the storage API in `ps2/src/storage/file.c` already abstracts open/read/seek/close. Keep the DAT parser unchanged.
- `ps2/ps2.yaml` currently declares one native EE target (`ssb64`). Confirm the distribution's three ELF variants and their build provenance before asserting all three have distinct backends. Output filenames or video-mode variants need not imply three separate storage implementations.

## Implementation phases (do not combine with renderer/audio refactors)

1. **Preflight and provenance:** Parse complete launch path, device prefix and numerical index, optional explicit DAT path, and launcher-provided arguments *before* any reset. Never silently select another massN or partition based solely on DAT presence. Preserve the original argv path and record failures separately (service unavailable vs file missing vs permission/mount/read error).
2. **Environment negotiation:** For an inherited IOP, prove compatible RPC service bindings and visibility of the **same exact launch device** before reusing it. Never infer compatibility solely because a module load fails or one RPC bind transiently fails. Avoid duplicate module loads and preserve original filesystem state whenever possible.
3. **Fresh-IOP bootstrap:** When inheritance is unavailable or explicitly reset by the launcher, initialize a minimal, ordered device-specific stack. Ensure version/import dependency checks and bounded retry/failure paths. Do not assume old FILEIO and new fileXio expose identical modern BDM mounts.
4. **Device backends:** Support USB multi-drive FAT, MX4SIO, MMCE, HDD APA/PFS, HDD exFAT when a compatible backend exists, UDPFS/UDPBD, i.LINK and memory-card/host paths. Differentiate `massN:` logical partitions from physical devices. Validate supported filesystems and conflicting driver stacks independently.
5. **Storage API:** Keep `ps2_file_*` as the only DAT I/O boundary. Use explicit paths where available and ELF-adjacent DAT as the fallback. Ensure large reads, short reads, failures and seeks retain caller-visible semantics; no silent redirection.
6. **Performance isolation:** Do not change GS, SPU2, game scheduling, or DAT content while introducing device support. Benchmark blocking DAT operations separately from game/render/audio performance. Storage support and playable FPS are separate acceptance criteria.
7. **Build and regression gates:** Compile all published ELF variants and verify build artifacts. Run host-side path-mapping, identity, reset/inheritance, dependency-failure, invalid-DAT, short-read, and stale-service tests. Verify generated DAT hashes and website outputs are unchanged. Emulator runs are useful but do not prove every hardware transport.

## Lessons from earlier experiments (historical PR #24)

Reset OFF worked with the user's launcher, while Reset ON lost inherited storage identity and exposed multiple `massN:` DAT candidates. A manual selection menu avoided an arbitrary wrong pick but did not reconstruct trusted physical identity. Earlier performance changes did not establish playable frame pacing. Do not cherry-pick PR #24's broad storage stack, asset loader, audio or renderer modifications into the clean branch.

## Reference implementations to inspect

- `NathanNeurotic/launcHER`: launcher environment and device initialization, especially handoff arguments and reset policy.
- `NathanNeurotic/Open-PS2-Loader` (RiptOPL): multi-device support, individual backends, and network paths.
- `NathanNeurotic/Launchstation`: the fixed multi-USB launch handling and launch-time IOP setting.
- PS2SDK: fileXio/FILEIO service compatibility, dependency and mount semantics.
- Original Cooper PS2 v1.0 commit above: authoritative runtime behavior to preserve.

These are **references**, not modules to import wholesale.

## Non-negotiable acceptance criteria

- Identical valid DAT is read from its explicit, original device without substitution.
- Both inherited-IOP and launcher-reset modes fail safely or initialize the proper stack, without indefinite waits.
- Two USB drives do not trigger speculative selection of a third logical `massN:` entry.
- Failed service initialization or mount is identified distinctly from missing DAT.
- All published ELFs share a single storage behavior contract.
- DAT builder, website, renderer, audio scheduling, and original gameplay logic remain untouched by device-support patches.
- Automated tests and CI are required before hardware claims. Do not transfer an iterative hardware-debugging burden to the user.

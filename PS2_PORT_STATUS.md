# PS2 port — status

Last updated 2026-09-29. Everything below was observed in **PCSX2** (booting
the ELF from `host:`) unless stated otherwise. **Nothing has been tested on
real PS2 hardware yet.** See [PS2_PORT.md](PS2_PORT.md) for the design.

## Works (verified in PCSX2)

| area | notes |
|---|---|
| Build | `ps2build build` produces `ssb64.elf`; `prepare_assets.sh` pipeline builds `SSB64.DAT` (2308 regions, 21 MiB) from the US ROM |
| Boot | IOP modules, VBlank, GS, threads, asset pack from `argv[0]` directory (`host:`) |
| Front end | "No Controller" check, Nintendo/HAL logo, full opening movie, title, Mode Select, 1P Game character select (portraits, fighter model, options) |
| Scene changes | leaving and re-entering scenes (overlay `.data`/`.bss` reset on load) |
| Gameplay | 1P Game stage 1 (Mario vs CPU Link, Hyrule Castle): entry animations, HUD, timer, damage, CPU opponent, player movement/attacks from the keyboard-mapped pad |
| Rendering | textured/lit 3D, sprites, CI4/CI8/I/IA/RGBA16/RGBA32 textures, clipping, fog, alpha blend/test, depth |
| Memory | 13.1 MB committed at boot and in the match (budget 24 MB); scene arena 1.5 of 6 MB used in the 1P match; peak equals steady state so far |
| Debug overlay | Select + R3 |

## Measured performance (PCSX2, 1P match on Hyrule)

- ~31 FPS on the debug overlay.
- GBI translation ~17–18 ms per frame on the EE (3200 commands, ~470
  triangles). This is the main cost; menus take 1–2 ms.
- Texture VRAM pool (3.4 MiB) is full in the match; evictions stop once the
  match is running (steady working set).

The frame rate the N64 original achieves in this scene has not been measured
for comparison.

## Not working / not implemented

- **Audio is silent.** AI/SP audio tasks complete without output; the SPU2
  backend (and offline VADPCM → PS-ADPCM conversion) is not written.
- **Performance** is below target: no static display-list caching, no VU1
  transform path, combiner evaluated per vertex on the EE.
- **CPU framebuffer writes** (staff roll, congratulations screens) are not
  mirrored to the GS; those screens will be wrong.
- **HDD boot** is not implemented (USB mass, memory card, MMCE and `host:`
  paths exist; only `host:` has been exercised).

## Known visual issues

- Character select: the scrolling "Ready to fight" banner is garbled.
- 2 unknown GBI commands per frame in the match (skipped).
- Title/other scenes: the Yoshi heart / blocky cloud issue seen earlier was
  not re-checked after the texture fixes.

## Not yet verified

- Real hardware (any model), and boot from USB / memory card / MMCE.
- Memory card saves: the save path is implemented (A/B slots, CRC, debounced
  flush) but the test setup had no formatted card, so saves stayed in RAM.
- Multitap / 4 players; DualShock rumble.
- VS mode, other stages and characters, a full 1P run, bonus stages, ending.
- The texel-alpha two-pass decal combiner path (no scene seen so far uses it).
- relocData file 200 (`SYSignValidate`, anti-tamper data) is 40 bytes larger
  natively than in the ROM; the anti-tamper checks are disabled on PS2, so it
  is not read, but this is unconfirmed for all scenes.

## Next steps (in priority order)

1. SPU2 audio backend.
2. Performance: cache translated static DLs, move transforms to VU1.
3. Fix the character-select banner and the unknown GBI commands.
4. Verify VS mode, all stages/characters, memory card saves, multitap.
5. GS mirroring for CPU framebuffer writes; HDD boot.
6. Real hardware testing.

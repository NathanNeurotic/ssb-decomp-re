# Super Smash Bros. 64 — native PlayStation 2 port

This directory tree adds a **native** PS2 build of the SSB64 decompilation. The
game's C code is compiled for the Emotion Engine with the PS2Build SDK; there
is no N64 CPU/RSP/RDP emulation. libultra is replaced by a thin PS2 platform
layer, and F3DEX2 display lists are translated to GS primitives at runtime.

The N64 matching build is unaffected: all PS2 code lives under `ps2/`, and
the few edits to game sources are behind `#ifdef PLATFORM_PS2` (or are
`AVOID_UB` fixes that are no-ops for the matching build).

Current state is tracked in [PS2_PORT_STATUS.md](PS2_PORT_STATUS.md).

## Legal

- **No game data is distributed.** `baserom.us.z64`, ROM dumps, the
  extracted `assets/`/`relocAssets/` trees, generated `build/` sources and the
  asset pack `SSB64.DAT` are all git-ignored and must never be committed. You
  need your own cartridge dump; the pack is built from it locally.
- The upstream decomp repository currently has **no LICENSE file**. Until it
  has one, this port's code (which is derived from it) carries no licence
  grant either; do not redistribute builds. Third-party components used at
  build/run time: ps2sdk, gsKit, dmaKit and the IOP modules shipped by
  PS2Build, each under its own open-source licence (see the packages). `ps2/ps2_linkfile.ld` is derived from the
  ps2sdk default linkfile (AFL 2.0, credited in the file).

## Building

Requirements: the PS2Build SDK (`ps2build` on PATH), Python 3, and for asset preparation the decomp's own
extraction prerequisites (GNU make, splat; see README.md). IDO is **not**
needed.

```bash
ps2/tools/prepare_assets.sh          # once per ROM / asset change -> ps2/build/bin/SSB64.DAT
cd ps2 && ps2build build             # -> ps2/build/bin/ssb64.elf
```

Copy `ssb64.elf` and `SSB64.DAT` to the same directory on any boot device
(USB mass storage, memory card, MMCE, or `host:` in PCSX2) and run the ELF.
The boot code finds the pack next to the ELF from `argv[0]`; no path is
hard-coded.

Testing in PCSX2 (Windows helpers, default Pad 1 keyboard bindings):

```bash
powershell -File ps2/tools/run_pcsx2.ps1 -Seconds 20       # boot + print the port's log lines
powershell -File ps2/tools/goto_1p_match.ps1              # drive the menus into a 1P match
powershell -File ps2/tools/screenshot_pcsx2.ps1 -Out shot.png
```

### Build layout (`ps2/ps2.yaml`)

| target | kind | contents |
|---|---|---|
| `ssb_platform` | ee_lib | `ps2/src/*`: platform, memory, storage, input, audio, renderer, libultra replacement |
| `ssb_game` | ee_lib | the decomp's game sources (`src/`) plus the libultra parts that are pure C (gu, sp sprites, audio synthesis, libc) |
| `ssb64` | ee | `ps2/src/boot/main.c`, linked with a generated linkfile and the IOP modules embedded |

Game code is compiled with `-DPLATFORM_PS2 -DNON_MATCHING -DAVOID_UB`,
`-fno-strict-aliasing -fwrapv -fsingle-precision-constant`. The two libraries
are linked twice to resolve their mutual references.

## Architecture

### Threads and messages (`ps2/src/ultra/`)
`OSThread` maps onto EE kernel threads (priority `127 - pri`, stacks from
pools). Message queues keep libultra semantics (waiter lists, blocking and
non-blocking send/receive) on top of `SleepThread`/`WakeupThread`. The VBlank
interrupt drives VI retrace events and display-buffer swaps. SP tasks
(`osSpTaskStartGo`) are dispatched to the renderer (gfx) or completed
immediately (audio), and post the same SP/DP events the game waits for.

### Renderer (`ps2/src/renderer/`)
- GS: NTSC 240p, 320×240, three 16-bit (CT16S) framebuffers + Z16S; the
  texture pool uses the rest of the 4 MiB VRAM (~3.4 MiB).
- `gbi.c` walks F3DEX2 display lists (matrices, lighting, clipping against
  near/guard planes, RDP state) and emits GS primitives into a GIF DMA chain,
  batching triangles by GS state. The colour combiner is evaluated per vertex
  as `k·T + c` and mapped to GS MODULATE; lerps by texel alpha use a two-pass
  decal. The RDP blender maps to GS ALPHA/TEST.
- `texcache.c` converts N64 textures to GS formats (CI4/CI8 → T4/T8 + CLUT,
  I/IA → T4/T8 with fixed CLUTs, RGBA16 → CT16, RGBA32/IA16 → CT32), handles
  TMEM odd-row interleave, 32-bit TMEM half-word addressing and mask/clamp
  wrap semantics, and keeps textures VRAM-resident with LRU eviction keyed by
  source address + content.
- `overlay.c`: debug overlay (Select + R3): FPS, GBI time, memory by
  category, VRAM, texture and packet stats.

### Memory (`ps2/src/memory/`)
Every platform allocation is tracked per category (`ps2_mem_*`). The N64
game allocates scene heaps from "end of overlay BSS"; on PS2 all overlay
linker symbols resolve to one 6 MiB scene arena (`arena_glue.S`, generated
by `gen_arena_glue.py`) followed by the framebuffer block the game expects.

All N64 code overlays are linked statically. To keep N64 semantics, the
generated linkfile groups each overlay's `.data`/`.bss`; `syDmaLoadOverlay`
restores the overlay's initial data image and clears its bss, exactly what
the N64 DMA reload did (`overlay_state.c`).

### Assets (`ps2/tools/`, `ps2/src/storage/assets.c`)
The game reads everything through PI DMA from ROM addresses. `SSB64.DAT`
contains *virtual ROM regions*; `osPiStartDma` becomes a region lookup + file
read (a small metadata part is resident).

- relocData (2132 files): the decomp's typed sources are compiled natively
  with the EE gcc (o32 ABI, so layout matches IDO), sizes are checked against
  the ROM, and the ELF relocations are re-encoded into the game's reloc-chain
  format, so `lbreloc.c` runs unmodified.
- Endianness policy: everything the CPU reads is native (little-endian);
  **texel data stays in N64 byte order** (the converter reads it as such);
  **palettes are native u16**. `build_assets.py` fixes the places where the
  decomp's typing does not match how the data is used (mistyped script blocks,
  AnimJoint animations typed as u16 figatree data, u16/u32-typed texels,
  palettes inside u8 blobs, sequence/bank headers).
- Bitfields read from integer-encoded scripts are reversed for LE by
  `le_bitfields.py`; C-initialised structs keep declaration order.

Stage 1 (`n64prep.mk`) runs the decomp's own extractor without IDO; stage 2
(`build_assets.py`) compiles and packs. `prepare_assets.sh` runs both.

### Input (`ps2/src/input/pad.c`)
libpad + libmtap, up to four players (multitap on port 1, or port 1 + a
multitap on port 2). One mapping table (Cross = A, Square = B,
Triangle/Circle = C-up/C-right, L1/R2 = Z, R1 = R, L2 = L, right stick = C
buttons). The left stick is rescaled to the N64 range with a dead zone.
Rumble maps to the DualShock small motor.

### Saves (`ps2/src/storage/save.c`)
The 32 KiB SRAM image is kept in RAM and flushed to the memory card by a
low-priority thread, debounced. Two slots (A/B) with sequence number + CRC;
the newest valid slot wins at boot, so a torn write never loses the previous
save. Without a formatted card, saves stay in RAM.

### Boot (`ps2/src/platform/boot.c`, `iop.c`)
Order: log → memory → boot path from `argv[0]` → IOP modules (embedded IRX:
iomanX, fileXio, sio2man, padman, mtapman, mcman/mcserv, libsd, sdr, bdm +
FAT, USB mass storage, mmceman) → VBlank/GS → threads/VI → scene arena →
overlay state → input → assets → saves → audio → render thread → the game's
own `syMainLoop`.

### Game-source changes
- `include/PR/rcp.h`: register reads/writes go through the platform layer.
- `apply_ps2_guards.py`, `fix_missing_returns.py`, `le_bitfields.py`:
  scripted, reviewable edits (colour packing, top-of-RAM assumptions,
  unsigned-loop UB, missing returns, anti-tamper checks, LE bitfields).
- `src/ft/ftchardata.h`, `src/sys/dma.c` (overlay reset hook) and a few
  one-line fixes.

## Debugging

`ps2_log` output goes to the PCSX2 console log (`logs/emulog.txt`). These
globals can be poked from the PCSX2 debugger (addresses via `nm` on
`ps2/build/obj/ssb64/ssb64.unstripped.elf`):

| variable | effect |
|---|---|
| `gPS2GbiTrace = N` | log the next N GBI commands |
| `gPS2TexTrace = N` + `gPS2TexFlush = 1` | re-convert all textures and log N conversions with tile state |
| `gPS2CombTrace = 1` | log each distinct textured combiner setup once |
| `gPS2TlutTrace = N` | log N CI texture binds with the commands that loaded texel/TLUT data |

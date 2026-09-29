#!/usr/bin/env python3
"""Re-apply the small PLATFORM_PS2 guards in shared game sources.

These edits are already committed; the script documents them in one place
and can re-apply them after merging upstream decomp changes that touch the
same lines. Every guard keeps the N64 code path unchanged (#else branch), so
the matching N64 build is unaffected.

Usage (repo root): python3 ps2/tools/apply_ps2_guards.py
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def patch(path, old, new, count=1):
    path = os.path.join(ROOT, path)
    with open(path, encoding="utf-8", newline="") as f:
        s = f.read()
    if "\r\n" in s:
        old = old.replace("\n", "\r\n")
        new = new.replace("\n", "\r\n")
    if new in s:
        print("already applied:", os.path.relpath(path, ROOT))
        return
    n = s.count(old)
    if n != count:
        sys.exit("%s: expected %d match(es), found %d" % (path, count, n))
    s = s.replace(old, new)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(s)
    print("applied:", os.path.relpath(path, ROOT))


GUARDS = [
    # 1. SYColorPack <-> 0xRRGGBBAA words, independent of endianness.
    ("include/ssb_types.h",
     "} SYColorPack;\n",
     "} SYColorPack;\n"
     "\n"
     "#ifdef PLATFORM_PS2\n"
     "// SYColorPack.pack only equals 0xRRGGBBAA on big-endian CPUs; code that moves\n"
     "// colors between u32 words and SYColorPack uses these on little-endian PS2.\n"
     "#define SYCOLOR_PACK_TO_WORD(c) \\\n"
     "    (((u32)(c).s.r << 24) | ((u32)(c).s.g << 16) | ((u32)(c).s.b << 8) | (u32)(c).s.a)\n"
     "#define SYCOLOR_PACK_FROM_WORD(c, w) \\\n"
     "    ((c).s.r = (u8)((w) >> 24), (c).s.g = (u8)((w) >> 16), (c).s.b = (u8)((w) >> 8), (c).s.a = (u8)(w))\n"
     "#endif\n"),

    # 2. objdisplay: light colors are sent to the RSP as 0xRRGGBB00 words.
    ("src/sys/objdisplay.c",
     "            gSPLightColor(branch_dl++, LIGHT_1, mobj->sub.light1color.pack);\n",
     "#ifdef PLATFORM_PS2\n"
     "            gSPLightColor(branch_dl++, LIGHT_1, SYCOLOR_PACK_TO_WORD(mobj->sub.light1color));\n"
     "#else\n"
     "            gSPLightColor(branch_dl++, LIGHT_1, mobj->sub.light1color.pack);\n"
     "#endif\n"),
    ("src/sys/objdisplay.c",
     "            gSPLightColor(branch_dl++, LIGHT_2, mobj->sub.light2color.pack);\n",
     "#ifdef PLATFORM_PS2\n"
     "            gSPLightColor(branch_dl++, LIGHT_2, SYCOLOR_PACK_TO_WORD(mobj->sub.light2color));\n"
     "#else\n"
     "            gSPLightColor(branch_dl++, LIGHT_2, mobj->sub.light2color.pack);\n"
     "#endif\n"),

    # 3. objanim: color tracks interpolate 0xRRGGBBAA words with a
    #    big-endian byte trick; PS2 does the same per-channel math portably.
    ("src/sys/objanim.c",
     "                    case nGCAnimKindLinear: \n"
     "                        interp = (aobj->length * aobj->length_invert * 256.0F);\n",
     "                    case nGCAnimKindLinear: \n"
     "                        interp = (aobj->length * aobj->length_invert * 256.0F);\n"
     "#ifdef PLATFORM_PS2\n"
     "                        {\n"
     "                            u32 w0 = *(u32*)&aobj->value_base, w1 = *(u32*)&aobj->value_target;\n"
     "                            s32 t = (interp < 0) ? 0 : (interp > 256) ? 256 : interp;\n"
     "\n"
     "                            color.s.r = (u8)((((w0 >> 24) & 0xFF) * (256 - t) + ((w1 >> 24) & 0xFF) * t) >> 8);\n"
     "                            color.s.g = (u8)((((w0 >> 16) & 0xFF) * (256 - t) + ((w1 >> 16) & 0xFF) * t) >> 8);\n"
     "                            color.s.b = (u8)((((w0 >> 8) & 0xFF) * (256 - t) + ((w1 >> 8) & 0xFF) * t) >> 8);\n"
     "                            color.s.a = (u8)(((w0 & 0xFF) * (256 - t) + (w1 & 0xFF) * t) >> 8);\n"
     "                        }\n"
     "                        break;\n"
     "#endif\n"),
    ("src/sys/objanim.c",
     "                        color = (aobj->length_invert <= aobj->length) ? *(SYColorPack*)&aobj->value_target : *(SYColorPack*)&aobj->value_base;\n",
     "#ifdef PLATFORM_PS2\n"
     "                        {\n"
     "                            u32 w = (aobj->length_invert <= aobj->length) ? *(u32*)&aobj->value_target : *(u32*)&aobj->value_base;\n"
     "\n"
     "                            SYCOLOR_PACK_FROM_WORD(color, w);\n"
     "                        }\n"
     "#else\n"
     "                        color = (aobj->length_invert <= aobj->length) ? *(SYColorPack*)&aobj->value_target : *(SYColorPack*)&aobj->value_base;\n"
     "#endif\n"),

    # 4. lbparticle: particle scripts store floats as big-endian bytes.
    ("src/lb/lbparticle.c",
     "\tbytes[3] = *csr++;\n\n\t*f = *(f32*)bytes;\n",
     "\tbytes[3] = *csr++;\n\n"
     "#ifdef PLATFORM_PS2\n"
     "\t{\n"
     "\t\tunion { u32 w; f32 f; } conv;\n"
     "\n"
     "\t\tconv.w = ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) | ((u32)bytes[2] << 8) | bytes[3];\n"
     "\t\t*f = conv.f;\n"
     "\t}\n"
     "#else\n"
     "\t*f = *(f32*)bytes;\n"
     "#endif\n"),

    # 4b. Instrument banks: instArray[] entries of offset 0 mean "no
    #     instrument". N64 relocates first and tests for NULL afterwards,
    #     which only works because the file header, misread as an
    #     ALInstrument, has a non-zero `flags` byte in big-endian order.
    ("src/sys/audio.c",
     "    for (i = 0; i < bank->instCount; i++)\n"
     "    {\n"
     "        bank->instArray[i] = (ALInstrument*) ((uintptr_t)bank->instArray[i] + offset);\n",
     "    for (i = 0; i < bank->instCount; i++)\n"
     "    {\n"
     "#ifdef PLATFORM_PS2\n"
     "        if (bank->instArray[i] == NULL)\n"
     "        {\n"
     "            continue;\n"
     "        }\n"
     "#endif\n"
     "        bank->instArray[i] = (ALInstrument*) ((uintptr_t)bank->instArray[i] + offset);\n"),

    # 4c. "Top of RDRAM" (0x80400000): framebuffer placement and the CPU
    #     loops that clear framebuffers up to it. On PS2 the top of game RAM
    #     is the end of the scene-arena + framebuffer block.
    ("src/sys/video.h",
     "#define SYVIDEO_DEFINE_FRAMEBUFFER_ADDR(width, height, w_border, h_border, type, id)   \\\n"
     "(                                                                                   \\\n"
     "    (0x80400000 - (((width) * (height) * sizeof(type)) * (3 - (id)))) -             \\\n",
     "#ifdef PLATFORM_PS2\n"
     "extern u8 gPS2TopOfRam[]; // ps2/src/memory/arena_glue.S\n"
     "#define SYVIDEO_RAM_END ((uintptr_t)gPS2TopOfRam)\n"
     "#else\n"
     "#define SYVIDEO_RAM_END 0x80400000\n"
     "#endif\n"
     "\n"
     "#define SYVIDEO_DEFINE_FRAMEBUFFER_ADDR(width, height, w_border, h_border, type, id)   \\\n"
     "(                                                                                   \\\n"
     "    (SYVIDEO_RAM_END - (((width) * (height) * sizeof(type)) * (3 - (id)))) -         \\\n"),
    ("src/sc/scmanager.c",
     "\tend = 0x80400000;\n",
     "\tend = SYVIDEO_RAM_END;\n"),
    ("src/sc/sccommon/scstaffroll.c",
     "\twhile ((uintptr_t)fb32 < 0x80400000) { *fb32++ = 0x00000000; }\n",
     "\twhile ((uintptr_t)fb32 < SYVIDEO_RAM_END) { *fb32++ = 0x00000000; }\n"),
    ("src/sc/sccommon/scstaffroll.c",
     "\twhile ((uintptr_t)fb16 < 0x80400000) { *fb16++ = GPACK_RGBA5551(0x00, 0x00, 0x00, 0x01); }\n",
     "\twhile ((uintptr_t)fb16 < SYVIDEO_RAM_END) { *fb16++ = GPACK_RGBA5551(0x00, 0x00, 0x00, 0x01); }\n"),
    ("src/mn/mncommon/mncongra.c",
     "\twhile ((uintptr_t)fb32 < 0x80400000) { *fb32++ = GPACK_RGBA8888(0x00, 0x00, 0x00, 0xFF); } // WARNING: Newline memes!\n",
     "\twhile ((uintptr_t)fb32 < SYVIDEO_RAM_END) { *fb32++ = GPACK_RGBA8888(0x00, 0x00, 0x00, 0xFF); } // WARNING: Newline memes!\n"),
    ("src/mn/mncommon/mncongra.c",
     "\twhile ((uintptr_t)fb16 < 0x80400000) { *fb16++ = GPACK_RGBA5551(0x00, 0x00, 0x00, 0x01); }\n",
     "\twhile ((uintptr_t)fb16 < SYVIDEO_RAM_END) { *fb16++ = GPACK_RGBA5551(0x00, 0x00, 0x00, 0x01); }\n"),

    # 5. Anti-tamper checks that execute N64 MIPS code shipped as data.
    ("src/sc/sc1pmode/sc1pgame.c",
     "    if (!(gSCManagerBackupData.error_flags & LBBACKUP_ERROR_VSBATTLECASTLE) && (gSCManagerBackupData.boot > 92))\n",
     "#ifdef PLATFORM_PS2\n"
     "    // The ROM-signature check runs N64 machine code from a relocData file.\n"
     "    if (FALSE)\n"
     "#else\n"
     "    if (!(gSCManagerBackupData.error_flags & LBBACKUP_ERROR_VSBATTLECASTLE) && (gSCManagerBackupData.boot > 92))\n"
     "#endif\n"),
    ("src/sc/sccommon/scvsbattle.c",
     "\tif (!(gSCManagerBackupData.error_flags & LBBACKUP_ERROR_1PGAMEMARIO) && (gSCManagerBackupData.boot > 68))\n",
     "#ifdef PLATFORM_PS2\n"
     "\t// The KSEG1 check runs N64 machine code from a relocData file.\n"
     "\tif (FALSE)\n"
     "#else\n"
     "\tif (!(gSCManagerBackupData.error_flags & LBBACKUP_ERROR_1PGAMEMARIO) && (gSCManagerBackupData.boot > 68))\n"
     "#endif\n"),
]


def main():
    for g in GUARDS:
        patch(*g)
    return 0


if __name__ == "__main__":
    sys.exit(main())

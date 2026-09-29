/*
 * Game-side glue for the PS2 build (compiled with the game's own headers).
 *
 * Only symbols the N64 build gets from outside the C sources live here:
 * microcode images, a libm constant from an assembly file, and the entry
 * points of the debug scenes whose overlays (src/db, src/ovl8) are not part
 * of the retail flow and are left out of the PS2 build.
 */
#include <common.h>

/* RSP microcode images: referenced only to fill OSTask fields, which the PS2
 * renderer/audio backends ignore. */
long long int gspF3DEX2_fifoTextStart[2];
long long int gspF3DEX2_fifoDataStart[2];
u64 n_aspMainTextStart[2];
u64 n_aspMainDataStart[2];

/* From src/libultra/gu/libm_vals.s */
const u32 __libm_qnan_f_bits = 0x7F810000;
extern f32 __libm_qnan_f __attribute__((alias("__libm_qnan_f_bits")));

/* Debug scenes (src/db) - reachable only from the debug menu overlay. */
void dbBattleStartScene(void) {}
void dbCubeStartScene(void) {}
void dbFallsStartScene(void) {}
void dbMapsStartScene(void) {}

/*
 * EE exception vector for crash reports (installed by ps2_crash_init).
 *
 * Runs in exception context: saves all GPRs (128-bit), HI/LO and the COP0
 * state into gPS2CrashFrame, then "returns" from the exception into
 * ps2_crash_report() on a private stack, so the report runs as ordinary
 * code with kernel services available (same approach as ps2link).
 */
    .set noreorder
    .set noat

    .text
    .align 3
    .globl ps2_crash_vector
    .ent ps2_crash_vector
ps2_crash_vector:
    lui     $k0, %hi(gPS2CrashFrame)
    addiu   $k0, $k0, %lo(gPS2CrashFrame)
    sq      $0,  0x000($k0)
    sq      $1,  0x010($k0)
    sq      $2,  0x020($k0)
    sq      $3,  0x030($k0)
    sq      $4,  0x040($k0)
    sq      $5,  0x050($k0)
    sq      $6,  0x060($k0)
    sq      $7,  0x070($k0)
    sq      $8,  0x080($k0)
    sq      $9,  0x090($k0)
    sq      $10, 0x0A0($k0)
    sq      $11, 0x0B0($k0)
    sq      $12, 0x0C0($k0)
    sq      $13, 0x0D0($k0)
    sq      $14, 0x0E0($k0)
    sq      $15, 0x0F0($k0)
    sq      $16, 0x100($k0)
    sq      $17, 0x110($k0)
    sq      $18, 0x120($k0)
    sq      $19, 0x130($k0)
    sq      $20, 0x140($k0)
    sq      $21, 0x150($k0)
    sq      $22, 0x160($k0)
    sq      $23, 0x170($k0)
    sq      $24, 0x180($k0)
    sq      $25, 0x190($k0)
    sq      $26, 0x1A0($k0)
    sq      $27, 0x1B0($k0)
    sq      $28, 0x1C0($k0)
    sq      $29, 0x1D0($k0)
    sq      $30, 0x1E0($k0)
    sq      $31, 0x1F0($k0)
    pmfhi   $k1
    sq      $k1, 0x200($k0)
    pmflo   $k1
    sq      $k1, 0x210($k0)
    mfc0    $k1, $12            /* Status */
    sw      $k1, 0x220($k0)
    mfc0    $k1, $13            /* Cause */
    sw      $k1, 0x224($k0)
    mfc0    $k1, $14            /* EPC */
    sw      $k1, 0x228($k0)
    mfc0    $k1, $8             /* BadVAddr */
    sw      $k1, 0x22C($k0)
    mfc0    $k1, $30            /* ErrorEPC */
    sw      $k1, 0x230($k0)

    /* resume in ps2_crash_report() with a fresh stack */
    lui     $k1, %hi(ps2_crash_report)
    addiu   $k1, $k1, %lo(ps2_crash_report)
    mtc0    $k1, $14
    sync.p
    lui     $sp, %hi(gPS2CrashStackTop)
    addiu   $sp, $sp, %lo(gPS2CrashStackTop)
    lw      $sp, 0($sp)
    lui     $gp, %hi(_gp)
    addiu   $gp, $gp, %lo(_gp)
    eret
    nop
    .end ps2_crash_vector

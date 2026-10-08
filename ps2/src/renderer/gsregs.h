/*
 * GS general-purpose register encodings and GIFtags (GS User's Manual).
 * Kept local to the renderer so packet building has no hidden dependency on
 * any particular helper library's macro spellings.
 */
#ifndef PS2_GSREGS_H
#define PS2_GSREGS_H

#include <stdint.h>

typedef uint64_t gs_u64;

/* Register addresses (A+D) */
#define GSR_PRIM       0x00
#define GSR_RGBAQ      0x01
#define GSR_ST         0x02
#define GSR_UV         0x03
#define GSR_XYZF2      0x04
#define GSR_XYZ2       0x05
#define GSR_TEX0_1     0x06
#define GSR_CLAMP_1    0x08
#define GSR_FOG        0x0A
#define GSR_XYZF3      0x0C
#define GSR_XYZ3       0x0D
#define GSR_TEX1_1     0x14
#define GSR_TEX2_1     0x16
#define GSR_XYOFFSET_1 0x18
#define GSR_PRMODECONT 0x1A
#define GSR_TEXCLUT    0x1C
#define GSR_TEXA       0x3B
#define GSR_FOGCOL     0x3D
#define GSR_TEXFLUSH   0x3F
#define GSR_SCISSOR_1  0x40
#define GSR_ALPHA_1    0x42
#define GSR_DTHE       0x45
#define GSR_COLCLAMP   0x46
#define GSR_TEST_1     0x47
#define GSR_SCANMSK    0x22
#define GSR_PABE       0x49
#define GSR_FBA_1      0x4A
#define GSR_FRAME_1    0x4C
#define GSR_ZBUF_1     0x4E
#define GSR_BITBLTBUF  0x50
#define GSR_TRXPOS     0x51
#define GSR_TRXREG     0x52
#define GSR_TRXDIR     0x53
#define GSR_FINISH     0x61
#define GSR_AD         0x0E /* GIFtag REGS nibble for A+D */

/* Pixel storage modes */
#define GSPSM_CT32  0x00
#define GSPSM_CT24  0x01
#define GSPSM_CT16  0x02
#define GSPSM_CT16S 0x0A
#define GSPSM_T8    0x13
#define GSPSM_T4    0x14
#define GSPSM_Z32   0x30
#define GSPSM_Z24   0x31
#define GSPSM_Z16   0x32
#define GSPSM_Z16S  0x3A

/* Primitive types */
#define GSPRIM_POINT     0
#define GSPRIM_LINE      1
#define GSPRIM_LINESTRIP 2
#define GSPRIM_TRI       3
#define GSPRIM_TRISTRIP  4
#define GSPRIM_TRIFAN    5
#define GSPRIM_SPRITE    6

#define GSV_PRIM(prim, iip, tme, fge, abe, aa1, fst, ctxt, fix)                                          \
    ((gs_u64)(prim) | ((gs_u64)(iip) << 3) | ((gs_u64)(tme) << 4) | ((gs_u64)(fge) << 5) |              \
     ((gs_u64)(abe) << 6) | ((gs_u64)(aa1) << 7) | ((gs_u64)(fst) << 8) | ((gs_u64)(ctxt) << 9) |       \
     ((gs_u64)(fix) << 10))

#define GSV_RGBAQ(r, g, b, a, q) \
    ((gs_u64)(r) | ((gs_u64)(g) << 8) | ((gs_u64)(b) << 16) | ((gs_u64)(a) << 24) | ((gs_u64)(q) << 32))

#define GSV_UV(u, v) ((gs_u64)(u) | ((gs_u64)(v) << 16))
#define GSV_XYZ(x, y, z) ((gs_u64)(x) | ((gs_u64)(y) << 16) | ((gs_u64)(z) << 32))

#define GSV_TEX0(tbp, tbw, psm, tw, th, tcc, tfx, cbp, cpsm, csm, csa, cld)                             \
    ((gs_u64)(tbp) | ((gs_u64)(tbw) << 14) | ((gs_u64)(psm) << 20) | ((gs_u64)(tw) << 26) |           \
     ((gs_u64)(th) << 30) | ((gs_u64)(tcc) << 34) | ((gs_u64)(tfx) << 35) | ((gs_u64)(cbp) << 37) |    \
     ((gs_u64)(cpsm) << 51) | ((gs_u64)(csm) << 55) | ((gs_u64)(csa) << 56) | ((gs_u64)(cld) << 61))

#define GSTFX_MODULATE   0
#define GSTFX_DECAL      1
#define GSTFX_HIGHLIGHT  2
#define GSTFX_HIGHLIGHT2 3

#define GSV_CLAMP(wms, wmt, minu, maxu, minv, maxv)                                                      \
    ((gs_u64)(wms) | ((gs_u64)(wmt) << 2) | ((gs_u64)(minu) << 4) | ((gs_u64)(maxu) << 14) |          \
     ((gs_u64)(minv) << 24) | ((gs_u64)(maxv) << 34))
#define GSWRAP_REPEAT        0
#define GSWRAP_CLAMP         1
#define GSWRAP_REGION_CLAMP  2
#define GSWRAP_REGION_REPEAT 3

#define GSV_TEX1(lcm, mxl, mmag, mmin, mtba, l, k)                                                        \
    ((gs_u64)(lcm) | ((gs_u64)(mxl) << 2) | ((gs_u64)(mmag) << 5) | ((gs_u64)(mmin) << 6) |          \
     ((gs_u64)(mtba) << 9) | ((gs_u64)(l) << 19) | ((gs_u64)(k) << 32))

#define GSV_XYOFFSET(ofx, ofy) ((gs_u64)(ofx) | ((gs_u64)(ofy) << 32))
#define GSV_SCISSOR(x0, x1, y0, y1) \
    ((gs_u64)(x0) | ((gs_u64)(x1) << 16) | ((gs_u64)(y0) << 32) | ((gs_u64)(y1) << 48))

#define GSV_ALPHA(a, b, c, d, fix) \
    ((gs_u64)(a) | ((gs_u64)(b) << 2) | ((gs_u64)(c) << 4) | ((gs_u64)(d) << 6) | ((gs_u64)(fix) << 32))
/* ALPHA selectors */
#define GSBL_CS 0
#define GSBL_CD 1
#define GSBL_ZERO 2
#define GSBL_AS 0
#define GSBL_AD 1
#define GSBL_FIX 2

#define GSV_TEST(ate, atst, aref, afail, date, datm, zte, ztst)                                           \
    ((gs_u64)(ate) | ((gs_u64)(atst) << 1) | ((gs_u64)(aref) << 4) | ((gs_u64)(afail) << 12) |       \
     ((gs_u64)(date) << 14) | ((gs_u64)(datm) << 15) | ((gs_u64)(zte) << 16) | ((gs_u64)(ztst) << 17))
#define GSATST_NEVER 0
#define GSATST_ALWAYS 1
#define GSATST_LESS 2
#define GSATST_LEQUAL 3
#define GSATST_EQUAL 4
#define GSATST_GEQUAL 5
#define GSATST_GREATER 6
#define GSATST_NOTEQUAL 7
#define GSAFAIL_KEEP 0
#define GSAFAIL_FB_ONLY 1
#define GSAFAIL_ZB_ONLY 2
#define GSAFAIL_RGB_ONLY 3
#define GSZTST_NEVER 0
#define GSZTST_ALWAYS 1
#define GSZTST_GEQUAL 2
#define GSZTST_GREATER 3

#define GSV_FOGCOL(r, g, b) ((gs_u64)(r) | ((gs_u64)(g) << 8) | ((gs_u64)(b) << 16))
#define GSV_FRAME(fbp, fbw, psm, fbmsk) \
    ((gs_u64)(fbp) | ((gs_u64)(fbw) << 16) | ((gs_u64)(psm) << 24) | ((gs_u64)(fbmsk) << 32))
#define GSV_ZBUF(zbp, psm, zmsk) ((gs_u64)(zbp) | ((gs_u64)((psm) & 0xF) << 24) | ((gs_u64)(zmsk) << 32))
#define GSV_TEXA(ta0, aem, ta1) ((gs_u64)(ta0) | ((gs_u64)(aem) << 15) | ((gs_u64)(ta1) << 32))

#define GSV_BITBLTBUF(sbp, sbw, spsm, dbp, dbw, dpsm)                                                     \
    ((gs_u64)(sbp) | ((gs_u64)(sbw) << 16) | ((gs_u64)(spsm) << 24) | ((gs_u64)(dbp) << 32) |         \
     ((gs_u64)(dbw) << 48) | ((gs_u64)(dpsm) << 56))
#define GSV_TRXPOS(ssax, ssay, dsax, dsay, dir) \
    ((gs_u64)(ssax) | ((gs_u64)(ssay) << 16) | ((gs_u64)(dsax) << 32) | ((gs_u64)(dsay) << 48) | ((gs_u64)(dir) << 59))
#define GSV_TRXREG(rrw, rrh) ((gs_u64)(rrw) | ((gs_u64)(rrh) << 32))

/* GIFtag (low 64 bits; REGS go in the high 64 bits) */
#define GIF_FLG_PACKED 0
#define GIF_FLG_REGLIST 1
#define GIF_FLG_IMAGE 2
#define GIFTAG_LO(nloop, eop, pre, prim, flg, nreg)                                                     \
    ((gs_u64)(nloop) | ((gs_u64)(eop) << 15) | ((gs_u64)(pre) << 46) | ((gs_u64)(prim) << 47) |       \
     ((gs_u64)(flg) << 58) | ((gs_u64)(nreg) << 60))

/* PACKED-mode register formats (each register occupies one qword = lo,hi). */
static inline void gs_packed_uv(uint64_t *q, uint32_t u, uint32_t v)
{
    q[0] = (uint64_t)u | ((uint64_t)v << 32);
    q[1] = 0;
}

static inline void gs_packed_xyz2(uint64_t *q, uint32_t x, uint32_t y, uint32_t z)
{
    q[0] = (uint64_t)x | ((uint64_t)y << 32);
    q[1] = (uint64_t)z;
}

static inline void gs_packed_xyzf2(uint64_t *q, uint32_t x, uint32_t y, uint32_t z24, uint32_t f)
{
    q[0] = (uint64_t)x | ((uint64_t)y << 32);
    q[1] = ((uint64_t)(z24 & 0xFFFFFF) << 4) | ((uint64_t)(f & 0xFF) << 36);
}

static inline void gs_packed_rgba(uint64_t *q, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    q[0] = (uint64_t)r | ((uint64_t)g << 32);
    q[1] = (uint64_t)b | ((uint64_t)a << 32);
}

static inline void gs_packed_stq(uint64_t *q, float s, float t, float qv)
{
    union { float f; uint32_t u; } S, T, Q;

    S.f = s;
    T.f = t;
    Q.f = qv;
    q[0] = (uint64_t)S.u | ((uint64_t)T.u << 32);
    q[1] = (uint64_t)Q.u;
}

/* DMA source-chain tags */
#define DMATAG_REFE 0
#define DMATAG_CNT  1
#define DMATAG_NEXT 2
#define DMATAG_REF  3
#define DMATAG_END  7
#define DMATAG(qwc, id, addr) \
    ((gs_u64)(qwc) | ((gs_u64)(id) << 28) | ((gs_u64)((uint32_t)(uintptr_t)(addr) & 0x0FFFFFF0u) << 32))

#endif /* PS2_GSREGS_H */

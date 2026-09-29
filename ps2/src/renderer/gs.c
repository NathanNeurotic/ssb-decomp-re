/*
 * GS core: video mode, VRAM layout, GIF DMA packet chain, display flips,
 * debug text.
 *
 * Video mode: NTSC 240p (non-interlaced), 320x240 - the exact resolution and
 * cadence of the N64 original, so the game's 60 Hz VI-driven timing maps 1:1
 * onto VBlank. Framebuffers are 16-bit like the N64's; three color buffers
 * mirror the game's gSYFramebufferSets[3], plus one 16-bit Z buffer.
 *
 *   VRAM (256-byte blocks)        size
 *   0     .. 639    FB0          160 KiB   (20 pages)
 *   640   .. 1279   FB1          160 KiB
 *   1280  .. 1919   FB2          160 KiB
 *   1920  .. 2559   Z (Z16S)     160 KiB
 *   2560  .. 2591   debug font     8 KiB   (T4 128x64 + CLUT)
 *   2592  .. 16383  texture/CLUT pool  3448 KiB
 */
#include "render.h"

#include <ps2/platform.h>

#include <debug.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>

/* GS privileged registers used directly (display circuit, CSR). */
#define PGS_PMODE   ((volatile uint64_t *)0x12000000)
#define PGS_DISPFB1 ((volatile uint64_t *)0x12000070)
#define PGS_DISPFB2 ((volatile uint64_t *)0x12000090)
#define PGS_BGCOLOR ((volatile uint64_t *)0x120000E0)
#define PGS_CSR     ((volatile uint64_t *)0x12001000)
#define PGS_DISPFB_VAL(fbp, fbw, psm)     ((uint64_t)(fbp) | ((uint64_t)(fbw) << 9) | ((uint64_t)(psm) << 15))
/* EN1, EN2, CRTMD=1, MMOD, AMOD, SLBG, ALP */
#define PGS_PMODE_VAL(en1, en2, mmod, alp)     ((uint64_t)(en1) | ((uint64_t)(en2) << 1) | (1ull << 2) | ((uint64_t)(mmod) << 5) | ((uint64_t)(alp) << 8))
#define PGS_CSR_FINISH 2ull
#include <stdio.h>
#include <string.h>

PS2Packet gPS2Pkt;
PS2RenderStats gPS2RenderStats;
PS2RenderStats gPS2RenderStatsLast;

/* This port only needs gsKit for low-level screen setup.  Keep its state
 * entirely static so retail hardware never depends on newlib heap state,
 * OSD config RPCs, or ROM-region probing during GS bring-up. */
#define PS2_GSKIT_BOOT_OS_QUEUE_BYTES  (16 * 1024)
#define PS2_GSKIT_BOOT_PER_QUEUE_BYTES (4 * 1024)

static GSGLOBAL sGsGlobalStorage;
static GSBGCOLOR sGsBgColor;
static GSTEST sGsTest;
static GSCLAMP sGsClamp;
static GSQUEUE sGsOsQueue;
static GSQUEUE sGsPerQueue;
static uint8_t sGsOsPool0[PS2_GSKIT_BOOT_OS_QUEUE_BYTES] __attribute__((aligned(64)));
static uint8_t sGsOsPool1[PS2_GSKIT_BOOT_OS_QUEUE_BYTES] __attribute__((aligned(64)));
static uint8_t sGsPerPool[PS2_GSKIT_BOOT_PER_QUEUE_BYTES] __attribute__((aligned(64)));
static uint8_t sGsDmaMisc[512] __attribute__((aligned(64)));
static GSGLOBAL *sGsGlobal;

/* Two packet buffers: one is filled while the other may still be in DMA. */
#define PKT_BUF_QWORDS (48 * 1024) /* 768 KiB each */
static uint64_t sPktBuf[2][PKT_BUF_QWORDS * 2] __attribute__((aligned(64)));
static int sPktCur;
static volatile int sPktInFlight; /* a buffer was kicked and not yet waited on */

/* The game's framebuffers (RDRAM on N64) - defined by the linker glue
 * (ps2/src/platform/arena.S) so every scene's arena-size arithmetic holds. */
extern uint16_t gSYFramebufferSets[PS2_FB_COUNT][PS2_SCREEN_W * PS2_SCREEN_H];
extern uint16_t gSYZBuffer[PS2_SCREEN_W * PS2_SCREEN_H] __attribute__((weak));

static volatile int sDisplayedFb = -1;
static volatile int sPrevDisplayedFb = -1;       /* on screen until the last switch */
static volatile uint32_t sDisplaySwitchVblank;   /* VBlank count at that switch */
static volatile int sPendingFb = -1;             /* requested by osViSwapBuffer */

/* Scan-out race counters (logged with the periodic gfx stats). */
uint32_t gPS2FbDrawDisplayed; /* frames drawn into the buffer on screen */
uint32_t gPS2FbDrawPending;   /* ... into the buffer queued for display */
uint32_t gPS2FbRetireWaits;   /* VBlanks waited for a just-retired buffer */
static volatile int sBlackout = 1;

/* ------------------------------------------------------------------ */
/* 8x8 font (public domain font8x8_basic, bit 0 = leftmost pixel)       */
/* ------------------------------------------------------------------ */

static const uint8_t sFont8x8[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    {0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},
    {0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00}, {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

/* Font as a PSMT4 128x64 image (16 x 6 glyph grid) and its 16-entry CLUT. */
static uint8_t sFontT4[128 * 64 / 2] __attribute__((aligned(64)));
static uint32_t sFontClut[16] __attribute__((aligned(64)));

/* ------------------------------------------------------------------ */
/* Packet chain                                                         */
/* ------------------------------------------------------------------ */

static void pkt_open(int buf)
{
    gPS2Pkt.base = sPktBuf[buf];
    gPS2Pkt.end = sPktBuf[buf] + PKT_BUF_QWORDS * 2 - 8; /* room for closing tags */
    gPS2Pkt.cnt = gPS2Pkt.base;
    gPS2Pkt.cnt[0] = DMATAG(0, DMATAG_CNT, 0);
    gPS2Pkt.cnt[1] = 0;
    gPS2Pkt.ptr = gPS2Pkt.base + 2;
}

static void pkt_close_cnt(void)
{
    uint32_t qwc = (uint32_t)((gPS2Pkt.ptr - gPS2Pkt.cnt) / 2) - 1;

    gPS2Pkt.cnt[0] = DMATAG(qwc, DMATAG_CNT, 0);
}

static void dma_wait_gif(void)
{
    if (sPktInFlight)
    {
        dmaKit_wait(DMA_CHANNEL_GIF, 0);
        sPktInFlight = 0;
    }
}

/* Terminate the current buffer's chain and start its DMA. */
static void pkt_kick(void)
{
    uint64_t *end_tag;
    uint32_t bytes;

    if (gPS2Pkt.ptr == gPS2Pkt.cnt + 2)
    {
        /* Nothing since the last tag: turn that tag into END. */
        gPS2Pkt.cnt[0] = DMATAG(0, DMATAG_END, 0);
        end_tag = gPS2Pkt.cnt;
        if (end_tag == gPS2Pkt.base)
        {
            return; /* empty buffer */
        }
    }
    else
    {
        pkt_close_cnt();
        end_tag = gPS2Pkt.ptr;
        end_tag[0] = DMATAG(0, DMATAG_END, 0);
        end_tag[1] = 0;
        gPS2Pkt.ptr += 2;
    }

    bytes = (uint32_t)((uint8_t *)gPS2Pkt.ptr - (uint8_t *)gPS2Pkt.base);
    gPS2RenderStats.packet_bytes += bytes;
    gPS2RenderStats.kicks++;

    dma_wait_gif();
    FlushCache(0); /* write back the packet (and any texel data it references) */
    dmaKit_send_chain(DMA_CHANNEL_GIF, gPS2Pkt.base, bytes / 16);
    sPktInFlight = 1;

    sPktCur ^= 1;
    pkt_open(sPktCur);
}

void ps2_pkt_reserve(uint32_t qwords)
{
    if (gPS2Pkt.ptr + qwords * 2 >= gPS2Pkt.end)
    {
        if (qwords * 2 + 8 >= PKT_BUF_QWORDS * 2)
        {
            ps2_panic("GIF packet request too large (%u qwords)", (unsigned)qwords);
        }
        pkt_kick();
    }
}

void ps2_pkt_ref(const void *data, uint32_t qwc)
{
    const uint8_t *p = (const uint8_t *)data;

    while (qwc > 0)
    {
        uint32_t n = (qwc > 0xFFFF) ? 0xFFFF : qwc;

        ps2_pkt_reserve(4);
        pkt_close_cnt();
        /* REF tag, then a fresh CNT tag for whatever follows. */
        gPS2Pkt.ptr[0] = DMATAG(n, DMATAG_REF, p);
        gPS2Pkt.ptr[1] = 0;
        gPS2Pkt.cnt = gPS2Pkt.ptr + 2;
        gPS2Pkt.cnt[0] = DMATAG(0, DMATAG_CNT, 0);
        gPS2Pkt.cnt[1] = 0;
        gPS2Pkt.ptr = gPS2Pkt.cnt + 2;
        p += (uint32_t)n * 16;
        qwc -= n;
    }
}

void ps2_pkt_flush(void)
{
    pkt_kick();
}

void ps2_pkt_finish(void)
{
    /* FINISH makes the GS raise its FINISH flag once everything before it
     * has been drawn; we wait on the DMA and then on that flag. */
    ps2_pkt_ad_begin(1);
    ps2_pkt_ad(GSR_FINISH, 0);
    *PGS_CSR = PGS_CSR_FINISH; /* clear FINISH */
    pkt_kick();
    dma_wait_gif();
    while (!(*PGS_CSR & PGS_CSR_FINISH))
    {
    }
}

/* ------------------------------------------------------------------ */
/* Framebuffers and display                                             */
/* ------------------------------------------------------------------ */

int ps2_gs_fb_index_for(const void *n64_fb)
{
    uintptr_t a = (uintptr_t)n64_fb & 0x0FFFFFFF;
    int i;

    for (i = 0; i < PS2_FB_COUNT; i++)
    {
        uintptr_t fb = (uintptr_t)gSYFramebufferSets[i] & 0x0FFFFFFF;

        if (a >= fb && a < fb + sizeof(gSYFramebufferSets[i]))
        {
            return i;
        }
    }
    return -1;
}

int ps2_gs_is_zbuffer(const void *n64_addr)
{
    uintptr_t a = (uintptr_t)n64_addr & 0x0FFFFFFF;
    uintptr_t z = (uintptr_t)gSYZBuffer & 0x0FFFFFFF;

    if (gSYZBuffer == NULL)
    {
        return 0;
    }
    /* The game points its Z image up to 64 KiB before gSYZBuffer
     * (SYVIDEO_ZBUFFER_START subtracts the letterbox border). */
    return (a + 0x10000 >= z && a < z + sizeof(gSYZBuffer));
}

static void display_fb(int index)
{
    uint64_t dispfb;

    if (index < 0 || index >= PS2_FB_COUNT)
    {
        return;
    }
    dispfb = PGS_DISPFB_VAL(PS2_FB_PAGE(index), PS2_FBW, PS2_FB_PSM);
    *PGS_DISPFB2 = dispfb;
    if (index != sDisplayedFb)
    {
        sPrevDisplayedFb = sDisplayedFb;
        sDisplaySwitchVblank = ps2_vblank_count();
    }
    sDisplayedFb = index;
    if (sPendingFb == index)
    {
        sPendingFb = -1;
    }
}

static void apply_blackout(void)
{
    /* gsKit programs DISPLAY2 for the mode, so read circuit 2 only; with it
     * disabled the GS outputs BGCOLOR (black). */
    *PGS_PMODE = PGS_PMODE_VAL(0, sBlackout ? 0 : 1, 1, 0x80);
}

/* Thread context, from osViSwapBuffer(): program the new display address
 * right away instead of waiting for the VBlank handler.  The GS does not
 * switch DISPFB mid-picture: it latches it around vertical sync.  Written
 * only from the VBlank-start handler, the write sometimes lands after that
 * latch (interrupt latency), so the old buffer stays on screen one more
 * field while the game - told it was swapped - already clears and redraws
 * it: the scanline being output at that moment shows the cleared colour,
 * a thin black line that moves with the scene (PCSX2 applies the register
 * at once and never shows it).  Written here, during the active picture,
 * the address is latched at the next vertical sync, the same VBlank where
 * ps2_vi_vblank_isr() then reports the swap to the game. */
void ps2_gs_queue_display_framebuffer(void *n64_fb)
{
    int index = ps2_gs_fb_index_for(n64_fb);

    if (index >= 0 && index < PS2_FB_COUNT)
    {
        *PGS_DISPFB2 = PGS_DISPFB_VAL(PS2_FB_PAGE(index), PS2_FBW, PS2_FB_PSM);
        sPendingFb = index;
    }
}

/* Render thread, before the first draw into framebuffer `fb` in a frame.
 * Counts draws into a buffer that is on screen or queued for it (the game's
 * VI bookkeeping should never allow either), and waits - at most two
 * VBlanks - before redrawing a buffer that left the screen less than two
 * VBlanks ago, in case the TV is still being fed from it. */
void ps2_gs_before_draw_into(int fb)
{
    extern void ps2_delay_vblanks(int n);
    int n;

    if (fb < 0)
    {
        return;
    }
    if (fb == sDisplayedFb)
    {
        gPS2FbDrawDisplayed++;
    }
    if (fb == sPendingFb)
    {
        gPS2FbDrawPending++;
    }
    for (n = 0; n < 2 && fb == sPrevDisplayedFb && fb != sDisplayedFb &&
                ps2_vblank_count() - sDisplaySwitchVblank < 2;
         n++)
    {
        ps2_delay_vblanks(1);
        gPS2FbRetireWaits++;
    }
}

/* Interrupt context (VBlank): latch a new display framebuffer. */
void ps2_gs_isr_display_framebuffer(void *n64_fb)
{
    display_fb(ps2_gs_fb_index_for(n64_fb));
    ps2_overlay_frame_presented();
}

void ps2_gs_isr_set_blackout(int black)
{
    sBlackout = black ? 1 : 0;
    apply_blackout();
}

void ps2_gs_present_now(int fb_index)
{
    display_fb(fb_index);
    sBlackout = 0;
    apply_blackout();
}

void ps2_gs_frame_setup(int fb_index)
{
    if (fb_index < 0)
    {
        fb_index = 0;
    }
    ps2_pkt_ad_begin(12);
    /* Neither this renderer nor gsKit's init otherwise writes these two, and
     * a real GS keeps whatever the launcher left in them (PCSX2 starts them
     * at 0): PABE=1 turns alpha blending off for pixels with source alpha
     * below 0x80, SCANMSK=2/3 stops every even/odd line from being drawn. */
    ps2_pkt_ad(GSR_PABE, 0);
    ps2_pkt_ad(GSR_SCANMSK, 0);
    ps2_pkt_ad(GSR_FRAME_1, GSV_FRAME(PS2_FB_PAGE(fb_index), PS2_FBW, PS2_FB_PSM, 0));
    ps2_pkt_ad(GSR_ZBUF_1, GSV_ZBUF(PS2_Z_PAGE, PS2_Z_PSM, 0));
    /* Primitive coordinates are emitted relative to a 2048,2048 origin so the
     * GS guard band covers geometry well outside the 320x240 viewport. */
    ps2_pkt_ad(GSR_XYOFFSET_1, GSV_XYOFFSET(2048 << 4, 2048 << 4));
    ps2_pkt_ad(GSR_SCISSOR_1, GSV_SCISSOR(0, PS2_SCREEN_W - 1, 0, PS2_SCREEN_H - 1));
    ps2_pkt_ad(GSR_PRMODECONT, 1);
    ps2_pkt_ad(GSR_COLCLAMP, 1);
    ps2_pkt_ad(GSR_DTHE, 1);  /* dither to 16 bit like the N64 */
    ps2_pkt_ad(GSR_TEXA, GSV_TEXA(0x00, 0, 0x80)); /* 1-bit alpha -> 0 / 1.0 */
    ps2_pkt_ad(GSR_TEST_1, GSV_TEST(0, 0, 0, 0, 0, 0, 1, GSZTST_ALWAYS));
    ps2_pkt_ad(GSR_FBA_1, 0);
    gPS2RenderStats.state_writes += 12;
}

void ps2_gs_clear(int fb_index, uint32_t rgba, int clear_z)
{
    uint64_t *p;

    ps2_gs_frame_setup(fb_index);
    ps2_pkt_ad_begin(4);
    ps2_pkt_ad(GSR_TEST_1, GSV_TEST(0, 0, 0, 0, 0, 0, 1, GSZTST_ALWAYS));
    ps2_pkt_ad(GSR_ZBUF_1, GSV_ZBUF(PS2_Z_PAGE, PS2_Z_PSM, clear_z ? 0 : 1));
    ps2_pkt_ad(GSR_PRIM, GSV_PRIM(GSPRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
    ps2_pkt_ad(GSR_RGBAQ, GSV_RGBAQ(rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, 0x80, 0x3F800000));

    ps2_pkt_ad_begin(2);
    p = gPS2Pkt.ptr;
    p[0] = GSV_XYZ((2048 << 4), (2048 << 4), 0);
    p[1] = GSR_XYZ2;
    p[2] = GSV_XYZ(((2048 + PS2_SCREEN_W) << 4), ((2048 + PS2_SCREEN_H) << 4), 0);
    p[3] = GSR_XYZ2;
    gPS2Pkt.ptr = p + 4;
}

/* ------------------------------------------------------------------ */
/* Debug text                                                           */
/* ------------------------------------------------------------------ */

static void build_font(void)
{
    int c, row, col;

    memset(sFontT4, 0, sizeof(sFontT4));
    for (c = 0; c < 95; c++)
    {
        int gx = (c % 16) * 8;
        int gy = (c / 16) * 8;

        for (row = 0; row < 8; row++)
        {
            for (col = 0; col < 8; col++)
            {
                if (sFont8x8[c][row] & (1 << col))
                {
                    int x = gx + col;
                    int y = gy + row;
                    int idx = y * 128 + x;

                    /* PSMT4 upload order: low nibble = even pixel */
                    sFontT4[idx >> 1] |= (idx & 1) ? 0x10 : 0x01;
                }
            }
        }
    }
    memset(sFontClut, 0, sizeof(sFontClut));
    sFontClut[1] = 0x80FFFFFF; /* white, alpha 1.0 */
}

static void upload_image(uint32_t dbp, uint32_t dbw, uint32_t psm, int w, int h, const void *data, uint32_t bytes)
{
    uint32_t qwc = (bytes + 15) / 16;

    ps2_pkt_ad_begin(4);
    ps2_pkt_ad(GSR_BITBLTBUF, GSV_BITBLTBUF(0, 0, 0, dbp, dbw, psm));
    ps2_pkt_ad(GSR_TRXPOS, GSV_TRXPOS(0, 0, 0, 0, 0));
    ps2_pkt_ad(GSR_TRXREG, GSV_TRXREG(w, h));
    ps2_pkt_ad(GSR_TRXDIR, 0);
    ps2_pkt_reserve(1);
    gPS2Pkt.ptr[0] = GIFTAG_LO(qwc, 0, 0, 0, GIF_FLG_IMAGE, 0);
    gPS2Pkt.ptr[1] = 0;
    gPS2Pkt.ptr += 2;
    ps2_pkt_ref(data, qwc);
}

static void upload_font(void)
{
    upload_image(PS2_FONT_BLOCK, 2, GSPSM_T4, 128, 64, sFontT4, sizeof(sFontT4));
    upload_image(PS2_FONT_CLUT_BLOCK, 1, GSPSM_CT32, 8, 2, sFontClut, sizeof(sFontClut));
    ps2_pkt_ad_begin(1);
    ps2_pkt_ad(GSR_TEXFLUSH, 0);
}

void ps2_gs_text(int x, int y, uint32_t rgba, const char *s)
{
    int n = (int)strlen(s);
    int i;
    uint64_t *p;

    if (n == 0)
    {
        return;
    }
    ps2_pkt_ad_begin(6);
    ps2_pkt_ad(GSR_TEX0_1, GSV_TEX0(PS2_FONT_BLOCK, 2, GSPSM_T4, 7, 6, 1, GSTFX_MODULATE,
                                   PS2_FONT_CLUT_BLOCK, GSPSM_CT32, 0, 0, 1));
    ps2_pkt_ad(GSR_TEX1_1, GSV_TEX1(0, 0, 0, 0, 0, 0, 0));
    ps2_pkt_ad(GSR_CLAMP_1, GSV_CLAMP(GSWRAP_CLAMP, GSWRAP_CLAMP, 0, 0, 0, 0));
    ps2_pkt_ad(GSR_ALPHA_1, GSV_ALPHA(GSBL_CS, GSBL_CD, GSBL_AS, GSBL_CD, 0));
    ps2_pkt_ad(GSR_TEST_1, GSV_TEST(1, GSATST_NOTEQUAL, 0, GSAFAIL_KEEP, 0, 0, 1, GSZTST_ALWAYS));
    ps2_pkt_ad(GSR_RGBAQ, GSV_RGBAQ(rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, 0x80, 0x3F800000));

    /* One SPRITE per glyph: UV + XYZ2 pairs in packed mode. */
    ps2_pkt_reserve(1 + n * 4);
    p = gPS2Pkt.ptr;
    p[0] = GIFTAG_LO(n * 2, 0, 1, GSV_PRIM(GSPRIM_SPRITE, 0, 1, 0, 1, 0, 1, 0, 0), GIF_FLG_PACKED, 2);
    p[1] = (uint64_t)GSR_UV | ((uint64_t)GSR_XYZ2 << 4);
    p += 2;
    for (i = 0; i < n; i++)
    {
        int c = (unsigned char)s[i];
        int u, v, sx, sy;

        if (c < 32 || c > 126)
        {
            c = '?';
        }
        c -= 32;
        u = (c % 16) * 8;
        v = (c / 16) * 8;
        sx = (2048 + x + i * 8) << 4;
        sy = (2048 + y) << 4;
        gs_packed_uv(p + 0, (u << 4) + 8, (v << 4) + 8);
        gs_packed_xyz2(p + 2, sx, sy, 0);
        gs_packed_uv(p + 4, ((u + 8) << 4) + 8, ((v + 8) << 4) + 8);
        gs_packed_xyz2(p + 6, sx + (8 << 4), sy + (8 << 4), 0);
        p += 8;
    }
    gPS2Pkt.ptr = p;
    gPS2RenderStats.batches++;
}

void ps2_gs_rect(int x0, int y0, int x1, int y1, uint32_t rgba)
{
    uint64_t *p;

    ps2_pkt_ad_begin(3);
    ps2_pkt_ad(GSR_ALPHA_1, GSV_ALPHA(GSBL_CS, GSBL_CD, GSBL_AS, GSBL_CD, 0));
    ps2_pkt_ad(GSR_TEST_1, GSV_TEST(0, 0, 0, 0, 0, 0, 1, GSZTST_ALWAYS));
    ps2_pkt_ad(GSR_RGBAQ, GSV_RGBAQ(rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF,
                                   ((rgba >> 24) + 1) >> 1, 0x3F800000));
    ps2_pkt_reserve(3);
    p = gPS2Pkt.ptr;
    p[0] = GIFTAG_LO(2, 0, 1, GSV_PRIM(GSPRIM_SPRITE, 0, 0, 0, 1, 0, 0, 0, 0), GIF_FLG_PACKED, 1);
    p[1] = GSR_XYZ2;
    gs_packed_xyz2(p + 2, (2048 + x0) << 4, (2048 + y0) << 4, 0);
    gs_packed_xyz2(p + 4, (2048 + x1) << 4, (2048 + y1) << 4, 0);
    gPS2Pkt.ptr = p + 6;
}

/* ------------------------------------------------------------------ */
/* Boot / panic screens                                                 */
/* ------------------------------------------------------------------ */

static int sGsReady;
static const char *sBootTitle;

void ps2_boot_stage(const char *name, uint32_t rgb)
{
    if (!sGsReady)
    {
        /* Hardware diagnostics must not depend on printf/RPC.  Show the
         * marker first; if logging itself wedges after an IOP reboot the TV
         * must still identify the stage we actually entered. */
        *PGS_PMODE = PGS_PMODE_VAL(0, 0, 1, 0);
        *PGS_BGCOLOR = ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((uint64_t)(rgb & 0xFF) << 16);
        __asm__ volatile("sync.p" ::: "memory");
    }
    ps2_log("boot: %s", name);
    if (sGsReady && sBootTitle != NULL)
    {
        /* Past GS init the boot log is on screen: redraw it so a stage that
         * hangs is the last line the TV shows. */
        ps2_gs_boot_screen(sBootTitle);
    }
}

void ps2_gs_boot_screen(const char *title)
{
    int i, n, first, y;
    int fb = 0;

    if (!sGsReady)
    {
        return;
    }
    sBootTitle = title;
    ps2_gs_clear(fb, 0x302010, 0);
    ps2_gs_text(8, 8, 0x40C0FF, title);
    n = ps2_log_line_count();
    first = (n > 26) ? n - 26 : 0;
    for (i = first, y = 24; i < n; i++, y += 8)
    {
        ps2_gs_text(8, y, 0xE0E0E0, ps2_log_line(i));
    }
    ps2_pkt_finish();
    ps2_gs_present_now(fb);
}

void ps2_gs_show_panic(const char *msg)
{
    char line[41];
    int y = 40;
    size_t off = 0, len = strlen(msg);

    if (!sGsReady)
    {
        /* before our GS setup: use ps2sdk's self-contained text screen */
        init_scr();
        scr_setbgcolor(0x600000);
        scr_clear();
        scr_printf("\n  SSB64 PS2 - FATAL ERROR\n\n  %s\n", msg);
        return;
    }
    sPktInFlight = 0; /* the pipeline state is unknown; start over */
    pkt_open(sPktCur);
    ps2_gs_clear(0, 0x000060, 0);
    ps2_gs_text(8, 16, 0x4040FF, "SSB64 PS2 - FATAL ERROR");
    while (off < len && y < 230)
    {
        size_t n = len - off;

        if (n > 38)
        {
            n = 38;
        }
        memcpy(line, msg + off, n);
        line[n] = '\0';
        ps2_gs_text(8, y, 0xFFFFFF, line);
        off += n;
        y += 10;
    }
    ps2_pkt_finish();
    ps2_gs_present_now(0);
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

static void *gs_ucab_alias(void *p)
{
    return (void *)((uintptr_t)p | 0x30000000u);
}

static void ps2_gskit_queue_init_static(GSQUEUE *queue, void *pool0, void *pool1, int size, int mode)
{
    memset(queue, 0, sizeof(*queue));
    queue->pool[0] = gs_ucab_alias(pool0);
    queue->pool_max[0] = (void *)((uintptr_t)queue->pool[0] + (uintptr_t)size);
    if (mode == GS_ONESHOT)
    {
        queue->pool[1] = gs_ucab_alias(pool1);
        queue->pool_max[1] = (void *)((uintptr_t)queue->pool[1] + (uintptr_t)size);
    }
    queue->dma_tag = queue->pool[0];
    queue->pool_cur = (void *)((uintptr_t)queue->pool[0] + 16u);
    queue->last_tag = queue->pool_cur;
    queue->last_type = GIF_RESERVED;
    queue->mode = (u8)mode;
    queue->dbuf = 0;
}

static GSGLOBAL *ps2_gskit_init_static(void)
{
    static const s8 dither_matrix[16] = {4, 2, 5, 3, 0, 6, 1, 7, 5, 3, 4, 2, 1, 7, 0, 6};
    GSGLOBAL *g = &sGsGlobalStorage;

    memset(g, 0, sizeof(*g));
    memset(&sGsBgColor, 0, sizeof(sGsBgColor));
    memset(&sGsTest, 0, sizeof(sGsTest));
    memset(&sGsClamp, 0, sizeof(sGsClamp));

    ps2_gskit_queue_init_static(&sGsOsQueue, sGsOsPool0, sGsOsPool1,
                                 PS2_GSKIT_BOOT_OS_QUEUE_BYTES, GS_ONESHOT);
    ps2_gskit_queue_init_static(&sGsPerQueue, sGsPerPool, NULL,
                                 PS2_GSKIT_BOOT_PER_QUEUE_BYTES, GS_PERSISTENT);

    g->BGColor = &sGsBgColor;
    g->Test = &sGsTest;
    g->Clamp = &sGsClamp;
    g->Os_Queue = &sGsOsQueue;
    g->Per_Queue = &sGsPerQueue;
    g->CurQueue = &sGsOsQueue;
    g->Os_AllocSize = PS2_GSKIT_BOOT_OS_QUEUE_BYTES;
    g->Per_AllocSize = PS2_GSKIT_BOOT_PER_QUEUE_BYTES;
    g->dma_misc = gs_ucab_alias(sGsDmaMisc);

    g->Aspect = GS_ASPECT_4_3;
    g->PSM = GS_PSM_CT24;
    g->PSMZ = GS_PSMZ_32;
    g->Dithering = GS_SETTING_OFF;
    g->DoubleBuffering = GS_SETTING_ON;
    g->ZBuffering = GS_SETTING_ON;
    g->Mode = GS_MODE_NTSC;
    g->Interlace = GS_INTERLACED;
    g->Field = GS_FIELD;
    g->Width = 640;
    g->Height = 448;
    g->CurrentPointer = 0;
    g->DrawOrder = GS_PER_OS;
    g->EvenOrOdd = 0;
    g->OffsetX = 2048 << 4;
    g->OffsetY = 2048 << 4;
    g->ActiveBuffer = 1;
    g->LockBuffer = GS_SETTING_OFF;
    g->PrimFogEnable = GS_SETTING_OFF;
    g->PrimAAEnable = GS_SETTING_OFF;
    g->PrimAlphaEnable = GS_SETTING_OFF;
    g->PrimAlpha = GS_BLEND_BACK2FRONT;
    g->PrimContext = 0;
    g->FirstFrame = GS_SETTING_ON;
    memcpy(g->DitherMatrix, dither_matrix, sizeof(dither_matrix));

    sGsBgColor.Red = 0;
    sGsBgColor.Green = 0;
    sGsBgColor.Blue = 0;

    sGsTest.ATE = GS_SETTING_OFF;
    sGsTest.ATST = GS_SETTING_ON;
    sGsTest.AREF = 0x80;
    sGsTest.AFAIL = 0;
    sGsTest.DATE = GS_SETTING_OFF;
    sGsTest.DATM = 0;
    sGsTest.ZTE = GS_SETTING_ON;
    sGsTest.ZTST = 2;

    sGsClamp.WMS = GS_CMODE_CLAMP;
    sGsClamp.WMT = GS_CMODE_CLAMP;

    return g;
}

void ps2_gs_init(void)
{
    /* Avoid gsKit_init_global[_custom]() entirely on hardware.  Besides
     * heap allocations it queries OSD/ROM configuration that this port does
     * not need because the video mode is selected explicitly below. */
    ps2_boot_stage("GS: static state begin", 0xFFFFFF); /* white */
    sGsGlobal = ps2_gskit_init_static();
    ps2_boot_stage("GS: static state ready", 0x00FF00); /* bright green */

    sGsGlobal->Mode = GS_MODE_NTSC;
    if (ps2_video_progressive())
    {
        /* 240p: sharpest on CRTs, but many TVs / HDMI adapters show no
         * picture for it. */
        sGsGlobal->Interlace = GS_NONINTERLACED;
        sGsGlobal->Field = GS_FRAME;
    }
    else
    {
        /* 480i field mode: every field scans the same 240-line buffer, so
         * the picture matches 240p but is a standard TV signal. */
        sGsGlobal->Interlace = GS_INTERLACED;
        sGsGlobal->Field = GS_FIELD;
    }
    sGsGlobal->Width = PS2_SCREEN_W;
    sGsGlobal->Height = PS2_SCREEN_H;
    sGsGlobal->PSM = PS2_FB_PSM;
    sGsGlobal->PSMZ = GS_PSMZ_16S;
    sGsGlobal->DoubleBuffering = GS_SETTING_OFF;
    sGsGlobal->ZBuffering = GS_SETTING_ON;
    sGsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    sGsGlobal->Dithering = GS_SETTING_ON;

    /* Do not call dmaKit_init() here on hardware.  By this point SIF RPC
     * has already been used extensively to reboot the IOP and load modules.
     * dmaKit_init() resets the global EE DMAC CTRL/PCR/SQWC/RBSR/RBOR
     * registers, which can destroy live SIF DMA state on a real console.
     * PCSX2 is much more forgiving of that reset.
     *
     * The DMAC is already enabled (SIF RPC could not have worked otherwise).
     * We only own the GIF channel, so reset that channel alone and preserve
     * every other channel's global DMAC state.  dmaKit_wait_fast() uses the
     * CPCOND mask in PCR, therefore OR the GIF bit into the existing mask
     * instead of replacing PCR wholesale. */
    ps2_boot_stage("GS: GIF channel init", 0xFF8000); /* bright orange */
    if (dmaKit_chan_init(DMA_CHANNEL_GIF) < 0)
    {
        ps2_boot_stage("GS: GIF channel failed", 0xFF0000); /* bright red */
        for (;;)
        {
            SleepThread();
        }
    }
    *DMA_REG_PCR |= (1u << DMA_CHANNEL_GIF);
    __asm__ volatile("sync.p" ::: "memory");
    ps2_boot_stage("GS: DMAC ready", 0xFFFF00); /* bright yellow */

    /* gsKit programs SMODE/SYNC/DISPLAY for the mode; from here on the
     * renderer owns VRAM layout, FRAME/ZBUF and the display circuit. */
    gsKit_init_screen(sGsGlobal);
    ps2_boot_stage("GS: screen ready", 0xFF00FF); /* bright magenta */

    ps2_mem_reclassify_static(PS2_MEM_GFX_STAGING, sizeof(sPktBuf));

    build_font();
    sPktCur = 0;
    pkt_open(0);
    upload_font();
    ps2_gs_clear(0, 0, 1);
    ps2_boot_stage("GS: first GIF finish", 0x8000FF); /* violet */
    ps2_pkt_finish();

    *PGS_BGCOLOR = 0;
    sBlackout = 1;
    display_fb(0);
    apply_blackout();
    sGsReady = 1;

    ps2_log("GS: NTSC %s %dx%d CT16S x%d + Z16S, tex pool %u KiB (MAGV %d DH %d)",
            ps2_video_progressive() ? "240p" : "480i", PS2_SCREEN_W, PS2_SCREEN_H, PS2_FB_COUNT,
            (unsigned)(PS2_TEX_POOL_BLOCKS / 4), sGsGlobal->MagV, sGsGlobal->DH);
}

/*
 * N64 SRAM -> PS2 memory card.
 *
 * Smash 64 keeps one LBBackupData record (plus a second copy) in the
 * cartridge's 32 KiB battery SRAM and validates it with its own checksum.
 * The port keeps a 32 KiB SRAM image in EE RAM: the game's PI reads/writes
 * hit that image immediately, exactly like SRAM. A low-priority thread
 * persists the image to the memory card once writes have settled.
 *
 * On-card format (mc0:/SSB64PS2/): two slots SRAM_A.BIN / SRAM_B.BIN, each
 *   PS2SaveHeader { "SSB64SAV", version, sequence, size, crc32 } + image.
 * Flushes alternate slots with an increasing sequence number, so a write
 * interrupted by power-off/card removal leaves the previous slot intact;
 * loading picks the valid slot with the highest sequence.
 */
#include <ps2/platform.h>

#include <kernel.h>
#include <libmc.h>
#include <string.h>

#define SRAM_SIZE (32 * 1024)
#define SAVE_DIR "/SSB64PS2"
#define SAVE_VERSION 1
#define FLUSH_DELAY_VBLANKS 60 /* write once the game has been quiet for ~1 s */

extern void ps2_delay_vblanks(int n);

typedef struct PS2SaveHeader
{
    char magic[8];
    uint32_t version;
    uint32_t sequence;
    uint32_t size;
    uint32_t crc;
    uint32_t reserved[2];
} PS2SaveHeader;

static uint8_t sSram[SRAM_SIZE] __attribute__((aligned(64)));
static uint8_t sWriteBuf[sizeof(PS2SaveHeader) + SRAM_SIZE] __attribute__((aligned(64)));
static volatile int sDirty;
static volatile uint32_t sDirtyVBlank;
static uint32_t sSequence;
static int sNextSlot;
static int sCardOk;
static int sLock = -1;
static int sThreadId = -1;
static uint8_t sThreadStack[16 * 1024] __attribute__((aligned(64)));

static uint32_t crc32_calc(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int k;

    for (i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static int mc_call(int r)
{
    int cmd = MC_FUNC_NONE;
    int result = -1;
    int i;

    if (r != 0)
        return -1;

    /*
     * Never let a damaged SIO2/MCSERV transaction freeze game boot forever.
     * libmc operations are asynchronous, so poll their completion for about
     * two seconds instead of using MC_WAIT's unbounded blocking path.
     */
    for (i = 0; i < 120; i++)
    {
        int sync = mcSync(MC_NOWAIT, &cmd, &result);

        if (sync > 0)
            return result;
        if (sync < 0)
            return -1;
        ps2_delay_vblanks(1);
    }

    ps2_log("save: MCSERV timeout on command %d; disabling this operation", cmd);
    return -1;
}

static int card_present(void)
{
    int type = 0, free_kb = 0, format = 0;
    int r = mc_call(mcGetInfo(0, 0, &type, &free_kb, &format));

    return (r >= -2) && (type == MC_TYPE_PS2) && format;
}

static const char *slot_name(int slot)
{
    return slot ? SAVE_DIR "/SRAM_B.BIN" : SAVE_DIR "/SRAM_A.BIN";
}

/* Returns sequence number of a valid slot image (copied into dst), or -1. */
static int64_t load_slot(int slot, uint8_t *dst)
{
    PS2SaveHeader *h = (PS2SaveHeader *)sWriteBuf;
    int fd = mc_call(mcOpen(0, 0, slot_name(slot), 1 /* O_RDONLY */));
    int n;

    if (fd < 0)
    {
        return -1;
    }
    n = mc_call(mcRead(fd, sWriteBuf, sizeof(sWriteBuf)));
    mc_call(mcClose(fd));

    if (n != (int)sizeof(sWriteBuf) || memcmp(h->magic, "SSB64SAV", 8) != 0 || h->version != SAVE_VERSION ||
        h->size != SRAM_SIZE || crc32_calc(sWriteBuf + sizeof(*h), SRAM_SIZE) != h->crc)
    {
        return -1;
    }
    memcpy(dst, sWriteBuf + sizeof(*h), SRAM_SIZE);
    return h->sequence;
}

static void flush_now(void)
{
    PS2SaveHeader *h = (PS2SaveHeader *)sWriteBuf;
    uint32_t snapshot_vblank;
    int fd, n;

    WaitSema(sLock);
    memcpy(sWriteBuf + sizeof(*h), sSram, SRAM_SIZE);
    snapshot_vblank = sDirtyVBlank;
    SignalSema(sLock);

    if (!sCardOk && !(sCardOk = card_present()))
    {
        return;
    }
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "SSB64SAV", 8);
    h->version = SAVE_VERSION;
    h->sequence = ++sSequence;
    h->size = SRAM_SIZE;
    h->crc = crc32_calc(sWriteBuf + sizeof(*h), SRAM_SIZE);

    mc_call(mcMkDir(0, 0, SAVE_DIR)); /* fails harmlessly if it exists */
    fd = mc_call(mcOpen(0, 0, slot_name(sNextSlot), 0x0200 | 0x0002 /* O_CREAT|O_WRONLY */));
    if (fd < 0)
    {
        ps2_log("save: cannot open %s (%d)", slot_name(sNextSlot), fd);
        sCardOk = 0;
        return;
    }
    n = mc_call(mcWrite(fd, sWriteBuf, sizeof(sWriteBuf)));
    mc_call(mcClose(fd));
    if (n != (int)sizeof(sWriteBuf))
    {
        ps2_log("save: write failed (%d)", n);
        sCardOk = 0;
        return;
    }
    ps2_log("save: wrote %s seq %u", slot_name(sNextSlot), (unsigned)sSequence);
    sNextSlot ^= 1;

    /* Do not lose a newer game write that arrived while MCSERV was writing. */
    WaitSema(sLock);
    if (sDirty && sDirtyVBlank == snapshot_vblank)
        sDirty = 0;
    SignalSema(sLock);
}

static void save_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        ps2_delay_vblanks(15);
        if (sDirty && (ps2_vblank_count() - sDirtyVBlank) >= FLUSH_DELAY_VBLANKS)
        {
            flush_now();
        }
    }
}

void ps2_save_init(void)
{
    ee_sema_t sema = { 0 };
    ee_thread_t th = { 0 };
    int64_t seq_a, seq_b;
    static uint8_t tmp[SRAM_SIZE] __attribute__((aligned(64)));
    extern void *_gp;

    sema.init_count = 1;
    sema.max_count = 1;
    sLock = CreateSema(&sema);
    memset(sSram, 0, sizeof(sSram));

    ps2_log("save: binding MCSERV RPC");
    if (mcInit(MC_TYPE_XMC) < 0)
    {
        sCardOk = 0;
        ps2_log("save: mcInit failed, saving disabled");
    }
    else if ((sCardOk = card_present()))
    {
        seq_a = load_slot(0, tmp);
        if (seq_a >= 0)
        {
            memcpy(sSram, tmp, SRAM_SIZE);
        }
        seq_b = load_slot(1, tmp);
        if (seq_b > seq_a)
        {
            memcpy(sSram, tmp, SRAM_SIZE);
        }
        sSequence = (uint32_t)((seq_a > seq_b) ? seq_a : (seq_b > 0 ? seq_b : 0));
        sNextSlot = (seq_a > seq_b) ? 1 : 0;
        ps2_log("save: memory card 1 ready (slot A %d, slot B %d)", (int)seq_a, (int)seq_b);
    }
    else
    {
        ps2_log("save: no formatted memory card in slot 1; saves stay in RAM");
    }
    ps2_mem_reclassify_static(PS2_MEM_SCRATCH, sizeof(sWriteBuf) + sizeof(tmp));

    th.func = (void *)save_thread;
    th.stack = sThreadStack;
    th.stack_size = sizeof(sThreadStack);
    th.gp_reg = &_gp;
    th.initial_priority = 110; /* below every game thread */
    sThreadId = CreateThread(&th);
    if (sThreadId >= 0)
        StartThread(sThreadId, NULL);
    else
        ps2_log("save: background flush thread unavailable; SRAM remains in RAM");
}

void ps2_sram_read(uint32_t offset, void *dst, uint32_t size)
{
    if (offset >= SRAM_SIZE)
    {
        memset(dst, 0, size);
        return;
    }
    if (offset + size > SRAM_SIZE)
    {
        size = SRAM_SIZE - offset;
    }
    WaitSema(sLock);
    memcpy(dst, sSram + offset, size);
    SignalSema(sLock);
}

void ps2_sram_write(uint32_t offset, const void *src, uint32_t size)
{
    if (offset >= SRAM_SIZE)
    {
        return;
    }
    if (offset + size > SRAM_SIZE)
    {
        size = SRAM_SIZE - offset;
    }
    WaitSema(sLock);
    memcpy(sSram + offset, src, size);
    sDirty = 1;
    sDirtyVBlank = ps2_vblank_count();
    SignalSema(sLock);
}

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
#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <sifrpc.h>
#include <string.h>

#define SRAM_SIZE (32 * 1024)
#define SAVE_DIR "mc0:SSB64PS2"
#define SAVE_VERSION 1
#define FLUSH_DELAY_VBLANKS 60 /* write once the game has been quiet for ~1 s */

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
static int sLock = -1;
static int sThreadId = -1;
static int sFioReady;
static uint32_t sNextRetryVBlank;
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

static int save_fio_init(void)
{
    SifRpcClientData_t probe __attribute__((aligned(64)));
    int i;

    if (sFioReady)
        return 1;

    /*
     * Keep memory-card traffic off fileXio, which owns the persistent DAT
     * stream. PS2SDK's fioInit() has an unbounded bind loop, so first prove
     * the ROM FILEIO service is present with a bounded probe.
     */
    memset(&probe, 0, sizeof(probe));
    for (i = 0; i < 500; i++)
    {
        int rc = sceSifBindRpc(&probe, 0x80000001u, 0);

        if (rc < 0)
            break;
        if (probe.server != NULL)
        {
            if (fioInit() >= 0)
            {
                sFioReady = 1;
                return 1;
            }
            break;
        }
        DelayThread(1000);
    }

    ps2_log("save: FILEIO RPC unavailable; SRAM stays in RAM");
    return 0;
}

static int read_full_fd(int fd, void *dst, int size)
{
    uint8_t *p = (uint8_t *)dst;
    int done = 0;

    while (done < size)
    {
        int n = fioRead(fd, p + done, size - done);

        if (n <= 0)
            return -1;
        done += n;
    }
    return done;
}

static int write_full_fd(int fd, const void *src, int size)
{
    const uint8_t *p = (const uint8_t *)src;
    int done = 0;

    while (done < size)
    {
        int n = fioWrite(fd, p + done, size - done);

        if (n <= 0)
            return -1;
        done += n;
    }
    return done;
}

static const char *slot_name(int slot)
{
    return slot ? SAVE_DIR "/SRAM_B.BIN" : SAVE_DIR "/SRAM_A.BIN";
}

/* Returns sequence number of a valid slot image (copied into dst), or -1. */
static int64_t load_slot(int slot, uint8_t *dst)
{
    PS2SaveHeader *h = (PS2SaveHeader *)sWriteBuf;
    int fd;
    int n;

    if (!save_fio_init())
        return -1;

    fd = fioOpen(slot_name(slot), FIO_O_RDONLY);
    if (fd < 0)
        return -1;

    n = read_full_fd(fd, sWriteBuf, sizeof(sWriteBuf));
    fioClose(fd);

    if (n != (int)sizeof(sWriteBuf) || memcmp(h->magic, "SSB64SAV", 8) != 0 ||
        h->version != SAVE_VERSION || h->size != SRAM_SIZE ||
        crc32_calc(sWriteBuf + sizeof(*h), SRAM_SIZE) != h->crc)
        return -1;

    memcpy(dst, sWriteBuf + sizeof(*h), SRAM_SIZE);
    return h->sequence;
}

static void flush_now(void)
{
    PS2SaveHeader *h = (PS2SaveHeader *)sWriteBuf;
    uint32_t snapshot_vblank;
    int fd;
    int n;

    /*
     * MCMAN itself registers an ioman/iomanX "mc" filesystem driver. Use it
     * through the same normal file API as every other backend instead of
     * binding libmc to MCSERV. The latter has unbounded synchronous RPC paths
     * and was able to freeze the entire boot after storage had already
     * succeeded on hardware.
     */
    WaitSema(sLock);
    memcpy(sWriteBuf + sizeof(*h), sSram, SRAM_SIZE);
    snapshot_vblank = sDirtyVBlank;
    SignalSema(sLock);

    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "SSB64SAV", 8);
    h->version = SAVE_VERSION;
    h->sequence = sSequence + 1;
    h->size = SRAM_SIZE;
    h->crc = crc32_calc(sWriteBuf + sizeof(*h), SRAM_SIZE);

    if (!save_fio_init())
    {
        sNextRetryVBlank = ps2_vblank_count() + 300;
        return;
    }

    /* fio/MCMAN is intentionally separate from the fileXio DAT stream. */
    fioMkdir(SAVE_DIR);

    fd = fioOpen(slot_name(sNextSlot), FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
    if (fd < 0)
    {
        sNextRetryVBlank = ps2_vblank_count() + 300; /* retry in ~5 s */
        ps2_log("save: mc0 unavailable; keeping SRAM dirty in RAM");
        return;
    }

    n = write_full_fd(fd, sWriteBuf, sizeof(sWriteBuf));
    fioClose(fd);
    if (n != (int)sizeof(sWriteBuf))
    {
        sNextRetryVBlank = ps2_vblank_count() + 300;
        ps2_log("save: write failed (%d); keeping SRAM dirty in RAM", n);
        return;
    }

    sSequence++;
    ps2_log("save: wrote %s seq %u", slot_name(sNextSlot), (unsigned)sSequence);
    sNextSlot ^= 1;
    sNextRetryVBlank = 0;

    /*
     * A game write may have arrived while the card write was in flight. Only
     * clear dirty if the snapshot we persisted is still the newest SRAM
     * generation; otherwise the save thread will flush the newer one later.
     */
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
        extern void ps2_delay_vblanks(int n);
        uint32_t now;

        ps2_delay_vblanks(15);
        now = ps2_vblank_count();
        if (sDirty &&
            (uint32_t)(now - sDirtyVBlank) >= FLUSH_DELAY_VBLANKS &&
            (sNextRetryVBlank == 0 || (int32_t)(now - sNextRetryVBlank) >= 0))
            flush_now();
    }
}

int ps2_save_init(void)
{
    ee_sema_t sema = { 0 };
    ee_thread_t th = { 0 };
    int64_t seq_a, seq_b;
    static uint8_t tmp[SRAM_SIZE] __attribute__((aligned(64)));
    extern void *_gp;

    sema.init_count = 1;
    sema.max_count = 1;
    sLock = CreateSema(&sema);
    if (sLock < 0)
    {
        ps2_log("save: SRAM lock creation failed (%d)", sLock);
        return 0;
    }

    memset(sSram, 0, sizeof(sSram));
    sDirty = 0;
    sFioReady = 0;
    sNextRetryVBlank = 0;

    /*
     * No libmc/MCSERV bootstrap here. MCMAN already exposes mc0: as a normal
     * filesystem through iomanX, so load the two alternating images directly.
     * Missing card, missing directory and first-run/no-save all degrade to an
     * empty SRAM image without blocking game boot.
     */
    ps2_log("save: checking mc0 SRAM slot A");
    seq_a = load_slot(0, tmp);
    if (seq_a >= 0)
        memcpy(sSram, tmp, SRAM_SIZE);

    ps2_log("save: checking mc0 SRAM slot B");
    seq_b = load_slot(1, tmp);
    if (seq_b > seq_a)
        memcpy(sSram, tmp, SRAM_SIZE);

    if (seq_a >= 0 || seq_b >= 0)
    {
        sSequence = (uint32_t)((seq_a > seq_b) ? seq_a : seq_b);
        sNextSlot = (seq_a > seq_b) ? 1 : 0;
        ps2_log("save: mc0 direct I/O ready (slot A %d, slot B %d)",
                (int)seq_a, (int)seq_b);
    }
    else
    {
        sSequence = 0;
        sNextSlot = 0;
        ps2_log("save: no valid mc0 save; starting with empty SRAM");
    }

    ps2_mem_reclassify_static(PS2_MEM_SCRATCH, sizeof(sWriteBuf) + sizeof(tmp));

    th.func = (void *)save_thread;
    th.stack = sThreadStack;
    th.stack_size = sizeof(sThreadStack);
    th.gp_reg = &_gp;
    th.initial_priority = 110; /* below every game thread */
    sThreadId = CreateThread(&th);
    if (sThreadId >= 0)
    {
        if (StartThread(sThreadId, NULL) < 0)
        {
            ps2_log("save: background flush thread failed to start; SRAM remains in RAM");
            DeleteThread(sThreadId);
            sThreadId = -1;
        }
    }
    else
    {
        ps2_log("save: background flush thread unavailable; SRAM remains in RAM");
    }

    return 1;
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

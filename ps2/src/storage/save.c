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
#include <sifrpc.h>
#include <delaythread.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define SRAM_SIZE (32 * 1024)
#define SAVE_DIR "/SSB64PS2"
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
static int sCardOk;
static int sFileBackend;
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

static int mc_rpc_ready(void)
{
    SifRpcClientData_t probe;
    int i;

    memset(&probe, 0, sizeof(probe));
    for (i = 0; i < 100; i++)
    {
        int rc = sceSifBindRpc(&probe, 0x80000400, 0); /* MCSERV/XMCSERV */

        if (rc < 0)
            return 0;
        if (probe.server != NULL)
            return 1;
        DelayThread(10000);
    }
    return 0;
}

static int mc_call(int r)
{
    int cmd = 0, result = -1;
    int i;

    if (r != 0)
        return -1;

    /*
     * Never use mcSync(0): ps2sdk waits forever if a card/SIO2 transaction
     * wedges. Poll the async RPC instead so a missing/bad card cannot stop
     * game boot. Two seconds is far longer than a normal memory-card command.
     */
    for (i = 0; i < 200; i++)
    {
        int done = mcSync(1, &cmd, &result);

        if (done == 1)
            return result;
        if (done < 0)
            return -1;
        DelayThread(10000);
    }

    ps2_log("save: memory-card RPC timed out (cmd=%d); disabling card saves", cmd);
    mcReset();
    sCardOk = 0;
    return -1;
}

static int card_present(void)
{
    int type = 0, free_kb = 0, format = 0;
    int r = mc_call(mcGetInfo(0, 0, &type, &free_kb, &format));

    return (r >= -2) && (type == MC_TYPE_PS2) && format;
}

static void file_slot_path(int slot, char *out, size_t out_size)
{
    ps2_storage_path(out, out_size, slot ? "SSB64_SAVE_B.BIN" : "SSB64_SAVE_A.BIN");
}

static int file_read_all(const char *path, void *dst, int size)
{
    uint8_t *p = (uint8_t *)dst;
    int fd = open(path, O_RDONLY);
    int done = 0;

    if (fd < 0)
        return -1;
    while (done < size)
    {
        int n = (int)read(fd, p + done, (size - done > 0x800) ? 0x800 : size - done);

        if (n <= 0)
            break;
        done += n;
        if (done < size)
            DelayThread(500);
    }
    close(fd);
    return done;
}

static int file_write_all(const char *path, const void *src, int size)
{
    const uint8_t *p = (const uint8_t *)src;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int done = 0;

    if (fd < 0)
        return -1;
    while (done < size)
    {
        int n = (int)write(fd, p + done, (size - done > 0x800) ? 0x800 : size - done);

        if (n <= 0)
            break;
        done += n;
        if (done < size)
            DelayThread(500);
    }
    close(fd);
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
    int n;

    if (sFileBackend)
    {
        char path[320];

        file_slot_path(slot, path, sizeof(path));
        n = file_read_all(path, sWriteBuf, sizeof(sWriteBuf));
    }
    else
    {
        int fd = mc_call(mcOpen(0, 0, slot_name(slot), 1 /* O_RDONLY */));

        if (fd < 0)
            return -1;
        n = mc_call(mcRead(fd, sWriteBuf, sizeof(sWriteBuf)));
        mc_call(mcClose(fd));
    }

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
    int fd, n;

    WaitSema(sLock);
    memcpy(sWriteBuf + sizeof(*h), sSram, SRAM_SIZE);
    sDirty = 0;
    SignalSema(sLock);

    if (!sFileBackend && !sCardOk && !(sCardOk = card_present()))
    {
        return;
    }
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "SSB64SAV", 8);
    h->version = SAVE_VERSION;
    h->sequence = ++sSequence;
    h->size = SRAM_SIZE;
    h->crc = crc32_calc(sWriteBuf + sizeof(*h), SRAM_SIZE);

    if (sFileBackend)
    {
        char path[320];

        file_slot_path(sNextSlot, path, sizeof(path));
        n = file_write_all(path, sWriteBuf, sizeof(sWriteBuf));
        if (n != (int)sizeof(sWriteBuf))
        {
            ps2_log("save: MMCE write failed (%d) for %s", n, path);
            return;
        }
        ps2_log("save: wrote %s seq %u", path, (unsigned)sSequence);
    }
    else
    {
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
    }
    sNextSlot ^= 1;
}

static void save_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        extern void ps2_delay_vblanks(int n);

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

    sFileBackend = (ps2_storage_data_device() == PS2_BOOT_MMCE);

    /*
     * MMCE occupies a memory-card slot and MMCEMAN owns its SIO2 traffic.
     * Probing that same physical slot through MCSERV can wedge the card RPC on
     * hardware. Keep SRAM beside the game on MMCE instead; the dual-slot +
     * CRC format remains identical and the working asset stream is untouched.
     */
    if (sFileBackend)
    {
        seq_a = load_slot(0, tmp);
        if (seq_a >= 0)
            memcpy(sSram, tmp, SRAM_SIZE);
        seq_b = load_slot(1, tmp);
        if (seq_b > seq_a)
            memcpy(sSram, tmp, SRAM_SIZE);

        sSequence = (uint32_t)((seq_a > seq_b) ? seq_a : (seq_b > 0 ? seq_b : 0));
        sNextSlot = (seq_a > seq_b) ? 1 : 0;
        sCardOk = 1;
        ps2_log("save: MMCE filesystem backend ready (slot A %d, slot B %d)", (int)seq_a, (int)seq_b);
    }
    else if (!mc_rpc_ready())
    {
        ps2_log("save: memory-card RPC server unavailable; saves stay in RAM");
    }
    else if (mcInit(MC_TYPE_XMC) < 0)
    {
        ps2_log("save: mcInit failed, saving disabled");
    }
    else if ((sCardOk = card_present()))
    {
        seq_a = load_slot(0, tmp);
        if (seq_a >= 0)
            memcpy(sSram, tmp, SRAM_SIZE);
        seq_b = load_slot(1, tmp);
        if (seq_b > seq_a)
            memcpy(sSram, tmp, SRAM_SIZE);

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
    StartThread(sThreadId, NULL);
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

/*
 * EE RAM accounting.
 *
 * The port avoids general-purpose malloc during gameplay: the game keeps its
 * own N64-style bump heaps inside the fixed scene arena (see arena.c), and
 * platform subsystems allocate their persistent buffers once at init through
 * ps2_mem_alloc(). Everything is attributed to a category so the debug
 * overlay can show where the 32 MiB go and whether we stay inside the
 * 24 MiB steady-state / 27 MiB peak engineering budget.
 */
#include <ps2/platform.h>

#include <malloc.h>
#include <string.h>

/* Retail EE RAM and the port's self-imposed budgets. */
#define PS2_EE_RAM_SIZE     (32u * 1024u * 1024u)
#define PS2_EE_BUDGET       (24u * 1024u * 1024u)
#define PS2_EE_PEAK_BUDGET  (27u * 1024u * 1024u)

extern char _ftext[];
extern char _end[];

static PS2MemStats sStats;

static const char *const sNames[PS2_MEM_CATEGORY_COUNT] = {
    "code/static", "game heap", "scene arena", "fighter", "gfx staging",
    "tex cache", "audio", "scratch", "threads",
};

static void recompute_totals(void)
{
    uint32_t total = 0;
    int i;

    for (i = 0; i < PS2_MEM_CATEGORY_COUNT; i++)
    {
        /* A fixed pool costs its full reservation even when partly empty. */
        total += (sStats.reserved[i] > sStats.used[i]) ? sStats.reserved[i] : sStats.used[i];
    }
    sStats.total_used = total;
    if (total > sStats.total_peak)
    {
        sStats.total_peak = total;
    }
}

void ps2_mem_init(void)
{
    memset(&sStats, 0, sizeof(sStats));
    sStats.ram_size = PS2_EE_RAM_SIZE;
    sStats.budget = PS2_EE_BUDGET;

    /* Everything from the load address to the end of .bss: code, data,
     * rodata, bss (which includes the statically reserved pools). The pools
     * living in .bss are re-attributed by ps2_mem_reserve(), so subtract
     * them there to avoid double counting. The 1 MiB below the load
     * address belongs to the EE kernel and is not counted. */
    sStats.used[PS2_MEM_CODE_STATIC] = (uint32_t)(_end - _ftext);
    sStats.peak[PS2_MEM_CODE_STATIC] = sStats.used[PS2_MEM_CODE_STATIC];
    recompute_totals();
}

void ps2_mem_reserve(PS2MemCategory cat, uint32_t bytes)
{
    sStats.reserved[cat] += bytes;
    recompute_totals();
}

/* Statically reserved pools live inside .bss, which code/static already
 * counts. Call this for such pools so they are shown under their own
 * category instead. */
void ps2_mem_reclassify_static(PS2MemCategory cat, uint32_t bytes)
{
    if (sStats.used[PS2_MEM_CODE_STATIC] >= bytes)
    {
        sStats.used[PS2_MEM_CODE_STATIC] -= bytes;
        sStats.peak[PS2_MEM_CODE_STATIC] = sStats.used[PS2_MEM_CODE_STATIC];
    }
    ps2_mem_reserve(cat, bytes);
}

void ps2_mem_set_used(PS2MemCategory cat, uint32_t bytes)
{
    sStats.used[cat] = bytes;
    if (bytes > sStats.peak[cat])
    {
        sStats.peak[cat] = bytes;
    }
    recompute_totals();
}

void ps2_mem_add(PS2MemCategory cat, int32_t delta)
{
    int64_t v = (int64_t)sStats.used[cat] + delta;

    ps2_mem_set_used(cat, (v < 0) ? 0 : (uint32_t)v);
}

const PS2MemStats *ps2_mem_stats(void)
{
    return &sStats;
}

const char *ps2_mem_category_name(PS2MemCategory cat)
{
    return ((unsigned)cat < PS2_MEM_CATEGORY_COUNT) ? sNames[cat] : "?";
}

void *ps2_mem_alloc(PS2MemCategory cat, uint32_t size, uint32_t align)
{
    void *p;

    if ((unsigned)cat >= PS2_MEM_CATEGORY_COUNT)
        ps2_panic("invalid memory category %u", (unsigned)cat);
    if (size == 0)
        size = 1;
    if (size > UINT32_MAX - 63u)
        ps2_panic("memory allocation size overflow: %u bytes", (unsigned)size);

    if (align < 16)
    {
        align = 16; /* EE DMA / cache line friendliness */
    }
    if ((align & (align - 1u)) != 0)
        ps2_panic("invalid memory alignment %u", (unsigned)align);

    size = (size + 63u) & ~63u; /* whole cache lines: safe to invalidate */
    p = memalign(align, size);

    if (p == NULL)
    {
        ps2_panic("out of EE memory: %u bytes for %s (in use %u)", (unsigned)size,
                  ps2_mem_category_name(cat), (unsigned)sStats.total_used);
    }
    ps2_mem_add(cat, (int32_t)size);

    if (sStats.total_used > PS2_EE_PEAK_BUDGET)
    {
        ps2_log("MEM WARNING: %u KiB in use exceeds peak budget", (unsigned)(sStats.total_used >> 10));
    }
    return p;
}

void ps2_mem_free(PS2MemCategory cat, void *p, uint32_t size)
{
    if (p != NULL)
    {
        size = (size + 63u) & ~63u;
        free(p);
        ps2_mem_add(cat, -(int32_t)size);
    }
}

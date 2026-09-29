/*
 * Scene arena.
 *
 * On the N64, each scene's general heap runs from the end of the current
 * overlay's .bss to the start of the next resident region (ovl1_VRAM /
 * ovl9_VRAM / the framebuffers at the top of RDRAM). The game computes that
 * size itself with expressions like
 *
 *     (uintptr_t)&ovl1_VRAM - (uintptr_t)&ovl4_BSS_END
 *
 * On PS2 all overlays are linked statically, so arena_glue.S points every
 * overlay "BSS end" at gPS2SceneArena and every "top of heap" symbol at its
 * end, which is also where gSYFramebufferSets begins - exactly the N64
 * ordering. The game's own bump allocators (syTaskmanMalloc etc.) then work
 * unchanged inside a fixed, accounted block.
 */
#include <ps2/platform.h>

#include <string.h>

extern uint8_t gPS2SceneArena[];
extern uint8_t gPS2SceneArenaEnd[];

void ps2_arena_init(void)
{
    uint32_t size = (uint32_t)(gPS2SceneArenaEnd - gPS2SceneArena);

    ps2_mem_reclassify_static(PS2_MEM_SCENE_ARENA, size);
    ps2_log("arena: scene arena %u KiB at %p", (unsigned)(size >> 10), gPS2SceneArena);
}

/* Debug overlay: high-water mark of the arena in use, measured by scanning
 * for the untouched fill pattern would be expensive; the game's heap
 * pointer is sampled instead (see overlay.c). */
uint32_t ps2_arena_size(void)
{
    return (uint32_t)(gPS2SceneArenaEnd - gPS2SceneArena);
}

uintptr_t ps2_arena_base(void)
{
    return (uintptr_t)gPS2SceneArena;
}

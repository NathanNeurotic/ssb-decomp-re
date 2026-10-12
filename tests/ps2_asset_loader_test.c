/* Execute the production asset bootstrap against a fake USB DAT.
 * Host-only test; no PS2 toolchain or copyrighted game files required.
 */
#include <ps2/platform.h>
#include <ps2/assetpack.h>
#include <kernel.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char image[256];
static size_t image_len, position;
static int missing;
static int open_calls;

int CreateSema(ee_sema_t *s) { (void)s; return 1; }
int WaitSema(int id) { (void)id; return 0; }
int SignalSema(int id) { (void)id; return 0; }

void ps2_log(const char *fmt, ...) { (void)fmt; }
void ps2_panic(const char *fmt, ...) { (void)fmt; abort(); }
void *ps2_mem_alloc(PS2MemCategory cat, uint32_t size, uint32_t align)
{
    (void)cat; (void)align;
    return malloc(size ? size : 1);
}
PS2BootDevice ps2_storage_boot_device(void) { return PS2_BOOT_MASS; }
int ps2_storage_has_data_override(void) { return 0; }
int ps2_storage_pack_path(char *out, size_t size)
{
    const char *p = "mass1:/APPS/SSB64.DAT";
    if (size <= strlen(p))
        return 0;
    strcpy(out, p);
    return 1;
}
int ps2_file_open_read(const char *path)
{
    assert(strcmp(path, "mass1:/APPS/SSB64.DAT") == 0);
    open_calls++;
    position = 0;
    if (missing) { errno = ENOENT; return -1; }
    return 7;
}
int ps2_file_seek(int fd, uint32_t offset)
{
    assert(fd == 7);
    if (offset > image_len) { errno = EIO; return -1; }
    position = offset;
    return (int)offset;
}
int ps2_file_read(int fd, void *dst, uint32_t size)
{
    size_t available;
    assert(fd == 7);
    available = image_len - position;
    if (size > available)
        size = (uint32_t)available;
    memcpy(dst, image + position, size);
    position += size;
    return (int)size;
}
int ps2_file_size(int fd) { (void)fd; return (int)image_len; }
void ps2_file_close(int fd) { (void)fd; }

static void make_valid_pack(void)
{
    PS2PackHeader header;
    PS2PackRegion region;
    memset(image, 0, sizeof(image));
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, PS2PACK_MAGIC, 8);
    header.version = PS2PACK_VERSION;
    header.region_count = 1;
    header.region_table_offset = sizeof(header);
    header.resident_bytes = 4;
    header.resident_offset = sizeof(header) + sizeof(region);
    header.total_size = header.resident_offset + header.resident_bytes;
    memcpy(image, &header, sizeof(header));
    memset(&region, 0, sizeof(region));
    region.vrom_start = 0x20000000u;
    region.size = 4;
    region.file_offset = header.resident_offset;
    region.flags = PS2PACK_REGION_RESIDENT;
    memcpy(image + header.region_table_offset, &region, sizeof(region));
    image[header.resident_offset] = 0x12;
    image_len = header.total_size;
    missing = 0;
}
int main(void)
{
    PS2PackHeader *header = (PS2PackHeader *)image;
    make_valid_pack();
    missing = 1;
    assert(ps2_assets_init() == 0);
    assert(strstr(ps2_assets_last_error(), "open failed") != NULL);
    missing = 0;
    make_valid_pack();
    /* Reusing the verified USB descriptor avoids a second open. */
    assert(ps2_assets_adopt_open_fd(7) == 1);
    assert(ps2_assets_adopt_open_fd(8) == 0);
    assert(ps2_assets_init() == 1);
    assert(open_calls == 1); /* Only the intentional ENOENT probe ran. */
    make_valid_pack();
    header->magic[0] = 'X';
    assert(ps2_assets_init() == 0);
    assert(strstr(ps2_assets_last_error(), "magic") != NULL);
    make_valid_pack();
    header->version = 999;
    assert(ps2_assets_init() == 0);
    assert(strstr(ps2_assets_last_error(), "version") != NULL);
    make_valid_pack();
    header->region_table_offset = 0xFFFFFFF0u;
    assert(ps2_assets_init() == 0);
    assert(strstr(ps2_assets_last_error(), "sizes") != NULL);
    make_valid_pack();
    image_len = sizeof(PS2PackHeader) + 3;
    assert(ps2_assets_init() == 0);
    assert(strstr(ps2_assets_last_error(), "region table") != NULL);
    make_valid_pack();
    assert(ps2_assets_init() == 1);
    assert(!strcmp(ps2_assets_last_error(), "OK"));
    puts("PASS: USB DAT open, adopt/reuse, invalid header, version, short read, and valid pack");
    return 0;
}

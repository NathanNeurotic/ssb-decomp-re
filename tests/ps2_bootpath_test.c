/* Host-side boot-path identity regression: cc -std=c99 -Wall -Wextra
 * -Ips2/include ps2/src/storage/bootpath.c tests/ps2_bootpath_test.c
 * -o /tmp/ps2_bootpath_test && /tmp/ps2_bootpath_test
 */
#include <ps2/platform.h>
#include <assert.h>
#include <string.h>

static void check(const char *source, PS2BootDevice device, const char *dir)
{
    ps2_storage_set_boot_path(source);
    assert(ps2_storage_boot_device() == device);
    assert(strcmp(ps2_storage_boot_dir(), dir) == 0);
}

int main(void)
{
    char path[300];
    check("mass0:/APPS/SSB64.ELF", PS2_BOOT_MASS, "mass0:/APPS/");
    check("mass1:/APPS/SSB64.ELF", PS2_BOOT_MASS, "mass1:/APPS/");
    check("mass2:/SSB64.ELF", PS2_BOOT_MASS, "mass2:/");
    check("usb1:/APPS/SSB64.ELF", PS2_BOOT_MASS, "usb1:/APPS/");
    check("bdm2:/GAMES/SSB64.ELF", PS2_BOOT_UNKNOWN, "bdm2:/GAMES/");
    check("mmce0:/SSB64.ELF", PS2_BOOT_MMCE, "mmce0:/");
    check("mx4sio0:/SSB64.ELF", PS2_BOOT_MX4SIO, "mx4sio0:/");
    check("ata0:/GAMES/SSB64.ELF", PS2_BOOT_ATA, "ata0:/GAMES/");
    check("ilink0:/SSB64.ELF", PS2_BOOT_ILINK, "ilink0:/");
    check("udpbd0:/SSB64.ELF", PS2_BOOT_UDPBD, "udpbd0:/");
    check("udpfs:/SSB64.ELF", PS2_BOOT_UDPFS, "udpfs:/");
    check("bdm:/SSB64.ELF", PS2_BOOT_UNKNOWN, "bdm:/");
    check("mc1:/APPS/SSB64.ELF", PS2_BOOT_MC, "mc1:/APPS/");
    check("host:ssb64.elf", PS2_BOOT_HOST, "host:");
    check("hdd0:__common:pfs:/APPS/SSB64.ELF", PS2_BOOT_HDD, "pfs0:/APPS/");
    assert(strcmp(ps2_storage_boot_hdd_partition(), "hdd0:__common") == 0);
    check("hdd1:+OPL:pfs:/APPS/SSB64.ELF", PS2_BOOT_HDD, "pfs0:/APPS/");
    assert(strcmp(ps2_storage_boot_hdd_partition(), "hdd1:+OPL") == 0);
    check("mass0:/SSB64.ELF", PS2_BOOT_MASS, "mass0:/");
    assert(ps2_storage_boot_hdd_partition()[0] == 0);
    check("mass0:/SSB64_240p.elf", PS2_BOOT_MASS, "mass0:/");
    assert(ps2_video_progressive() == 1);
    check("mass0:/SSB64.ELF", PS2_BOOT_MASS, "mass0:/");
    assert(ps2_video_progressive() == 0);
    ps2_storage_path(path, sizeof(path), "SSB64.DAT");
    assert(strcmp(path, "mass0:/SSB64.DAT") == 0);
    assert(ps2_storage_set_data_path("mass2:/DATA/") == 1);
    assert(ps2_storage_data_device() == PS2_BOOT_MASS);
    assert(ps2_storage_has_data_override());
    assert(ps2_storage_pack_path(path, sizeof(path)) == 1);
    assert(strcmp(path, "mass2:/DATA/SSB64.DAT") == 0);
    assert(ps2_storage_set_data_path("udpfs:/SSB64.DAT") == 1);
    assert(ps2_storage_data_device() == PS2_BOOT_UDPFS);
    assert(ps2_storage_set_data_path("mmce1:/SSB64.DAT") == 1);
    assert(ps2_storage_data_device() == PS2_BOOT_MMCE);
    assert(ps2_storage_pack_path(path, sizeof(path)) == 1);
    assert(strcmp(path, "mmce1:/SSB64.DAT") == 0);
    assert(ps2_storage_set_data_path("relative/data") == 0);
    assert(ps2_storage_has_data_override() == 0);
    assert(ps2_storage_data_device() == PS2_BOOT_MASS);
    assert(ps2_storage_set_data_path(NULL) == 1);
    assert(ps2_storage_pack_path(path, sizeof(path)) == 1);
    assert(strcmp(path, "mass0:/SSB64.DAT") == 0);
    assert(ps2_storage_pack_path(path, 5) == 0);
    return 0;
}

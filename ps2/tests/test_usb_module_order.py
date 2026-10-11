#!/usr/bin/env python3
"""Compile real PS2 IOP transport loaders with deterministic module-order mocks.

Covers cold USB USBD->BDM->BDMFS->USBMASS, inherited IOP preservation,
fallback multi-transport setup, and USB-module failure isolation.
Real hardware USB initialization / drive identity are not simulated.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = (root / "ps2/src/platform/iop.c").read_text(encoding="utf-8")


def extract(signature):
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (src[end] == "{") - (src[end] == "}")
        end += 1
    return src[start:end]


functions = "\n\n".join([
    extract("static int load_bdm_core(void)"),
    extract("int ps2_iop_load_boot_device_drivers("),
    extract("int ps2_iop_load_bdm_fallback_transports("),
])

harness = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef int PS2BootDevice;
enum {
 PS2_BOOT_UNKNOWN, PS2_BOOT_HOST, PS2_BOOT_BDM, PS2_BOOT_USB, PS2_BOOT_MC,
 PS2_BOOT_ATA, PS2_BOOT_MX4SIO, PS2_BOOT_ILINK, PS2_BOOT_UDPBD,
 PS2_BOOT_UDPFS, PS2_BOOT_HDD, PS2_BOOT_MMCE, PS2_BOOT_CDROM
};
static int sInheritedBdm, sRebuiltBdm, sInstalledBdm, sSleep;
static const char *failure;
static char sequence[64];
static unsigned int sequence_count;
static char tag(const char *name) {
    if (!strcmp(name,"usbd_mini")) return 'U';
    if (!strcmp(name,"bdm")) return 'B';
    if (!strcmp(name,"bdmfs_fatfs")) return 'F';
    if (!strcmp(name,"usbmass_bd_mini")) return 'M';
    if (!strcmp(name,"mx4sio_bd")) return 'X';
    if (!strcmp(name,"ps2dev9")) return 'D';
    if (!strcmp(name,"ps2atad")) return 'A';
    if (!strcmp(name,"iLinkman")) return 'I';
    if (!strcmp(name,"IEEE1394_bd")) return 'L';
    return '?';
}
static int fake_load(const char *name) {
    assert(sequence_count + 1 < sizeof(sequence));
    sequence[sequence_count++] = tag(name);
    sequence[sequence_count] = 0;
    return failure && !strcmp(failure, name) ? -1 : 0;
}
#define LOAD_IRX(name) fake_load(#name)
#define LOAD_IRX_ARGS(name,arg,len) fake_load(#name)
#define ps2_log(...) ((void)0)
#define sleep(n) do { sSleep++; } while (0)
static int ps2_iop_module_loaded(const char *name) {
    return !strcmp(name,"bdm") && sInstalledBdm;
}
static int read_ip_arg(char *out, size_t n) {
    (void)n;strcpy(out,"ip=192.0.2.1");return 1;
}
static const char *ps2_storage_hdd_mount_source(void) {return "hdd0:TEST";}
static int ps2_storage_requires_iop_preserve(void) {return 0;}
static int mount_hdd_partition(void) {return 0;}
__FUNCTIONS__

static void reset(const char *fail) {
    sequence[0] = 0;
    sequence_count = 0;
    sSleep = 0;
    failure = fail;
    sInheritedBdm = 0;
    sRebuiltBdm = 0;
    sInstalledBdm = 0;
}
static void assert_sequence(const char *expected) {
    if (strcmp(sequence,expected))
        fprintf(stderr,"got order %s, expected %s\n",sequence,expected);
    assert(!strcmp(sequence,expected));
}
int main(void) {
    reset(NULL);
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB) == 0);
    assert_sequence("UBFM");

    reset("usbd_mini");
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB) < 0);
    assert_sequence("U"); /* Must NOT load BDM or USBMASS without USBD */

    reset("bdm");
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB) < 0);
    assert_sequence("UB");

    reset("bdmfs_fatfs");
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB) < 0);
    assert_sequence("UBF");

    reset("usbmass_bd_mini");
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB) < 0);
    assert_sequence("UBFM");

    reset(NULL);
    sInheritedBdm = 1;
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB) == 0);
    assert_sequence("");
    assert(ps2_iop_load_bdm_fallback_transports() == 0);
    assert_sequence("");

    reset(NULL);
    sRebuiltBdm = 1;
    sInstalledBdm = 1;
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB) == 0);
    assert_sequence("");

    reset(NULL);
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_MX4SIO) == 0);
    assert_sequence("BFX");
    reset(NULL);
    assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_ATA) == 0);
    assert_sequence("DBFA");

    /* BDM fallback starts USB host first, then general BDM core, then mass.
     * This also catches any accidental reordering of the non-USB followups. */
    reset(NULL);
    assert(ps2_iop_load_bdm_fallback_transports() == 1);
    assert_sequence("UBFMXDAIL");
    assert(sSleep == 1);

    reset(NULL);
    assert(ps2_iop_load_bdm_fallback_transports() == 0);
    assert_sequence(""); /* No second initialization on repeated probe */

    puts("PASS: Launchstation USB-first parity, fallback transports, module failure isolation, inherited IOP");
    return 0;
}
""".replace("__FUNCTIONS__", functions)

with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "usb_order"
    c.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2",
                    "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)

"""Exercise actual transport-loader control flow with host IOP mocks."""
from pathlib import Path
import subprocess
import tempfile
import os
import runpy
root = Path(__file__).resolve().parents[2]
s = (root / "ps2/src/platform/iop.c").read_text()
a = s.index("int ps2_iop_load_boot_device_drivers(")
b = s.index("int ps2_iop_module_loaded", a)
c = r"""
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef int PS2BootDevice;
enum { PS2_BOOT_UNKNOWN, PS2_BOOT_HOST, PS2_BOOT_BDM, PS2_BOOT_USB, PS2_BOOT_MC, PS2_BOOT_ATA, PS2_BOOT_MX4SIO, PS2_BOOT_ILINK, PS2_BOOT_UDPBD, PS2_BOOT_UDPFS, PS2_BOOT_HDD, PS2_BOOT_MMCE, PS2_BOOT_CDROM };
static int sInheritedBdm, sRebuiltBdm, loads, cores, fail, installed;
static int ps2_iop_module_loaded(const char *name){(void)name; return installed;}
static int load(void){ loads++; return fail ? -1 : 0; }
#define LOAD_IRX(n) load()
#define LOAD_IRX_ARGS(n,a,l) load()
#define ps2_log(...) ((void)0)
#define sleep(x) ((void)0)
static int load_bdm_core(void){cores++;return fail ? -1 : 0;}
static int read_ip_arg(char *p,int n){(void)n;strcpy(p,"ip=1.2.3.4");return 1;}
static const char *ps2_storage_hdd_mount_source(void){return "hdd0:test";}
static int ps2_storage_requires_iop_preserve(void){return 0;}
static int mount_hdd_partition(void){return 0;}
""" + s[a:b] + r"""
int main(void){
 int devices[]={PS2_BOOT_BDM,PS2_BOOT_USB,PS2_BOOT_ATA,PS2_BOOT_MX4SIO,PS2_BOOT_ILINK,PS2_BOOT_UDPBD};
 for(unsigned i=0;i<sizeof(devices)/sizeof(devices[0]);i++){
  sInheritedBdm=1;loads=cores=0; assert(ps2_iop_load_boot_device_drivers(devices[i])==0);assert(!loads&&!cores);
 }
 sInheritedBdm=0;loads=cores=0;assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB)==0);assert(loads==2&&cores==1);
 fail=1;assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB)<0);fail=0;
 sInheritedBdm=1;loads=cores=0;assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_UDPFS)==0);assert(loads==4&&!cores);
 loads=cores=0;assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_MMCE)==0);assert(loads==1&&!cores);
 assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_UNKNOWN)<0);
 sRebuiltBdm=installed=1;loads=cores=0;
 assert(ps2_iop_load_boot_device_drivers(PS2_BOOT_USB)==0);assert(!loads&&!cores);
 installed=0;sInheritedBdm=1;loads=cores=0;
 assert(ps2_iop_load_bdm_fallback_transports()==0);assert(!loads&&!cores);
 sInheritedBdm=0;fail=1;assert(ps2_iop_load_bdm_fallback_transports()<0);
 fail=0;assert(ps2_iop_load_bdm_fallback_transports()==1);
 loads=cores=0;assert(ps2_iop_load_bdm_fallback_transports()==0);assert(!loads&&!cores);
 puts("PASS: inherited aliases, fresh USB, load failure, UDPFS/MMCE isolation, unknown device");
}
"""
with tempfile.TemporaryDirectory() as t:
 src=Path(t)/"test.c"; exe=Path(t)/"test.exe";src.write_text(c)
 subprocess.run([os.environ.get("CC","gcc"),"-O2",str(src),"-o",str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

# This script is already a required CI build gate. Keep loader-handoff
# coverage in that gate, including on installations without workflow scope.
runpy.run_path(str(root / "ps2/tests/test_iop_handoff.py"), run_name="__main__")

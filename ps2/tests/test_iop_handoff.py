"""Exercise actual startup policy for kept and loader-reset IOPs."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "ps2/src/platform/iop.c").read_text()

def extract(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int PS2BootDevice;
enum {PS2_BOOT_BDM, PS2_BOOT_USB, PS2_BOOT_MX4SIO, PS2_BOOT_ATA,
 PS2_BOOT_ILINK, PS2_BOOT_UDPBD, PS2_BOOT_HOST};
typedef struct { unsigned short version; } smod_mod_info_t;
static int sInheritedBdm, sRebuiltBdm, sLoadedCount, sIopWasReset;
static int sLastFileXioModuleId, sLastFileXioModuleResult;
static int preserve, device, rpc, rom_only, partial, modern, resets;
static int transport_ready, controllers_started, cores, installs;
static int smod_get_mod_by_name(const char *name, smod_mod_info_t *info) {
 if (!strcmp(name,"bdm") && partial) return 1;
 if (!strcmp(name,"IO/File_Manager")) {
  info->version = modern ? 0x203 : 0x101; return rom_only || modern;
 }
 return 0;
}
static int ps2_storage_requires_iop_preserve(void){return preserve;}
static int ps2_storage_data_device(void){return device;}
static int inherited_filexio_rpc_ready(void){return rpc;}
#define ps2_log(...) ((void)0)
#define ps2_boot_stage(...) ((void)0)
#define ps2_panic(...) abort()
#define SifInitRpc(...) ((void)0)
#define SifIopSync() 1
static int SifIopReset(const char *p,int f){(void)p;(void)f;resets++;return 1;}
#define SifLoadFileInit() ((void)0)
#define SifInitIopHeap() ((void)0)
#define sbv_patch_enable_lmb() ((void)0)
#define sbv_patch_disable_prefix_check() ((void)0)
#define sbv_patch_fileio() ((void)0)
static int init_filexio_runtime(void){return 0;}
static int load(const char *name){
 if (!strcmp(name,"iomanx")) cores++;
 if (!strcmp(name,"padman")) {
  if(sRebuiltBdm && device!=PS2_BOOT_UDPBD) assert(transport_ready);
  controllers_started=1;
 }
 return 0;
}
#define LOAD_IRX(name) load(#name)
static int ps2_iop_load_bdm_fallback_transports(void){
 assert(!controllers_started && !sInheritedBdm);transport_ready=1;installs++;return 1;
}
static int ps2_iop_load_boot_device_drivers(int dev){
 assert(dev==device && !controllers_started && !sInheritedBdm);
 transport_ready=1;installs++;return 0;
}
__FUNCTIONS__
static void reset(void){
 preserve=1;device=PS2_BOOT_BDM;rpc=rom_only=partial=modern=resets=0;
 transport_ready=controllers_started=cores=installs=0;
}
int main(void){
 /* ROM FILEIO/ioman surviving a loader reset is not a BDM stack. */
 reset();rom_only=1;ps2_iop_init();
 assert(resets==1 && sRebuiltBdm && !sInheritedBdm && cores==1 && installs==1);
 reset();rom_only=1;device=PS2_BOOT_USB;ps2_iop_init();
 assert(resets==1 && installs==1);
 reset();rom_only=1;device=PS2_BOOT_UDPBD;ps2_iop_init();
 assert(resets==1 && sRebuiltBdm && installs==0); /* needs MC IP config later */
 /* Modern replacement ioman, a partial BDM core, or a live fileXio
    must each prohibit destructive reconstruction. */
 reset();modern=1;ps2_iop_init();
 assert(!resets && sInheritedBdm && !cores && !installs);
 reset();partial=1;ps2_iop_init();
 assert(!resets && sInheritedBdm && !installs);
 reset();rpc=1;ps2_iop_init();
 assert(!resets && sInheritedBdm && !installs);
 reset();device=PS2_BOOT_HOST;ps2_iop_init();assert(!resets && !sRebuiltBdm);
 puts("PASS: ROM-only reset recovery, typed recovery, kept/partial stacks, storage before PAD");
}
'''.replace("__FUNCTIONS__", "\n".join([
    extract("static int inherited_storage_modules_present(void)"),
    extract("void ps2_iop_init(void)")]))

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "handoff.c"
    exe = Path(tmp) / "handoff.exe"
    src.write_text(harness)
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2",
                    str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)

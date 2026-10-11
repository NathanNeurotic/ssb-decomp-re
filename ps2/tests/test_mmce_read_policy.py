"""Exercise actual MMCEMAN read fairness, partial reads and bounded retries."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "ps2/src/storage/file.c").read_text()
start = source.index("int ps2_file_read(")
end = source.index("int ps2_file_seek(", start)
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
enum { PS2_BOOT_MMCE, PS2_BOOT_HOST, PS2_BOOT_CDROM, PS2_BOOT_BDM };
static int device, calls, yields, retries, failures, partial;
static uint32_t largest, transferred;
static int ps2_storage_data_device(void){return device;}
static void DelayThread(int us){
 assert(us==500 || us==1000);
 if(us==500)yields++;else retries++;
}
static int fake_read(int fd, void *dst, uint32_t size){
 assert(fd==7); (void)dst;
 calls++; if(size>largest)largest=size;
 if(failures){failures--;return -1;}
 if(partial && size>333)size=333;
 transferred+=size;return size;
}
#define read fake_read
__FUNCTION__
static void reset(int dev){device=dev;calls=yields=retries=failures=partial=0;largest=transferred=0;}
int main(void){
 uint8_t data[65536];
 reset(PS2_BOOT_MMCE);
 assert(ps2_file_read(7,data,sizeof(data))==sizeof(data));
 assert(calls==32 && largest==2048 && yields==31 && retries==0);
 reset(PS2_BOOT_MMCE);partial=1;
 assert(ps2_file_read(7,data,4096)==4096);assert(transferred==4096 && yields==calls-1);
 reset(PS2_BOOT_MMCE);failures=2;
 assert(ps2_file_read(7,data,100)==100);assert(calls==3 && retries==2 && !yields);
 reset(PS2_BOOT_MMCE);failures=99;
 assert(ps2_file_read(7,data,100)==-1);assert(calls==3 && retries==2 && !yields);
 reset(PS2_BOOT_BDM);
 assert(ps2_file_read(7,data,sizeof(data))==sizeof(data));
 assert(calls==4 && largest==16384 && !yields && !retries);
 reset(PS2_BOOT_HOST);
 assert(ps2_file_read(7,data,sizeof(data))==sizeof(data));assert(calls==1 && !yields);
 reset(PS2_BOOT_MMCE);assert(ps2_file_read(7,data,0)==0);assert(!calls && !yields);
 puts("PASS: MMCEMAN 2KiB fairness, partial reads, bounded failures, other transports unchanged");
}
'''.replace("__FUNCTION__", source[start:end])
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "read.c"
    exe = Path(tmp) / "read.exe"
    src.write_text(harness)
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2",
                    str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)

#!/usr/bin/env python3
"""Host regressions extracted from the actual ELF runtime C functions.

This does not validate PS2 SIF, pad, graphics or USB timing. It guards the
runtime contracts that can be exercised with deterministic host I/O mocks.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extract(path, signature):
    source = (ROOT / path).read_text(encoding="utf-8")
    at = source.index(signature)
    start = source.index("{", at)
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[at:end]


FUNCTIONS = "\n\n".join([
    extract("ps2/src/storage/bootpath.c", "int ps2_storage_resolve_data_root("),
    extract("ps2/src/storage/save.c", "void ps2_sram_read("),
    extract("ps2/src/storage/save.c", "void ps2_sram_write("),
    extract("ps2/src/audio/spu.c", "static int audio_rpc_drain("),
    extract("ps2/src/renderer/texcache.c", "static void *staging_alloc("),
])

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define PATH_BUF_MAX 256
#define SRAM_SIZE (32 * 1024)
#define STAGING_BYTES (1024 * 1024)
#define O_RDONLY 0
#define ps2_log(...) ((void)0)
#define WaitSema(...) (0)
#define SignalSema(...) (0)
#define DelayThread(v) fake_delay(v)

static char sDataDir[PATH_BUF_MAX];
static int sDataNeedsBdmResolve;
static int sDataDevice;
static const char *sAvailable;
static int sWrongVolumeOpens, sOpens;
static uint8_t sSram[SRAM_SIZE];
static int sLock;
static volatile int sDirty;
static volatile uint32_t sDirtyVBlank, sDirtyGeneration;
static volatile int sRpcPending;
static uint32_t sRpcRecv[4];
static int sAudioRpc, sRpcBusy, sRpcChecks;
static unsigned sDelayCount;
static uint8_t sStagingData[STAGING_BYTES], *sStaging = sStagingData;
static uint32_t sStagingPos;
static int sFinishCount;

static int fake_open(const char *path, int mode)
{
    (void)mode;
    sOpens++;
    if (!strncmp(path, "mass1:", 6))
        sWrongVolumeOpens++;
    return sAvailable && !strcmp(path, sAvailable) ? 3 : -1;
}
static int fake_close(int fd) { (void)fd; return 0; }
static const char *ps2_storage_device_name(int dev)
{
    (void)dev;
    return "mass";
}
static void ensure_directory_suffix(char *path, size_t cap, int dev)
{
    size_t n = strlen(path);
    (void)dev;
    if (n && path[n - 1] != '/' && n + 1 < cap) {
        path[n] = '/';
        path[n + 1] = 0;
    }
}
static uint32_t ps2_vblank_count(void) { return 42; }
static void fake_delay(int us) { (void)us; sDelayCount++; }
static int sceSifCheckStatRpc(int *rpc)
{
    (void)rpc;
    sRpcChecks++;
    return sRpcBusy;
}
static void ps2_pkt_finish(void) { sFinishCount++; }

#define open fake_open
#define close fake_close
__FUNCTIONS__
#undef open
#undef close

int main(void)
{
    /* A literal mass0: must never remap to another drive's matching DAT. */
    strcpy(sDataDir, "mass0:/GAME/");
    sDataNeedsBdmResolve = 1;
    sAvailable = "mass1:/GAME/SSB64.DAT";
    sWrongVolumeOpens = sOpens = 0;
    assert(ps2_storage_resolve_data_root("SSB64.DAT") == 0);
    assert(sWrongVolumeOpens == 0);
    assert(sDataNeedsBdmResolve == 1);

    /* Explicit volume permits alternate folders ON THAT SAME VOLUME. */
    sAvailable = "mass0:/SSB64/SSB64.DAT";
    assert(ps2_storage_resolve_data_root("SSB64.DAT") == 1);
    assert(!strcmp(sDataDir, "mass0:/SSB64/"));
    assert(!sDataNeedsBdmResolve);

    /* An intentionally generic mass: alias can resolve actual slots. */
    strcpy(sDataDir, "mass:/GAME/");
    sDataNeedsBdmResolve = 1;
    sAvailable = "mass2:/APPS/SSB64/SSB64.DAT";
    assert(ps2_storage_resolve_data_root("SSB64.DAT") == 1);
    assert(!strcmp(sDataDir, "mass2:/APPS/SSB64/"));

    /* SRAM end-crossing reads zero the tail; writes do not wrap/overflow. */
    memset(sSram, 0, sizeof(sSram));
    sSram[SRAM_SIZE-2] = 0xA5;
    sSram[SRAM_SIZE-1] = 0x5A;
    uint8_t out[6] = {1,1,1,1,1,1};
    ps2_sram_read(SRAM_SIZE-2, out, sizeof(out));
    assert(out[0] == 0xA5 && out[1] == 0x5A);
    for (int i=2;i<6;i++) assert(out[i] == 0);
    memset(out, 1, sizeof(out));
    ps2_sram_read(UINT32_MAX, out, sizeof(out));
    for (int i=0;i<6;i++) assert(out[i] == 0);

    uint8_t src[3] = {7,8,9};
    ps2_sram_write(SRAM_SIZE-1, src, sizeof(src));
    assert(sSram[SRAM_SIZE-1] == 7 && sDirtyGeneration == 1);
    ps2_sram_write(SRAM_SIZE-1, src, 0);
    assert(sDirtyGeneration == 1);
    ps2_sram_write(SRAM_SIZE-1, src, sizeof(src));
    assert(sDirtyGeneration == 2); /* even inside the same VBlank */

    /* Stuck audio RPC returns in bounded time and retains buffer ownership. */
    sRpcPending=1; sRpcBusy=1; sRpcChecks=0; sDelayCount=0;
    assert(audio_rpc_drain() < 0);
    assert(sRpcPending && sRpcChecks == 4000 && sDelayCount == 4000);
    sRpcBusy=0; sRpcRecv[0]=17;
    assert(audio_rpc_drain() == 17 && !sRpcPending);

    /* Staging ring cannot advance past its allocation, even on bad dimensions. */
    assert(staging_alloc(STAGING_BYTES+64) == NULL);
    assert(sStagingPos == 0);
    assert(staging_alloc(STAGING_BYTES-64) == sStagingData);
    assert(staging_alloc(128) == sStagingData && sFinishCount == 1);
    assert(sStagingPos == 128);

    puts("PASS: literal mass, alias fallback, SRAM bounds, audio timeout, staging ring");
    return 0;
}
"""
code = HARNESS.replace("__FUNCTIONS__", FUNCTIONS)
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "runtime_safety.c"
    exe = Path(tmp) / "runtime_safety"
    src.write_text(code, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2",
                    "-Wall", "-Wextra", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)

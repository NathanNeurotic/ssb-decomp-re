#!/usr/bin/env python3
"""Compile the ELF's actual fileXio discovery and recovery code with EE RPC mocks."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
C_SOURCE = (ROOT / "ps2/src/platform/iop.c").read_text(encoding="utf-8")

def extract(signature):
    start = C_SOURCE.index(signature)
    brace = C_SOURCE.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (C_SOURCE[end] == "{") - (C_SOURCE[end] == "}")
        end += 1
    return C_SOURCE[start:end]

functions = "\n\n".join([
    extract("static int inherited_filexio_rpc_ready(void)"),
    extract("static int init_filexio_runtime(void)")
])

harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FILEXIO_IRX 0x0b0b0b00
#define ps2_log(...) ((void)0)
typedef struct { void *server; uint32_t pad[32]; } SifRpcClientData_t;
static int sInheritedBdm;
static int sLastFileXioModuleId = -999;
static int sLastFileXioModuleResult = -999;
static int bind_calls, load_calls, init_calls, delays, negative_remaining;
static int server_enabled, server_after_load, load_fails, ee_client_fails;

static int sceSifBindRpc(SifRpcClientData_t *probe, int sid, int mode)
{
    assert(sid == FILEXIO_IRX && mode == 0);
    bind_calls++;
    if (negative_remaining-- > 0)
        return -1;
    probe->server = server_enabled ? (void*)0x1000 : NULL;
    return 0;
}
static void DelayThread(int micros)
{
    assert(micros == 10000);
    delays++;
}
static int fake_load(void)
{
    load_calls++;
    if (server_after_load)
        server_enabled = 1;
    if (load_fails)
    {
        sLastFileXioModuleId = -5;
        sLastFileXioModuleResult = 0;
        return -1;
    }
    sLastFileXioModuleId = 8;
    sLastFileXioModuleResult = 0;
    server_enabled = 1;
    return 0;
}
#define LOAD_IRX(name) fake_load()
static int fileXioInit(void)
{
    init_calls++;
    return ee_client_fails ? -1 : 0;
}

__FUNCTIONS__

static void reset(void)
{
    sInheritedBdm = 1;
    sLastFileXioModuleId = sLastFileXioModuleResult = -999;
    bind_calls = load_calls = init_calls = delays = negative_remaining = 0;
    server_enabled = server_after_load = load_fails = ee_client_fails = 0;
}
int main(void)
{
    /* Inherited fileXio: do not execute a duplicate module. */
    reset();
    server_enabled = 1;
    assert(init_filexio_runtime() == 0);
    assert(load_calls == 0 && init_calls == 1);

    /* Temporary SIF packet allocation/send failures must not imply absence. */
    reset();
    server_enabled = 1;
    negative_remaining = 3;
    assert(init_filexio_runtime() == 0);
    assert(load_calls == 0 && bind_calls == 4);

    /* Missing initial server: embedded fileXio loads, then EE binds. */
    reset();
    assert(init_filexio_runtime() == 0);
    assert(load_calls == 1 && init_calls == 1);

    /* Key field regression: duplicate module REJECTED, existing server
       becomes reachable. Must continue to bind rather than fatal on IRX rc. */
    reset();
    load_fails = 1;
    server_after_load = 1;
    assert(init_filexio_runtime() == 0);
    assert(load_calls == 1 && init_calls == 1);

    /* A genuinely absent server cannot be reported as ready. */
    reset();
    load_fails = 1;
    assert(init_filexio_runtime() == -1);
    assert(load_calls == 1 && init_calls == 0);

    /* A successful probe does not imply EE client initialization succeeded. */
    reset();
    server_enabled = 1;
    ee_client_fails = 1;
    assert(init_filexio_runtime() == -2);
    assert(init_calls == 1);

    /* Fresh IOP must attempt the embedded module. */
    reset();
    sInheritedBdm = 0;
    server_enabled = 1;
    assert(init_filexio_runtime() == 0);
    assert(load_calls == 1);

    puts("PASS: inherited fileXio, transient bind errors, duplicate reject, fresh module, missing RPC");
}
""".replace("__FUNCTIONS__", functions)
with tempfile.TemporaryDirectory() as d:
    src = Path(d) / "test_filexio.c"
    exe = Path(d) / "test_filexio"
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2", "-Wall",
                    "-Wextra", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)

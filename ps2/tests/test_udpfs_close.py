#!/usr/bin/env python3
"""Use the actual UDPFS close function to verify malformed reply/error behavior."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
s = (ROOT / "ps2/iop/udpfs_ioman/src/udpfs_core.c").read_text()
start = s.index("int udpfs_core_close(int32_t handle)")
opening = s.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (s[end] == "{") - (s[end] == "}")
    end += 1
function = s[start:end]
test = r"""
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#define M_DEBUG(...) ((void)0)
#define UDPFS_MSG_CLOSE_REQ 0x12
#define UDPFS_MSG_CLOSE_REPLY 0x13
typedef struct {
    uint8_t msg_type, reserved[3];
    int32_t handle;
} udpfs_msg_close_req_t;
typedef struct {
    uint8_t msg_type, reserved[3];
    int32_t result;
} udpfs_msg_close_reply_t;
static uint8_t g_tx_buf[64],g_rx_buf[64];
static int g_socket,connected=1,mode=0,requests=0;
static int udprdma_is_connected(int sock){(void)sock;return connected;}
static int _request(const void *req,unsigned size,void *reply,unsigned max)
{
    const udpfs_msg_close_req_t *r=(const udpfs_msg_close_req_t*)req;
    assert(size==sizeof(*r) && r->msg_type==UDPFS_MSG_CLOSE_REQ);
    assert(max==sizeof(udpfs_msg_close_reply_t));
    udpfs_msg_close_reply_t *out=(udpfs_msg_close_reply_t*)reply;
    requests++;
    memset(out,0,sizeof(*out));
    if(mode==1) return -EIO;
    if(mode==2) return 3;
    out->msg_type=(mode==3)?0xFF:UDPFS_MSG_CLOSE_REPLY;
    out->result=(mode==4)?-ENOSPC:0;
    return sizeof(*out);
}
__FUNCTION__
int main(void)
{
    assert(udpfs_core_close(4)==0 && requests==1);
    mode=1;assert(udpfs_core_close(4)<0);
    mode=2;assert(udpfs_core_close(4)<0);
    mode=3;assert(udpfs_core_close(4)<0);
    mode=4;assert(udpfs_core_close(4)==-ENOSPC);
    assert(udpfs_core_close(-1)==-EBADF);
    connected=0;assert(udpfs_core_close(4)==-EIO);
    assert(requests==5);
    puts("PASS: UDPFS close reply validation, remote errors, disconnection");
}
""".replace("__FUNCTION__", function)
with tempfile.TemporaryDirectory() as directory:
    src=Path(directory)/"udpfs_close.c"
    exe=Path(directory)/"udpfs_close"
    src.write_text(test)
    subprocess.run([os.environ.get("CC","gcc"),"-std=gnu11","-O2","-Wall","-Wextra",str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

#!/usr/bin/env python3
"""Regression for UDPFS receive's transient stack-header ownership on disconnect."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/"ps2/iop/udpfs_ioman/src/udpfs_core.c").read_text()
start=source.index("static int _recv_with_result(")
opening=source.index("{",start)
depth,end=1,opening+1
while depth:
    depth+=(source[end]=="{")-(source[end]=="}")
    end+=1
function=source[start:end]
test=r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#define M_DEBUG(...) ((void)0)
#define UDPRDMA_OK 0
#define UDPFS_MSG_RESULT_REPLY 0x3f
typedef struct { int state; } udprdma_socket_t;
typedef struct {
    uint8_t msg_type,reserved[3];
    int32_t result;
} udpfs_msg_result_reply_t;
static void *rx_pointer, *header_pointer;
static int mode,buffer_clear_count,header_clear_count;
static void udprdma_set_rx_buffer(udprdma_socket_t *s,void *p,uint32_t n)
{
    (void)s;(void)n;rx_pointer=p;
    if(!p)buffer_clear_count++;
}
static void udprdma_set_rx_app_header(udprdma_socket_t *s,void *p,uint32_t n)
{
    (void)s;(void)n;header_pointer=p;
    if(!p)header_clear_count++;
}
static int udprdma_send(udprdma_socket_t *s,const void *p,uint32_t n)
{
    (void)s;(void)p;(void)n;return mode==1?-9:UDPRDMA_OK;
}
static int udprdma_recv(udprdma_socket_t *s,void *buf,uint32_t size,uint32_t timeout)
{
    (void)s;(void)buf;(void)timeout;
    if(mode==2) return -7; /* Disconnected before recv: leaves pointers set. */
    ((udpfs_msg_result_reply_t *)header_pointer)->msg_type=UDPFS_MSG_RESULT_REPLY;
    ((udpfs_msg_result_reply_t *)header_pointer)->result=(int32_t)size;
    /* Real recv clears both pointers after completion. */
    rx_pointer=header_pointer=NULL;
    return (int)size;
}
__FUNCTION__
int main(void)
{
    udprdma_socket_t socket={0};
    char data[32]={0};
    for(int v=1;v<=2;v++)
    {
        mode=v;
        int r=_recv_with_result(&socket,data,4,data,sizeof(data),100);
        assert(r<0);
        assert(rx_pointer==NULL && header_pointer==NULL);
    }
    assert(buffer_clear_count>=2 && header_clear_count>=2);
    mode=0;
    assert(_recv_with_result(&socket,data,4,data,sizeof(data),100)==sizeof(data));
    assert(rx_pointer==NULL && header_pointer==NULL);
    puts("PASS: UDPFS disconnect/send-failure clears transient RX pointers");
}
""".replace("__FUNCTION__",function)
with tempfile.TemporaryDirectory() as t:
    c=Path(t)/"udpfs_recv.c"
    exe=Path(t)/"udpfs_recv"
    c.write_text(test)
    subprocess.run([os.environ.get("CC","gcc"),"-std=gnu11","-O2","-Wall","-Wextra",str(c),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

#!/usr/bin/env python3
"""Compile the real GBI vertex loader with host mocks and exercise invalid bases."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
s = (ROOT / "ps2/src/renderer/gbi.c").read_text()
start = s.index("static void load_vertices(const Vtx *src, int n, int dst)")
opening = s.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (s[end] == "{") - (s[end] == "}")
    end += 1
function = s[start:end]

test = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define MAX_VTX 64
#define MAX_LIGHTS 8
#define G_LIGHTING 1u
#define G_TEXTURE_GEN 2u
#define G_FOG 4u
#define CLIP_NEAR 1
#define CLIP_GUARD 2
#define GUARD 6.0f

typedef struct {
    short ob[3], tc[2];
    unsigned char cn[4];
} Vtx_t;
typedef struct {
    short ob[3], tc[2];
    signed char n[3];
    unsigned char a;
} Vtx_tn;
typedef union {
    Vtx_t v;
    Vtx_tn n;
} Vtx;
typedef struct {
    float x,y,z,w,s,t;
    uint8_t r,g,b,a,clip,fog;
} GbiVtx;
static struct {
    int mvp_dirty, mv_top, lights_dirty, geom, num_lights, fog_mul, fog_off;
    float mv[16][4][4], proj[4][4], mvp[4][4];
    float light_col[MAX_LIGHTS+1][3], light_dir_model[MAX_LIGHTS][3];
    float lookat_model[2][3], tex_scale_s, tex_scale_t;
    uint8_t sentinel_before[16];
    GbiVtx vtx[MAX_VTX];
    uint8_t sentinel_after[16];
} R;

static uint8_t clamp_u8(float v)
{
    return (v <= 0.f) ? 0 : (v >= 255.f) ? 255 : (uint8_t)v;
}
static void mtx_mul(float out[4][4], const float a[4][4], const float b[4][4])
{
    (void)out; (void)a; (void)b;
}
static void update_model_lights(void) {}
__FUNCTION__

int main(void)
{
    Vtx src[70];
    memset(src,0,sizeof(src));
    memset(&R,0,sizeof(R));
    memset(R.sentinel_before,0x9B,sizeof(R.sentinel_before));
    memset(R.sentinel_after,0xA6,sizeof(R.sentinel_after));
    R.mvp[3][3]=1.f;
    R.tex_scale_s=R.tex_scale_t=1.f;
    src[0].v.cn[0]=22; src[0].v.cn[1]=33;
    /* A bad encoded end<count must not index before R.vtx[0]. */
    load_vertices(src,2,-1);
    load_vertices(src,2,-64);
    load_vertices(src,2,MAX_VTX);
    load_vertices(src,0,0);
    load_vertices(NULL,2,0);
    for(int i=0;i<MAX_VTX;i++) assert(R.vtx[i].r==0);
    for(int i=0;i<16;i++){
        assert(R.sentinel_before[i]==0x9B);
        assert(R.sentinel_after[i]==0xA6);
    }
    /* Partial terminal write is allowed and must not cross 64 vertices. */
    load_vertices(src,2,63);
    assert(R.vtx[63].r==22 && R.vtx[63].g==33);
    for(int i=0;i<16;i++) assert(R.sentinel_after[i]==0xA6);
    puts("PASS: GBI invalid VTX bases and terminal vertex bounds");
}
""".replace("__FUNCTION__", function)
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "test_gbi.c"
    exe = Path(tmp) / "test_gbi"
    src.write_text(test)
    subprocess.run([os.environ.get("CC","gcc"),"-std=gnu11","-O2","-Wall","-Wextra",
                    str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

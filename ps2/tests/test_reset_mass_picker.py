#!/usr/bin/env python3
"""Test reset-on volume recovery without guessing partition/USB identity.

Compiles SSB64's actual user-selected mount function with host IOP mocks,
checks each logical massN case, and verifies the screen path only runs after
a real ambiguous rebuilt mount. No actual PS2 PAD/GS hardware is emulated.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "ps2/src/storage/bootpath.c").read_text(encoding="utf-8")
assets = (root / "ps2/src/storage/assets.c").read_text(encoding="utf-8")
pad = (root / "ps2/src/input/pad.c").read_text(encoding="utf-8")


def extract(source, signature):
    at = source.index(signature)
    start = source.index("{", at)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[at:end]


func = extract(source, "int ps2_storage_select_rebuilt_mass_slot(")
harness = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define PATH_BUF_MAX 256
static char sDataDir[PATH_BUF_MAX];
static unsigned int sPackConflictMassMask;
static int sDataNeedsBdmResolve;
static int sLastPackProbeErrno;
static int rebuilt;
static int ps2_iop_bdm_was_rebuilt(void){return rebuilt;}
#define ps2_log(...) ((void)0)
__FUNC__
int main(void) {
    const unsigned int mask = 7u; /* two physical USBs, three logical mass units */
    for(unsigned int slot=0;slot<3;slot++) {
        strcpy(sDataDir,"mass0:/APPS/GME_ssb64/");
        sPackConflictMassMask = mask;
        sDataNeedsBdmResolve = rebuilt = 1;
        sLastPackProbeErrno = 17;
        assert(ps2_storage_select_rebuilt_mass_slot(slot));
        char expect[80];
        snprintf(expect,sizeof(expect),"mass%u:/APPS/GME_ssb64/",slot);
        assert(!strcmp(sDataDir,expect));
        assert(!sDataNeedsBdmResolve && !sPackConflictMassMask);
        assert(!sLastPackProbeErrno);
        assert(!ps2_storage_select_rebuilt_mass_slot(slot));
    }
    strcpy(sDataDir,"mass0:/SSB64/");
    sPackConflictMassMask=3;
    sDataNeedsBdmResolve=1; rebuilt=0;
    assert(!ps2_storage_select_rebuilt_mass_slot(1));
    rebuilt=1;
    assert(!ps2_storage_select_rebuilt_mass_slot(3));
    assert(!ps2_storage_select_rebuilt_mass_slot(4));
    assert(!strcmp(sDataDir,"mass0:/SSB64/"));
    sDataNeedsBdmResolve=0;
    assert(!ps2_storage_select_rebuilt_mass_slot(1));
    puts("PASS: 3 logical mass units, explicit choice, reset guard, unambiguous safety");
    return 0;
}
""".replace("__FUNC__",func)
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    path.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", str(path), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)

selector = extract(assets, "static int choose_ambiguous_mass_slot(")
assert "ps2_input_init();" in selector
assert "ps2_input_poll();" in selector
assert "N64_BTN_DU" in selector and "N64_BTN_DD" in selector
assert "N64_BTN_A" in selector and "N64_BTN_B" in selector
assert "ps2_storage_select_rebuilt_mass_slot(" in selector
assert "frames < 1800" in selector
assert "ps2_pkt_finish();" in selector
assert "ps2_gs_present_now(0);" in selector
assert "if (sInitDone)" in extract(pad, "void ps2_input_init(void)")
asset_init = extract(assets,"int ps2_assets_init(void)")
assert asset_init.index("ps2_storage_resolve_data_root(PACK_NAME)") < asset_init.index("choose_ambiguous_mass_slot(")
assert "continue; /* open exactly the chosen logical volume */" in asset_init
print("PASS: reset-on chooser is bounded, isolated from normal launches, and controller-selectable")

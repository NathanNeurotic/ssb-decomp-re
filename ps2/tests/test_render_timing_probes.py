#!/usr/bin/env python3
"""Keep renderer stall metrics separated without altering GIF/GS wait ordering.

Source-level coverage ensures the waits remain synchronous and their timers
are inside the actual DMA and GS wait functions, not accumulated elsewhere.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
gs = (root / "ps2/src/renderer/gs.c").read_text(encoding="utf-8")
render = (root / "ps2/src/renderer/render_thread.c").read_text(encoding="utf-8")
overlay = (root / "ps2/src/renderer/overlay.c").read_text(encoding="utf-8")
hdr = (root / "ps2/src/renderer/render.h").read_text(encoding="utf-8")


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


dma = function(gs, "static void dma_wait_gif(void)")
finish = function(gs, "void ps2_pkt_finish(void)")
work = function(render, "static void render_one(")
presentation = function(overlay, "void ps2_overlay_draw(void)")

assert dma.index("uint32_t started = ps2_time_us()") < dma.index("dmaKit_wait(DMA_CHANNEL_GIF, 0)") < dma.index("gPS2RenderStats.dma_wait_us +=")
assert dma.index("gPS2RenderStats.dma_wait_us +=") < dma.index("sPktInFlight = 0")
assert finish.index("pkt_kick()") < finish.index("dma_wait_gif()") < finish.index("while (!(*PGS_CSR & PGS_CSR_FINISH))")
assert finish.index("while (!(*PGS_CSR & PGS_CSR_FINISH))") < finish.index("gPS2RenderStats.gs_wait_us +=")
assert work.index("memset(&gPS2RenderStats, 0, sizeof(gPS2RenderStats))") < work.index("ps2_gbi_run(job->dl)")
assert work.index("ps2_gbi_run(job->dl)") < work.index("ps2_pkt_finish()") < work.index("gPS2RenderStats.gfx_us =")
assert "uint32_t dma_wait_us;" in hdr and "uint32_t gs_wait_us;" in hdr
assert "gfx_other = (r->gfx_us > gfx_wait) ? r->gfx_us - gfx_wait : 0" in presentation
assert '"io %uK wait %ums/s %s"' in presentation
print("PASS: timed GIF/GS wait accounting and runtime overlay preserve execution order")

// ROM -> SSB64.DAT, driven by the conversion manifest.
// Mirrors ps2/tools/build_assets.py main(); see README.md for the mapping.
import { readRelocTable, relocFileBytes, externIds, walkRelocs } from "./n64reloc.js";
import { applyOps } from "./layout.js";
import { layoutPack, writePackHeader } from "./pack.js";
import { buildSpuSamples, SPU_SAMPLES_VROM } from "./spu.js";

const BOOT_SIZE = 0x1000;

async function sha256Hex(bytes) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(d, (x) => x.toString(16).padStart(2, "0")).join("");
}

export class BuildError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

/**
 * rom: Uint8Array (.z64, verified). manifest: parsed manifest object.
 * progress(stage, fraction): optional. Returns Uint8Array (the DAT).
 */
export async function buildDat(rom, manifest, progress = () => {}) {
  if (manifest.format !== 1) throw new BuildError("MANIFEST", `unsupported manifest format ${manifest.format}`);
  const { count, vrom: relocVrom } = manifest.relocTable;
  const layouts = manifest.layouts;
  const table64 = readRelocTable(rom);
  let scratch = new Uint8Array(0x40000);
  const fileBytes = (fid) => {
    const need = table64[fid].dsize * 4;
    if (scratch.length < need) scratch = new Uint8Array(need);
    return relocFileBytes(rom, table64, fid, scratch);
  };

  // ---- 1. sizes and relocations ----
  progress("Processing relocations", 0);
  const files = [];
  for (let fid = 0; fid < count; fid++) {
    const m = manifest.relocFiles[fid];
    const src = fileBytes(fid);
    const relocs = walkRelocs(fid, src, table64[fid], externIds(rom, table64, fid));
    files.push({
      size: m.size !== undefined ? m.size : table64[fid].dsize * 4,
      intern: relocs.filter((r) => !r.ext).sort((a, b) => a.slot - b.slot),
      extern: relocs.filter((r) => r.ext).sort((a, b) => a.slot - b.slot),
    });
    if (fid % 64 === 0) progress("Processing relocations", fid / count);
  }

  // ---- 2. SPU samples (size needed for the layout) ----
  progress("Converting audio samples", 0);
  const spu = buildSpuSamples(rom, (i, n) => { if (i % 8 === 0) progress("Converting audio samples", i / n); });

  // ---- 3. region layout ----
  const tableBytes = 12 * (count + 1);
  const hi = relocVrom + tableBytes;
  const regions = [];
  const boot = { vrom: 0, size: BOOT_SIZE, resident: true };
  const tableRegion = { vrom: relocVrom, size: tableBytes, resident: true };
  regions.push(boot, tableRegion);
  let bodyOff = 0;
  for (const f of files) {
    f.dataOffset = bodyOff;
    f.region = { vrom: hi + bodyOff, size: f.size, resident: false };
    regions.push(f.region);
    bodyOff += f.size;
    if (f.extern.length) {
      f.extRegion = { vrom: hi + bodyOff, size: 2 * f.extern.length, resident: true };
      regions.push(f.extRegion);
      bodyOff += 2 * f.extern.length;
    }
  }
  const romRegions = manifest.regions.map((r) => ({ vrom: r.vrom, size: r.size, resident: false, ops: r.ops }));
  regions.push(...romRegions);
  const spuRegion = { vrom: SPU_SAMPLES_VROM, size: spu.length, resident: false };
  regions.push(spuRegion);

  const layout = layoutPack(regions);
  if (layout.totalSize !== manifest.output.size) {
    throw new BuildError("OUTPUT_SIZE", `layout gives ${layout.totalSize} bytes, expected ${manifest.output.size}`);
  }
  let out;
  try {
    out = new Uint8Array(layout.totalSize);
  } catch (e) {
    throw new BuildError("OUT_OF_MEMORY", String(e));
  }
  writePackHeader(out, layout, relocVrom, count);
  const view = (r) => out.subarray(r.fileOffset, r.fileOffset + r.size);

  // ---- 4. fill ----
  progress("Building asset pack", 0);
  view(boot).set(rom.subarray(0, BOOT_SIZE));

  const tv = new DataView(out.buffer, tableRegion.fileOffset, tableBytes);
  for (let fid = 0; fid < count; fid++) {
    const f = files[fid];
    const dst = view(f.region);
    applyOps(fileBytes(fid), dst, manifest.relocFiles[fid].ops, layouts);
    const dv = new DataView(dst.buffer, dst.byteOffset, dst.byteLength);
    const chain = (list) => {
      for (let i = 0; i < list.length; i++) {
        const r = list[i];
        if (r.slot % 4 || r.target % 4 || r.slot / 4 >= 0xffff || r.target / 4 >= 0xffff) {
          throw new BuildError("CONVERSION", `relocData ${fid}: pointer slot not word addressable`);
        }
        dv.setUint16(r.slot, i + 1 < list.length ? list[i + 1].slot / 4 : 0xffff, true);
        dv.setUint16(r.slot + 2, r.target / 4, true);
      }
      return list.length ? list[0].slot / 4 : 0xffff;
    };
    const internFirst = chain(f.intern);
    const externFirst = chain(f.extern);
    if (f.extRegion) {
      const ev = new DataView(out.buffer, f.extRegion.fileOffset, f.extRegion.size);
      f.extern.forEach((r, i) => ev.setUint16(2 * i, r.fid, true));
    }
    const words = f.size / 4;
    tv.setUint32(12 * fid, (f.dataOffset * 2) >>> 0, true); // (data_offset << 1) | is_compressed=0
    tv.setUint16(12 * fid + 4, internFirst, true);
    tv.setUint16(12 * fid + 6, words, true);
    tv.setUint16(12 * fid + 8, externFirst, true);
    tv.setUint16(12 * fid + 10, words, true);
    if (fid % 64 === 0) progress("Building asset pack", fid / count);
  }
  tv.setUint32(12 * count, (bodyOff * 2) >>> 0, true);
  tv.setUint16(12 * count + 4, 0xffff, true);
  tv.setUint16(12 * count + 6, 0, true);
  tv.setUint16(12 * count + 8, 0xffff, true);
  tv.setUint16(12 * count + 10, 0, true);

  for (const r of romRegions) applyOps(rom.subarray(r.vrom, r.vrom + r.size), view(r), r.ops, layouts);
  view(spuRegion).set(spu);

  // ---- 5. verify ----
  progress("Verifying output", 0);
  const got = await sha256Hex(out);
  if (got !== manifest.output.sha256) {
    const spuOk = (await sha256Hex(spu)) === manifest.output.spuSha256;
    throw new BuildError("OUTPUT_HASH", `output sha256 ${got} != ${manifest.output.sha256} (audio samples ${spuOk ? "ok" : "differ"})`);
  }
  progress("Complete", 1);
  return out;
}


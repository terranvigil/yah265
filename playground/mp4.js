// SPDX-License-Identifier: GPL-2.0-or-later
// Wraps the encoder's Annex-B stream in an MP4 (ISO BMFF, 'hvc1'). The
// parameter sets move into the hvcC box; every other NAL stays in its
// sample, length-prefixed. Timing comes from the encoder: each access
// unit's display index, and the reorder depth that dts trails pts by.
'use strict';

(function (root) {
  function nals(buf) {
    // start-code split: [offset, end) of each NAL payload, header included
    const out = [];
    let i = 0, start = -1;
    const n = buf.length;
    while (i + 2 < n) {
      if (buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 1) {
        if (start >= 0) {
          let end = i;
          while (end > start && buf[end - 1] === 0) end--;
          out.push([start, end]);
        }
        i += 3;
        start = i;
      } else i++;
    }
    if (start >= 0) out.push([start, n]);
    return out;
  }

  function unescape(b) {
    const out = [];
    for (let i = 0; i < b.length; i++) {
      if (i >= 2 && b[i] === 3 && b[i - 1] === 0 && b[i - 2] === 0) continue;
      out.push(b[i]);
    }
    return new Uint8Array(out);
  }

  function bits(b) {
    let pos = 0;
    const u = (n) => {
      let v = 0;
      for (let k = 0; k < n; k++) {
        v = v * 2 + ((b[pos >> 3] >> (7 - (pos & 7))) & 1);
        pos++;
      }
      return v;
    };
    const ue = () => {
      let z = 0;
      while (u(1) === 0) z++;
      return z ? (2 ** z - 1) + u(z) : 0;
    };
    return { u, ue, skip: (n) => { pos += n; }, get pos() { return pos; } };
  }

  // what hvcC repeats from the SPS
  function parseSps(nal) {
    const b = unescape(nal.subarray(2));
    const r = bits(b);
    r.u(4);
    const maxSub = r.u(3);
    r.u(1);
    const ptl = b.slice(1, 13);
    r.skip(96);
    const subProfile = [], subLevel = [];
    for (let i = 0; i < maxSub; i++) { subProfile.push(r.u(1)); subLevel.push(r.u(1)); }
    if (maxSub > 0) for (let i = maxSub; i < 8; i++) r.u(2);
    for (let i = 0; i < maxSub; i++) {
      if (subProfile[i]) r.skip(88);
      if (subLevel[i]) r.skip(8);
    }
    r.ue();
    const chroma = r.ue();
    if (chroma === 3) r.u(1);
    r.ue(); r.ue();
    if (r.u(1)) { r.ue(); r.ue(); r.ue(); r.ue(); }
    const bdl = r.ue(), bdc = r.ue();
    return { ptl, chroma, bdl, bdc, maxSub, nesting: (b[0] & 1) };
  }

  class W {
    constructor() { this.parts = []; this.len = 0; }
    u8(v) { this.bytes([v & 255]); }
    u16(v) { this.bytes([(v >> 8) & 255, v & 255]); }
    u32(v) { this.bytes([(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]); }
    i32(v) { this.u32(v >>> 0); }
    str(s) { this.bytes([...s].map((c) => c.charCodeAt(0))); }
    bytes(a) { const u = a instanceof Uint8Array ? a : Uint8Array.from(a); this.parts.push(u); this.len += u.length; }
    zero(n) { this.bytes(new Uint8Array(n)); }
    out() {
      const o = new Uint8Array(this.len);
      let p = 0;
      for (const u of this.parts) { o.set(u, p); p += u.length; }
      return o;
    }
  }

  function box(type, ...kids) {
    const w = new W();
    let n = 8;
    for (const k of kids) n += k.length;
    w.u32(n); w.str(type);
    for (const k of kids) w.bytes(k);
    return w.out();
  }
  function full(type, ver, flags, body, ...kids) {
    const w = new W();
    w.u8(ver); w.u8(flags >> 16); w.u16(flags & 0xffff);
    w.bytes(body);
    return box(type, w.out(), ...kids);
  }
  function body(fn) { const w = new W(); fn(w); return w.out(); }
  const MATRIX = [0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000];

  // hevc: Uint8Array Annex-B; aus: Int32Array (display, bytes) pairs in
  // coding order; reorder: frames dts trails pts by
  function hevcToMp4(hevc, aus, reorder, width, height, fpsNum, fpsDen) {
    const all = nals(hevc);
    const ps = { 32: [], 33: [], 34: [] };
    // assign NALs to access units by byte offset
    const samples = [];
    let ni = 0, off = 0;
    for (let k = 0; k < aus.length; k += 2) {
      const disp = aus[k], end = off + aus[k + 1];
      const w = new W();
      let key = false;
      while (ni < all.length && all[ni][0] < end) {
        const [s, e] = all[ni++];
        const t = (hevc[s] >> 1) & 63;
        if (t >= 32 && t <= 34) {
          if (!ps[t].length) ps[t].push(hevc.subarray(s, e));
          continue;
        }
        if (t >= 16 && t <= 21) key = true;
        w.u32(e - s); w.bytes(hevc.subarray(s, e));
      }
      off = end;
      const data = w.out();
      if (disp < 0) {
        // a tail with no picture (end of sequence): it rides the last sample
        if (samples.length && data.length) {
          const last = samples[samples.length - 1];
          const m = new Uint8Array(last.data.length + data.length);
          m.set(last.data); m.set(data, last.data.length);
          last.data = m;
        }
        continue;
      }
      samples.push({ disp, data, key });
    }
    if (!ps[33].length || !samples.length) throw new Error('no SPS or no pictures in the stream');
    const sps = parseSps(ps[33][0]);

    const hvcC = box('hvcC', body((w) => {
      w.u8(1);
      w.bytes(sps.ptl.subarray(0, 1));     // profile space, tier, profile
      w.bytes(sps.ptl.subarray(1, 5));     // compatibility flags
      w.bytes(sps.ptl.subarray(5, 11));    // constraint flags
      w.u8(sps.ptl[11]);                   // level
      w.u16(0xf000);                       // min_spatial_segmentation 0
      w.u8(0xfc);                          // parallelism unknown
      w.u8(0xfc | sps.chroma);
      w.u8(0xf8 | sps.bdl);
      w.u8(0xf8 | sps.bdc);
      w.u16(0);                            // avg frame rate unknown
      w.u8(((sps.maxSub + 1) << 3) | (sps.nesting << 2) | 3);
      const arrays = [32, 33, 34].filter((t) => ps[t].length);
      w.u8(arrays.length);
      for (const t of arrays) {
        w.u8(0x80 | t);
        w.u16(ps[t].length);
        for (const p of ps[t]) { w.u16(p.length); w.bytes(p); }
      }
    }));
    const hvc1 = box('hvc1', body((w) => {
      w.zero(6); w.u16(1);                 // reserved, data_reference_index
      w.zero(16);
      w.u16(width); w.u16(height);
      w.u32(0x00480000); w.u32(0x00480000);
      w.u32(0); w.u16(1);
      w.u8(0); w.zero(31);                 // compressor name
      w.u16(0x18); w.u16(0xffff);
    }), hvcC);

    const n = samples.length;
    const dur = fpsDen;                    // one frame in the timescale fpsNum
    const total = n * dur;
    const delay = reorder * dur;
    const stts = full('stts', 0, 0, body((w) => { w.u32(1); w.u32(n); w.u32(dur); }));
    const ctts = full('ctts', 0, 0, body((w) => {
      w.u32(n);
      samples.forEach((s, i) => { w.u32(1); w.u32((s.disp - i + reorder) * dur); });
    }));
    const keys = samples.map((s, i) => (s.key ? i + 1 : 0)).filter(Boolean);
    const stss = full('stss', 0, 0, body((w) => { w.u32(keys.length); keys.forEach((k) => w.u32(k)); }));
    const stsz = full('stsz', 0, 0, body((w) => { w.u32(0); w.u32(n); samples.forEach((s) => w.u32(s.data.length)); }));
    const stsc = full('stsc', 0, 0, body((w) => { w.u32(1); w.u32(1); w.u32(n); w.u32(1); }));
    const stsd = full('stsd', 0, 0, body((w) => w.u32(1)), hvc1);

    const ftyp = box('ftyp', body((w) => { w.str('isom'); w.u32(512); w.str('isomiso2hvc1mp41'); }));
    const build = (mdatOff) => {
      const stco = full('stco', 0, 0, body((w) => { w.u32(1); w.u32(mdatOff); }));
      const stbl = box('stbl', stsd, stts, ctts, stss, stsz, stsc, stco);
      const dinf = box('dinf', full('dref', 0, 0, body((w) => w.u32(1)), full('url ', 0, 1, new Uint8Array(0))));
      const minf = box('minf', full('vmhd', 0, 1, new Uint8Array(8)), dinf, stbl);
      const hdlr = full('hdlr', 0, 0, body((w) => { w.u32(0); w.str('vide'); w.zero(12); w.str('yah265'); w.u8(0); }));
      const mdhd = full('mdhd', 0, 0, body((w) => { w.u32(0); w.u32(0); w.u32(fpsNum); w.u32(total); w.u16(0x55c4); w.u16(0); }));
      const mdia = box('mdia', mdhd, hdlr, minf);
      // the edit list hides the reorder delay, so the first picture shows at 0
      const elst = full('elst', 0, 0, body((w) => { w.u32(1); w.u32(Math.round(total * 1000 / fpsNum)); w.u32(delay); w.u16(1); w.u16(0); }));
      const tkhd = full('tkhd', 0, 3, body((w) => {
        w.u32(0); w.u32(0); w.u32(1); w.u32(0); w.u32(Math.round(total * 1000 / fpsNum));
        w.zero(8); w.u16(0); w.u16(0); w.u16(0); w.u16(0);
        MATRIX.forEach((m) => w.u32(m));
        w.u32(width * 65536); w.u32(height * 65536);
      }));
      const trak = box('trak', tkhd, box('edts', elst), mdia);
      const mvhd = full('mvhd', 0, 0, body((w) => {
        w.u32(0); w.u32(0); w.u32(1000); w.u32(Math.round(total * 1000 / fpsNum));
        w.u32(0x00010000); w.u16(0x0100); w.zero(10);
        MATRIX.forEach((m) => w.u32(m));
        w.zero(24); w.u32(2);
      }));
      return box('moov', mvhd, trak);
    };
    // moov goes first (fast start); its size doesn't depend on the offset
    const probe = build(0);
    const mdatOff = ftyp.length + probe.length + 8;
    const moov = build(mdatOff);
    let payload = 0;
    for (const s of samples) payload += s.data.length;
    const out = new Uint8Array(ftyp.length + moov.length + 8 + payload);
    let p = 0;
    out.set(ftyp, p); p += ftyp.length;
    out.set(moov, p); p += moov.length;
    const mh = new W(); mh.u32(8 + payload); mh.str('mdat');
    out.set(mh.out(), p); p += 8;
    for (const s of samples) { out.set(s.data, p); p += s.data.length; }
    return out;
  }

  root.hevcToMp4 = hevcToMp4;
  if (typeof module !== 'undefined') module.exports = { hevcToMp4 };
})(typeof self !== 'undefined' ? self : this);

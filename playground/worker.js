// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the encoder off the page's thread. Protocol, page -> worker:
//   {cmd: 'open', width, height, fpsNum, fpsDen, opts}
//   {cmd: 'frame', data: ArrayBuffer (I420), pts}
//   {cmd: 'finish'}
// worker -> page:
//   {type: 'ready', version} once the module has loaded
//   {type: 'opened'} | {type: 'progress', frames, bytes, encodeMs}
//   {type: 'done', hevc, recon, bytes, encodeMs, frames} | {type: 'error', message}
'use strict';

importScripts('yah265.js');

let mod = null;
let api = null;
let st = null;
const queue = [];

function fail(message) {
  postMessage({ type: 'error', message });
}

createYah265({
  locateFile: (p) => p,
  print: () => {},
  printErr: (t) => { if (st) st.log.push(t); },
}).then((m) => {
  mod = m;
  api = {
    open: m.cwrap('pg_open', 'number', ['number', 'number', 'number', 'number', 'string']),
    frame: m.cwrap('pg_frame', 'number', []),
    encode: m.cwrap('pg_encode', 'number', ['number']),
    flush: m.cwrap('pg_flush', 'number', []),
    close: m.cwrap('pg_close', 'number', []),
    version: m.cwrap('pg_version', 'string', []),
  };
  postMessage({ type: 'ready', version: api.version() });
  while (queue.length) handle(queue.shift());
}, (e) => fail('could not load the encoder: ' + e));

function handle(msg) {
  try {
    if (msg.cmd === 'open') {
      st = { w: msg.width, h: msg.height, frames: 0, bytes: 0, ms: 0, log: [] };
      const t0 = performance.now();
      const r = api.open(msg.width, msg.height, msg.fpsNum, msg.fpsDen, msg.opts);
      st.ms += performance.now() - t0;
      if (r < 0) {
        fail('the encoder refused these settings. ' + st.log.join(' '));
        st = null;
        return;
      }
      postMessage({ type: 'opened' });
    } else if (msg.cmd === 'frame') {
      if (!st) return;
      const src = new Uint8Array(msg.data);
      // read the pointer every time: a heap grow moves HEAPU8
      mod.HEAPU8.set(src, api.frame());
      const t0 = performance.now();
      const r = api.encode(msg.pts);
      st.ms += performance.now() - t0;
      if (r < 0) throw new Error('encode failed at frame ' + st.frames);
      st.frames++;
      st.bytes += r;
      postMessage({ type: 'progress', frames: st.frames, bytes: st.bytes, encodeMs: st.ms });
    } else if (msg.cmd === 'finish') {
      if (!st) return;
      const t0 = performance.now();
      for (;;) {
        const r = api.flush();
        if (r < 0) throw new Error('flush failed');
        if (r === 0) break;
        st.bytes += r;
        postMessage({ type: 'progress', frames: st.frames, bytes: st.bytes, encodeMs: st.ms + performance.now() - t0 });
      }
      api.close();
      st.ms += performance.now() - t0;
      const hevc = mod.FS.readFile('/out.hevc');
      const recon = mod.FS.readFile('/recon.yuv');
      mod.FS.unlink('/out.hevc');
      mod.FS.unlink('/recon.yuv');
      postMessage({
        type: 'done', hevc: hevc.buffer, recon: recon.buffer,
        bytes: hevc.length, encodeMs: st.ms, frames: st.frames,
      }, [hevc.buffer, recon.buffer]);
      st = null;
    }
  } catch (e) {
    st = null;
    fail(String(e && e.message || e));
  }
}

onmessage = (e) => {
  if (!api) queue.push(e.data);
  else handle(e.data);
};

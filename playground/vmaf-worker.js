// SPDX-License-Identifier: GPL-2.0-or-later
// Scores VMAF v1 off the page's thread. Protocol, page -> worker:
//   {cmd: 'open', w, h, ow, oh}   coded size, and the size it is scored at
//   {cmd: 'pair', ref, dis}       two I420 frames (ArrayBuffers), in order
//   {cmd: 'score'}
// worker -> page:
//   {type: 'ready'} | {type: 'pushed', n} | {type: 'score', value} | {type: 'error', message}
'use strict';

importScripts('vmaf.js');

let m = null, n = 0;
const queue = [];

createVmaf({ locateFile: (p) => p, print: () => {}, printErr: () => {} }).then((mod) => {
  m = mod;
  postMessage({ type: 'ready' });
  while (queue.length) handle(queue.shift());
}, (e) => postMessage({ type: 'error', message: 'could not load VMAF: ' + e }));

function handle(msg) {
  try {
    if (msg.cmd === 'open') {
      n = 0;
      const open = m.cwrap('vw_open', 'number', ['number', 'number', 'number', 'number', 'string']);
      const r = open(msg.w, msg.h, msg.ow, msg.oh, '/v1.json');
      if (r < 0) throw new Error('VMAF setup failed (' + r + ')');
    } else if (msg.cmd === 'pair') {
      m.HEAPU8.set(new Uint8Array(msg.ref), m._vw_ref());
      m.HEAPU8.set(new Uint8Array(msg.dis), m._vw_dis());
      if (m._vw_push() < 0) throw new Error('VMAF failed on frame ' + n);
      postMessage({ type: 'pushed', n: ++n });
    } else if (msg.cmd === 'score') {
      const v = m._vw_score();
      m._vw_close();
      if (v < 0) throw new Error('VMAF pooling failed');
      postMessage({ type: 'score', value: v });
    }
  } catch (e) {
    postMessage({ type: 'error', message: String(e && e.message || e) });
  }
}

onmessage = (e) => {
  if (!m) queue.push(e.data);
  else handle(e.data);
};

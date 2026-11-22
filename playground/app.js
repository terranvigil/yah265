// SPDX-License-Identifier: GPL-2.0-or-later
// The playground page: turn a clip into I420 frames, feed the encoder
// worker, score the result with VMAF v1 in a second worker, and show the
// source beside the encoder's reconstruction.
'use strict';

const MAX_W = 1280, MAX_H = 720, MAX_SECONDS = 10, MAX_FPS = 30;
// the page holds the source and the reconstruction in memory: 10 s at 480p
const PIXEL_BUDGET = 854 * 480 * 300;
// frames read ahead of the encoder before the reader waits
const AHEAD = 8;
const SAMPLE = 'samples/bbb-480p.mp4';

const $ = (id) => document.getElementById(id);
const ui = {
  drop: $('drop'), file: $('file'), sample: $('sample'), change: $('change'),
  dropEmpty: $('drop-empty'), dropClip: $('drop-clip'), clipName: $('clip-name'), clipMeta: $('clip-meta'),
  preset: $('preset'), crf: $('crf'), crfval: $('crfval'), go: $('go'),
  track: $('track'), bar: $('bar'), msg: $('msg'),
  stage: $('stage'), empty: $('empty'), view: $('view'), split: $('split'),
  transport: $('transport'), play: $('play'), prev: $('prev'), next: $('next'),
  seek: $('seek'), fno: $('fno'), result: $('result'), caption: $('caption'),
  dl: $('dl'), dlMp4: $('dl-mp4'), vmaf: $('s-vmaf'),
};

// what a test harness reads
window.pg = { ready: false, done: false, error: null, stats: null, vmaf: null };

let worker = null;
let clip = null;            // {kind, file, name}
let job = null;             // the encode in flight
let shown = null;           // {w, h, fps, src: [Uint8Array], rec: Uint8Array, n}
let vmafJob = 0;            // bumps to drop a stale score

function say(text) { ui.msg.textContent = text; }
function progress(f) {
  ui.track.hidden = f === null;
  if (f !== null) ui.bar.style.width = (100 * Math.max(0, Math.min(1, f))) + '%';
}
function setBusy(busy) {
  ui.go.textContent = busy ? 'Cancel' : 'Encode';
  ui.go.classList.toggle('primary', !busy);
  ui.go.disabled = !busy && (!clip || !window.pg.ready);
}
function fail(text) {
  window.pg.error = text;
  if (job && job.src && job.src.close) job.src.close();
  job = null;
  progress(null);
  say(text);
  setBusy(false);
}

function startWorker() {
  worker = new Worker('worker.js');
  worker.onmessage = (e) => onWorker(e.data);
  worker.onerror = (e) => fail('The encoder failed to load: ' + (e.message || 'unknown error'));
}

function onWorker(m) {
  if (m.type === 'ready') {
    window.pg.ready = true;
    window.pg.version = m.version;
    if (!job) say(clip ? 'Ready. Press Encode.' : 'Ready. Choose a clip to start.');
    setBusy(!!job);
  } else if (m.type === 'progress' && job) {
    job.encoded = m.frames;
    if (job.wake) { const w = job.wake; job.wake = null; w(); }
    progress(job.total ? m.frames / job.total : 0);
    const fps = m.encodeMs > 0 ? m.frames / (m.encodeMs / 1000) : 0;
    const left = fps > 0 ? Math.ceil((job.total - m.frames) / fps) : 0;
    say('Encoding frame ' + m.frames + ' of ' + job.total +
        (fps > 0 ? ', ' + fps.toFixed(1) + ' fps' : '') +
        (left > 0 ? ', about ' + left + ' s left' : '') + '.');
  } else if (m.type === 'done' && job) {
    finish(m);
  } else if (m.type === 'error') {
    fail('Error: ' + m.message);
  }
}

// ---- colour conversion, BT.601 limited range ----

function rgbaToI420(rgba, w, h, out) {
  const cw = w >> 1, ys = 0, us = w * h, vs = us + cw * (h >> 1);
  for (let y = 0; y < h; y++) {
    let p = y * w * 4, o = ys + y * w;
    for (let x = 0; x < w; x++, p += 4)
      out[o + x] = (16.5 + (65.738 * rgba[p] + 129.057 * rgba[p + 1] + 25.064 * rgba[p + 2]) / 256) | 0;
  }
  for (let y = 0; y < h; y += 2) {
    for (let x = 0; x < w; x += 2) {
      const a = (y * w + x) * 4, b = a + w * 4;
      const r = (rgba[a] + rgba[a + 4] + rgba[b] + rgba[b + 4]) / 4;
      const g = (rgba[a + 1] + rgba[a + 5] + rgba[b + 1] + rgba[b + 5]) / 4;
      const bl = (rgba[a + 2] + rgba[a + 6] + rgba[b + 2] + rgba[b + 6]) / 4;
      const c = (y >> 1) * cw + (x >> 1);
      out[us + c] = (128.5 + (-37.945 * r - 74.494 * g + 112.439 * bl) / 256) | 0;
      out[vs + c] = (128.5 + (112.439 * r - 94.154 * g - 18.285 * bl) / 256) | 0;
    }
  }
  return out;
}

function i420ToRgba(src, off, w, h, img) {
  const d = img.data, cw = w >> 1, us = off + w * h, vs = us + cw * (h >> 1);
  for (let y = 0; y < h; y++) {
    const crow = (y >> 1) * cw;
    for (let x = 0; x < w; x++) {
      const c = 1.164 * (src[off + y * w + x] - 16);
      const u = src[us + crow + (x >> 1)] - 128, v = src[vs + crow + (x >> 1)] - 128;
      const p = (y * w + x) * 4;
      d[p] = c + 1.596 * v;
      d[p + 1] = c - 0.392 * u - 0.813 * v;
      d[p + 2] = c + 2.017 * u;
      d[p + 3] = 255;
    }
  }
  return img;
}


function fitSize(w, h) {
  const s = Math.min(1, MAX_W / w, MAX_H / h);
  return { w: Math.max(2, Math.round(w * s / 2) * 2), h: Math.max(2, Math.round(h * s / 2) * 2) };
}

// a y4m reader: header, then FRAME lines; 4:2:0 8-bit only. It reads one
// frame at a time, so a large file isn't loaded whole.
async function y4mSource(file) {
  const headBuf = new Uint8Array(await file.slice(0, 4096).arrayBuffer());
  const nl = headBuf.indexOf(10);
  if (nl < 0) throw new Error('not a y4m file');
  const head = new TextDecoder().decode(headBuf.subarray(0, nl)).split(' ');
  if (head[0] !== 'YUV4MPEG2') throw new Error('not a y4m file');
  let w = 0, h = 0, fn = 25, fd = 1, cs = '420';
  for (const t of head.slice(1)) {
    if (t[0] === 'W') w = +t.slice(1);
    else if (t[0] === 'H') h = +t.slice(1);
    else if (t[0] === 'F') [fn, fd] = t.slice(1).split(':').map(Number);
    else if (t[0] === 'C') cs = t.slice(1);
  }
  if (!cs.startsWith('420') || cs.includes('p1')) throw new Error('only 4:2:0 8-bit y4m is supported (this one is C' + cs + ')');
  if (w & 1 || h & 1) throw new Error('y4m width and height must be even');
  const fsz = w * h * 3 / 2;
  // every FRAME line is plain ("FRAME\n"), so frames sit at a fixed stride
  const first = nl + 1, stride = 6 + fsz;
  const count = Math.floor((file.size - first) / stride);
  let fps = fn / fd, step = 1;
  while (fps / step > MAX_FPS + 0.01) step++;
  const out = fitSize(w, h);
  const total = Math.min(Math.floor(count / step), Math.floor(MAX_SECONDS * fps / step),
                         Math.floor(PIXEL_BUDGET / (out.w * out.h)));
  if (total < 1) throw new Error('no frames found');
  let scaler = null;
  if (out.w !== w || out.h !== h) {
    const a = document.createElement('canvas'); a.width = w; a.height = h;
    const b = document.createElement('canvas'); b.width = out.w; b.height = out.h;
    scaler = { a, b, ax: a.getContext('2d'), bx: b.getContext('2d', { willReadFrequently: true }), img: new ImageData(w, h) };
  }
  return {
    w: out.w, h: out.h, fpsNum: fn, fpsDen: fd * step, total,
    async frame(i) {
      const off = first + i * step * stride;
      const b = new Uint8Array(await file.slice(off, off + stride).arrayBuffer());
      if (b[0] !== 0x46) throw new Error('unexpected FRAME header');   // 'F'
      const f = b.subarray(6);
      if (!scaler) return f.slice();
      scaler.ax.putImageData(i420ToRgba(f, 0, w, h, scaler.img), 0, 0);
      scaler.bx.drawImage(scaler.a, 0, 0, out.w, out.h);
      const px = scaler.bx.getImageData(0, 0, out.w, out.h).data;
      return rgbaToI420(px, out.w, out.h, new Uint8Array(out.w * out.h * 3 / 2));
    },
  };
}

function once(el, ev) { return new Promise((res) => el.addEventListener(ev, res, { once: true })); }

// the frame rate is not exposed, so it is measured from presented frames
async function measureFps(video) {
  if (!('requestVideoFrameCallback' in HTMLVideoElement.prototype)) return 30;
  const times = [];
  await new Promise((res) => {
    const timer = setTimeout(res, 1500);
    const cb = (now, meta) => {
      times.push(meta.mediaTime);
      if (times.length >= 12) { clearTimeout(timer); res(); } else video.requestVideoFrameCallback(cb);
    };
    video.requestVideoFrameCallback(cb);
    video.play().catch(() => { clearTimeout(timer); res(); });
  });
  video.pause();
  const d = [];
  for (let i = 1; i < times.length; i++) if (times[i] > times[i - 1]) d.push(times[i] - times[i - 1]);
  if (d.length < 4) return 30;
  d.sort((a, b) => a - b);
  const fps = 1 / d[d.length >> 1];
  return fps > 1 && fps < 121 ? fps : 30;
}

async function videoSource(file) {
  const video = document.createElement('video');
  video.muted = true;
  video.playsInline = true;
  video.preload = 'auto';
  video.src = URL.createObjectURL(file);
  video.style.cssText = 'position:fixed;left:0;top:0;width:2px;height:2px;opacity:0.01;pointer-events:none';
  document.body.appendChild(video);
  const drop = () => { video.pause(); video.remove(); URL.revokeObjectURL(video.src); };
  let fps;
  try {
    await Promise.race([
      once(video, 'loadeddata'),
      once(video, 'error').then(() => { throw new Error('this browser cannot decode that file'); }),
    ]);
    if (!video.videoWidth) throw new Error('no video track found');
    fps = await measureFps(video);
  } catch (e) {
    drop();
    throw e;
  }
  // standard rates read better as fractions than as a rounded measurement
  const known = [[24000, 1001], [24, 1], [25, 1], [30000, 1001], [30, 1], [50, 1], [60000, 1001], [60, 1]];
  let best = [Math.round(fps), 1];
  for (const k of known) if (Math.abs(k[0] / k[1] - fps) < Math.abs(best[0] / best[1] - fps)) best = k;
  let [fn, fd] = best;
  while (fn / fd > MAX_FPS + 0.01) fd *= 2;
  fps = fn / fd;
  const out = fitSize(video.videoWidth, video.videoHeight);
  const canvas = document.createElement('canvas');
  canvas.width = out.w; canvas.height = out.h;
  const cx = canvas.getContext('2d', { willReadFrequently: true });
  const dur = isFinite(video.duration) ? video.duration : MAX_SECONDS;
  const total = Math.max(1, Math.min(Math.floor(Math.min(dur, MAX_SECONDS) * fps),
                                    Math.floor(PIXEL_BUDGET / (out.w * out.h))));
  const grab = () => {
    cx.drawImage(video, 0, 0, out.w, out.h);
    const px = cx.getImageData(0, 0, out.w, out.h).data;
    return rgbaToI420(px, out.w, out.h, new Uint8Array(out.w * out.h * 3 / 2));
  };

  // Frames are captured while the clip plays, because a seek decodes from
  // the last key frame and seeking every frame took ~0.2 s a frame. A
  // frame playback skipped is seeked to once playback is over.
  const got = new Map();
  let played = false, wake = null;
  const poke = () => { if (wake) { const w = wake; wake = null; w(); } };
  const finishPlay = () => { if (!played) { played = true; video.pause(); poke(); } };
  if ('requestVideoFrameCallback' in video) {
    const cb = (now, meta) => {
      const i = Math.round(meta.mediaTime * fps);
      if (i < total && !got.has(i)) got.set(i, grab());
      if (i >= total - 1) finishPlay();
      else video.requestVideoFrameCallback(cb);
      poke();
    };
    video.addEventListener('ended', finishPlay);
    if (video.currentTime !== 0) {
      const seeked = once(video, 'seeked');
      video.currentTime = 0;
      await seeked;
    }
    // already on screen, so no callback will present it
    got.set(0, grab());
    video.requestVideoFrameCallback(cb);
    video.play().catch(finishPlay);
  } else {
    played = true;
  }
  return {
    w: out.w, h: out.h, fpsNum: fn, fpsDen: fd, total,
    async frame(i) {
      while (!got.has(i) && !played) await new Promise((res) => { wake = res; });
      if (got.has(i)) {
        const f = got.get(i);
        got.delete(i);
        return f;
      }
      const t = (i + 0.5) / fps;
      if (Math.abs(video.currentTime - t) > 1e-4) {
        const seeked = once(video, 'seeked');
        video.currentTime = t;
        await seeked;
      }
      return grab();
    },
    close: drop,
  };
}

// ---- the encode ----

function presetValue() {
  const b = ui.preset.querySelector('[aria-pressed=true]');
  return b ? b.dataset.v : 'medium';
}

async function encode() {
  if (!clip || job || !window.pg.ready) return;
  stopPlay();
  vmafJob++;
  window.pg.done = false;
  window.pg.error = null;
  window.pg.vmaf = null;
  progress(0);
  say('Reading the clip...');
  const opts = 'preset=' + presetValue() + ':crf=' + ui.crf.value;
  job = { src: null, total: 0, decoded: 0, encoded: 0, frames: [], opts, t0: performance.now(), wake: null };
  const me = job;
  setBusy(true);
  let src;
  try {
    src = clip.kind === 'y4m' ? await y4mSource(clip.file) : await videoSource(clip.file);
  } catch (e) {
    if (job === me) fail('Could not read the clip: ' + e.message);
    return;
  }
  if (job !== me) { if (src.close) src.close(); return; }
  job.src = src;
  job.total = src.total;
  ui.clipMeta.textContent = src.w + 'x' + src.h + ', ' + src.total + ' frames at ' +
    (src.fpsNum / src.fpsDen).toFixed(2).replace(/\.00$/, '') + ' fps';
  worker.postMessage({ cmd: 'open', width: src.w, height: src.h, fpsNum: src.fpsNum, fpsDen: src.fpsDen, opts });
  try {
    for (let i = 0; i < src.total; i++) {
      // hold the reader a few frames ahead, so the worker's queue stays short
      while (job === me && job.decoded - job.encoded > AHEAD) await new Promise((res) => { me.wake = res; });
      if (job !== me) return;
      const f = await src.frame(i);
      if (job !== me) return;
      job.frames.push(f);
      job.decoded = i + 1;
      const copy = f.slice();
      worker.postMessage({ cmd: 'frame', data: copy.buffer, pts: i }, [copy.buffer]);
    }
  } catch (e) {
    if (job === me) fail('Could not read frame ' + (me.decoded + 1) + ': ' + e.message);
    return;
  }
  if (src.close) src.close();
  job.readMs = performance.now() - job.t0;
  say('Finishing the encode...');
  worker.postMessage({ cmd: 'finish' });
}

function cancel() {
  if (!job) return;
  worker.terminate();
  if (job.src && job.src.close) job.src.close();
  if (job.wake) job.wake();
  job = null;
  window.pg.ready = false;
  progress(null);
  say('Cancelled.');
  setBusy(false);
  startWorker();
}

function psnrY(src, rec, w, h) {
  const n = w * h, fsz = n * 3 / 2;
  let sse = 0;
  for (let i = 0; i < src.length; i++) {
    const a = src[i], off = i * fsz;
    for (let k = 0; k < n; k++) { const d = a[k] - rec[off + k]; sse += d * d; }
  }
  const mse = sse / (n * src.length);
  return mse > 0 ? 10 * Math.log10(255 * 255 / mse) : Infinity;
}

function fmtBytes(b) {
  return b < 1024 ? b + ' B' : b < 1048576 ? (b / 1024).toFixed(1) + ' KB' : (b / 1048576).toFixed(2) + ' MB';
}

function setUrl(a, blob) {
  if (a.href && a.href.startsWith('blob:')) URL.revokeObjectURL(a.href);
  a.href = URL.createObjectURL(blob);
}

function finish(m) {
  const src = job.src;
  const fps = src.fpsNum / src.fpsDen;
  const rec = new Uint8Array(m.recon);
  const secs = m.frames / fps;
  const psnr = psnrY(job.frames, rec, src.w, src.h);
  const preset = presetValue();
  const crf = ui.crf.value;
  const stats = {
    width: src.w, height: src.h, frames: m.frames, fps,
    bytes: m.bytes, kbps: m.bytes * 8 / 1000 / secs, psnrY: psnr,
    encodeMs: m.encodeMs, encodeFps: m.frames / (m.encodeMs / 1000),
    wallMs: performance.now() - job.t0, readMs: job.readMs, opts: job.opts,
  };
  const base = clip.name.replace(/\.[^.]+$/, '').replace(/[^\w.-]+/g, '_') + '-' + preset + '-crf' + crf;
  ui.dl.download = base + '.hevc';
  ui.dlMp4.download = base + '.mp4';
  setUrl(ui.dl, new Blob([m.hevc], { type: 'video/hevc' }));
  try {
    const mp4 = hevcToMp4(new Uint8Array(m.hevc), new Int32Array(m.aus), m.reorder,
                          src.w, src.h, src.fpsNum, src.fpsDen);
    setUrl(ui.dlMp4, new Blob([mp4], { type: 'video/mp4' }));
    ui.dlMp4.hidden = false;
  } catch (e) {
    ui.dlMp4.hidden = true;
  }
  $('s-size').textContent = fmtBytes(m.bytes);
  $('s-rate').textContent = stats.kbps.toFixed(0) + ' kbit/s';
  $('s-psnr').textContent = isFinite(psnr) ? psnr.toFixed(2) + ' dB' : 'lossless';
  $('s-time').textContent = (m.encodeMs / 1000).toFixed(1) + ' s';
  $('s-fps').textContent = stats.encodeFps.toFixed(1) + ' fps';
  ui.caption.textContent = src.w + 'x' + src.h + ', ' + m.frames + ' frames at ' + fps.toFixed(2).replace(/\.00$/, '') +
    ' fps, preset ' + preset + ', CRF ' + crf + '.';
  shown = { w: src.w, h: src.h, fps, src: job.frames, rec, n: m.frames };
  job = null;
  progress(null);
  setBusy(false);
  say('Done. Scoring VMAF v1...');
  showResult();
  window.pg.stats = stats;
  window.pg.done = true;
  scoreVmaf(shown);
}

// ---- VMAF v1, in its own worker ----

let vworker = null;

function scoreVmaf(s) {
  const id = ++vmafJob;
  ui.vmaf.textContent = '...';
  // a fresh worker each time, so pairs left from an older score can't mix in
  if (vworker) vworker.terminate();
  vworker = new Worker('vmaf-worker.js');
  // the project's rule: below 1920x1080 in both dimensions, scale up to fit it
  let ow = s.w, oh = s.h;
  if (s.w < 1920 && s.h < 1080) {
    const k = Math.min(1920 / s.w, 1080 / s.h);
    ow = Math.round(s.w * k / 2) * 2;
    oh = Math.round(s.h * k / 2) * 2;
  }
  const fsz = s.w * s.h * 3 / 2;
  let sent = 0, done = 0;
  const send = () => {
    // a few pairs in flight, so the worker never waits and memory stays flat
    while (sent < s.n && sent - done < 4) {
      const ref = s.src[sent].slice(), dis = s.rec.slice(sent * fsz, (sent + 1) * fsz);
      vworker.postMessage({ cmd: 'pair', ref: ref.buffer, dis: dis.buffer }, [ref.buffer, dis.buffer]);
      sent++;
    }
    if (sent === s.n) { vworker.postMessage({ cmd: 'score' }); sent++; }
  };
  vworker.onmessage = (e) => {
    const m = e.data;
    if (id !== vmafJob) return;
    if (m.type === 'pushed') {
      done = m.n;
      ui.vmaf.textContent = Math.floor(100 * done / s.n) + '%';
      send();
    } else if (m.type === 'score') {
      ui.vmaf.textContent = m.value.toFixed(2);
      window.pg.vmaf = m.value;
      if (!job) say('Done.');
    } else if (m.type === 'error') {
      ui.vmaf.textContent = 'n/a';
      if (!job) say('Done. VMAF could not run: ' + m.message);
    }
  };
  vworker.postMessage({ cmd: 'open', w: s.w, h: s.h, ow, oh });
  send();
}

// ---- the viewer ----

let splitX = 0.5, frameNo = 0, playRaf = 0, playT0 = 0, playF0 = 0, imgS = null, imgR = null, vcx = null;

function showResult() {
  const c = ui.view;
  c.width = shown.w; c.height = shown.h;
  vcx = c.getContext('2d');
  imgS = new ImageData(shown.w, shown.h);
  imgR = new ImageData(shown.w, shown.h);
  ui.seek.max = shown.n - 1;
  ui.stage.classList.add('has');
  for (const el of [ui.view, ui.split, ui.transport, ui.result, ...ui.stage.querySelectorAll('.tag')]) el.hidden = false;
  ui.empty.hidden = true;
  showFrame(0);
}

function showFrame(i) {
  frameNo = Math.max(0, Math.min(shown.n - 1, i));
  const fsz = shown.w * shown.h * 3 / 2;
  i420ToRgba(shown.src[frameNo], 0, shown.w, shown.h, imgS);
  i420ToRgba(shown.rec, frameNo * fsz, shown.w, shown.h, imgR);
  ui.seek.value = frameNo;
  ui.fno.textContent = (frameNo + 1) + ' / ' + shown.n;
  paint();
}

function paint() {
  const x = Math.round(splitX * shown.w);
  vcx.putImageData(imgS, 0, 0);
  if (x < shown.w) vcx.putImageData(imgR, 0, 0, x, 0, shown.w - x, shown.h);
  ui.split.style.left = (splitX * 100) + '%';
  ui.split.setAttribute('aria-valuenow', Math.round(splitX * 100));
}

function stopPlay() {
  if (playRaf) cancelAnimationFrame(playRaf);
  playRaf = 0;
  ui.play.innerHTML = '&#9654;';
  ui.play.setAttribute('aria-label', 'Play');
}

function tick(now) {
  const i = (playF0 + Math.floor((now - playT0) * shown.fps / 1000)) % shown.n;
  if (i !== frameNo) showFrame(i);
  playRaf = requestAnimationFrame(tick);
}

function togglePlay() {
  if (!shown) return;
  if (playRaf) return stopPlay();
  ui.play.innerHTML = '&#10074;&#10074;';
  ui.play.setAttribute('aria-label', 'Pause');
  playT0 = performance.now();
  playF0 = frameNo;
  playRaf = requestAnimationFrame(tick);
}

function step(d) { if (shown) { stopPlay(); showFrame(frameNo + d); } }

ui.play.onclick = togglePlay;
ui.prev.onclick = () => step(-1);
ui.next.onclick = () => step(1);
ui.seek.oninput = () => { stopPlay(); showFrame(+ui.seek.value); };

function dragSplit(e) {
  const r = ui.view.getBoundingClientRect();
  splitX = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  if (shown) paint();
}
for (const el of [ui.view, ui.split]) {
  el.addEventListener('pointerdown', (e) => {
    el.setPointerCapture(e.pointerId);
    dragSplit(e);
    el.onpointermove = dragSplit;
  });
  const end = () => { el.onpointermove = null; };
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
}
ui.split.addEventListener('keydown', (e) => {
  const d = e.key === 'ArrowLeft' ? -0.05 : e.key === 'ArrowRight' ? 0.05 : 0;
  if (!d) return;
  e.preventDefault();
  e.stopPropagation();
  splitX = Math.max(0, Math.min(1, splitX + d));
  paint();
});

document.addEventListener('keydown', (e) => {
  const t = e.target;
  if (t.closest && t.closest('input, select, textarea, [role=slider]')) return;
  if (e.key === 'Escape') { cancel(); return; }
  if (!shown) return;
  const n = e.shiftKey ? Math.round(shown.fps) : 1;
  if (e.key === ' ') { e.preventDefault(); togglePlay(); }
  else if (e.key === 'ArrowLeft') { e.preventDefault(); step(-n); }
  else if (e.key === 'ArrowRight') { e.preventDefault(); step(n); }
  else if (e.key === 'Home') { e.preventDefault(); step(-shown.n); }
  else if (e.key === 'End') { e.preventDefault(); step(shown.n); }
});

// ---- picking a clip ----

function setClip(file) {
  if (job) cancel();
  const kind = /\.y4m$/i.test(file.name) ? 'y4m' : 'video';
  clip = { kind, file, name: file.name };
  ui.dropEmpty.hidden = true;
  ui.dropClip.hidden = false;
  ui.clipName.textContent = file.name;
  ui.clipMeta.textContent = fmtBytes(file.size);
  say(window.pg.ready ? 'Ready. Press Encode.' : 'Waiting for the encoder...');
  setBusy(false);
}

async function loadUrl(url) {
  say('Fetching the sample clip...');
  try {
    const r = await fetch(url);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    setClip(new File([await r.blob()], url.split('/').pop(), { type: r.headers.get('content-type') || '' }));
    return true;
  } catch (e) {
    fail('Could not fetch ' + url + ': ' + e.message);
    return false;
  }
}

ui.file.onchange = () => { if (ui.file.files[0]) setClip(ui.file.files[0]); };
ui.change.onclick = () => ui.file.click();
ui.sample.onclick = async () => { if (await loadUrl(SAMPLE)) encode(); };
ui.drop.addEventListener('keydown', (e) => {
  if ((e.key === 'Enter' || e.key === ' ') && e.target === ui.drop) { e.preventDefault(); ui.file.click(); }
});
ui.drop.addEventListener('dragover', (e) => { e.preventDefault(); ui.drop.classList.add('over'); });
ui.drop.addEventListener('dragleave', () => ui.drop.classList.remove('over'));
ui.drop.addEventListener('drop', (e) => {
  e.preventDefault();
  ui.drop.classList.remove('over');
  if (e.dataTransfer.files[0]) setClip(e.dataTransfer.files[0]);
});
// a drop that misses the box would otherwise open the file in place of the page
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => {
  e.preventDefault();
  if (e.dataTransfer.files[0]) setClip(e.dataTransfer.files[0]);
});

for (const b of ui.preset.querySelectorAll('button')) {
  b.onclick = () => {
    for (const o of ui.preset.querySelectorAll('button')) o.setAttribute('aria-pressed', String(o === b));
    saveQuery();
  };
}
ui.crf.oninput = () => { ui.crfval.textContent = ui.crf.value; saveQuery(); };
ui.go.onclick = () => (job ? cancel() : encode());

function saveQuery() {
  const q = new URLSearchParams(location.search);
  q.set('preset', presetValue());
  q.set('crf', ui.crf.value);
  history.replaceState(null, '', '?' + q.toString());
}

// ?preset= and ?crf= set the controls; ?clip=<url> loads a clip; &auto=1 also encodes
async function fromQuery() {
  const q = new URLSearchParams(location.search);
  const p = q.get('preset');
  if (p) for (const o of ui.preset.querySelectorAll('button')) o.setAttribute('aria-pressed', String(o.dataset.v === p));
  if (q.get('crf')) { ui.crf.value = q.get('crf'); ui.crfval.textContent = ui.crf.value; }
  const url = q.get('clip');
  if (!url) return;
  if (!(await loadUrl(url))) return;
  if (q.get('auto')) {
    while (!window.pg.ready && !window.pg.error) await new Promise((res) => setTimeout(res, 50));
    encode();
  }
}

startWorker();
fromQuery();

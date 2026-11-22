// SPDX-License-Identifier: GPL-2.0-or-later
// The playground page: turn a clip into I420 frames, feed the encoder
// worker, and show the source beside the encoder's reconstruction.
'use strict';

const MAX_W = 1280, MAX_H = 720, MAX_SECONDS = 10, MAX_FPS = 30;
// the page holds the source and the reconstruction in memory: 10 s at 480p
const PIXEL_BUDGET = 854 * 480 * 300;

const $ = (id) => document.getElementById(id);
const ui = {
  drop: $('drop'), file: $('file'), preset: $('preset'), crf: $('crf'),
  crfval: $('crfval'), go: $('go'), bar: $('bar'), msg: $('msg'),
  result: $('result'), view: $('view'), split: $('split'), play: $('play'),
  seek: $('seek'), fno: $('fno'), dl: $('dl'), dlMp4: $('dl-mp4'),
};

// what a test harness reads
window.pg = { ready: false, done: false, error: null, stats: null };

let worker = null;
let clip = null;            // {kind, file, name}
let job = null;             // the encode in flight
let shown = null;           // {w, h, fps, src: [Uint8Array], rec: Uint8Array}

function say(text) { ui.msg.textContent = text; }
function progress(f) { ui.bar.style.width = (100 * Math.max(0, Math.min(1, f))) + '%'; }
function fail(text) {
  window.pg.error = text;
  say(text);
  ui.go.disabled = !clip;
  job = null;
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
    say(clip ? 'Ready.' : 'Encoder loaded (' + m.version + '). Drop a clip to start.');
    ui.go.disabled = !clip;
  } else if (m.type === 'progress' && job) {
    job.encoded = m.frames;
    progress(job.total ? m.frames / job.total : 0);
    say('Encoding frame ' + m.frames + ' of ' + (job.total || '?') +
        (job.decoded < job.total ? ' (reading frame ' + job.decoded + ')' : '') + '...');
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

// ---- reading the clip ----

function fitSize(w, h) {
  const s = Math.min(1, MAX_W / w, MAX_H / h);
  return { w: Math.max(2, Math.round(w * s / 2) * 2), h: Math.max(2, Math.round(h * s / 2) * 2) };
}

// a y4m reader: header, then FRAME lines; 4:2:0 8-bit only
async function y4mSource(file) {
  const buf = new Uint8Array(await file.arrayBuffer());
  let nl = buf.indexOf(10);
  const head = new TextDecoder().decode(buf.subarray(0, nl)).split(' ');
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
  const frames = [];
  let p = nl + 1;
  while (p < buf.length) {
    nl = buf.indexOf(10, p);
    if (nl < 0 || nl + 1 + fsz > buf.length) break;
    frames.push(nl + 1);
    p = nl + 1 + fsz;
  }
  let fps = fn / fd, step = 1;
  while (fps / step > MAX_FPS + 0.01) step++;
  const out = fitSize(w, h);
  const total = Math.min(Math.floor(frames.length / step), Math.floor(MAX_SECONDS * fps / step),
                         Math.floor(PIXEL_BUDGET / (out.w * out.h)));
  let scaler = null;
  if (out.w !== w || out.h !== h) {
    const a = document.createElement('canvas'); a.width = w; a.height = h;
    const b = document.createElement('canvas'); b.width = out.w; b.height = out.h;
    scaler = { a, b, ax: a.getContext('2d'), bx: b.getContext('2d', { willReadFrequently: true }), img: new ImageData(w, h) };
  }
  return {
    w: out.w, h: out.h, fpsNum: fn, fpsDen: fd * step, total,
    async frame(i) {
      const off = frames[i * step];
      if (!scaler) return buf.slice(off, off + fsz);
      scaler.ax.putImageData(i420ToRgba(buf, off, w, h, scaler.img), 0, 0);
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
  await Promise.race([
    once(video, 'loadeddata'),
    once(video, 'error').then(() => { throw new Error('this browser cannot decode that file'); }),
  ]);
  if (!video.videoWidth) throw new Error('no video track found');
  let fps = await measureFps(video);
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
    close() { video.pause(); video.remove(); URL.revokeObjectURL(video.src); },
  };
}

// ---- the encode ----

async function encode() {
  if (!clip || job) return;
  ui.go.disabled = true;
  ui.result.hidden = true;
  stopPlay();
  window.pg.done = false;
  window.pg.error = null;
  progress(0);
  say('Reading the clip...');
  let src;
  try {
    src = clip.kind === 'y4m' ? await y4mSource(clip.file) : await videoSource(clip.file);
  } catch (e) {
    fail('Could not read the clip: ' + e.message);
    return;
  }
  const opts = 'preset=' + ui.preset.value + ':crf=' + ui.crf.value;
  job = { src, total: src.total, decoded: 0, encoded: 0, frames: [], opts, t0: performance.now() };
  worker.postMessage({ cmd: 'open', width: src.w, height: src.h, fpsNum: src.fpsNum, fpsDen: src.fpsDen, opts });
  try {
    for (let i = 0; i < src.total; i++) {
      const f = await src.frame(i);
      if (!job) return;
      job.frames.push(f);
      job.decoded = i + 1;
      const copy = f.slice();
      worker.postMessage({ cmd: 'frame', data: copy.buffer, pts: i }, [copy.buffer]);
    }
  } catch (e) {
    if (src.close) src.close();
    fail('Could not read frame ' + job.decoded + ': ' + e.message);
    return;
  }
  if (src.close) src.close();
  job.readMs = performance.now() - job.t0;
  say('Finishing the encode...');
  worker.postMessage({ cmd: 'finish' });
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

function finish(m) {
  const src = job.src;
  const fps = src.fpsNum / src.fpsDen;
  const rec = new Uint8Array(m.recon);
  const secs = m.frames / fps;
  const psnr = psnrY(job.frames, rec, src.w, src.h);
  const stats = {
    width: src.w, height: src.h, frames: m.frames, fps,
    bytes: m.bytes, kbps: m.bytes * 8 / 1000 / secs, psnrY: psnr,
    encodeMs: m.encodeMs, encodeFps: m.frames / (m.encodeMs / 1000),
    wallMs: performance.now() - job.t0, readMs: job.readMs, opts: job.opts,
  };
  ui.dl.href = URL.createObjectURL(new Blob([m.hevc], { type: 'video/hevc' }));
  try {
    const mp4 = hevcToMp4(new Uint8Array(m.hevc), new Int32Array(m.aus), m.reorder,
                          src.w, src.h, src.fpsNum, src.fpsDen);
    ui.dlMp4.href = URL.createObjectURL(new Blob([mp4], { type: 'video/mp4' }));
    ui.dlMp4.hidden = false;
  } catch (e) {
    ui.dlMp4.hidden = true;
  }
  $('s-size').textContent = fmtBytes(m.bytes);
  $('s-rate').textContent = stats.kbps.toFixed(0) + ' kbit/s';
  $('s-psnr').textContent = isFinite(psnr) ? psnr.toFixed(2) + ' dB' : 'lossless';
  $('s-time').textContent = (m.encodeMs / 1000).toFixed(1) + ' s';
  $('s-fps').textContent = stats.encodeFps.toFixed(1) + ' fps, ' + src.w + 'x' + src.h;
  shown = { w: src.w, h: src.h, fps, src: job.frames, rec, n: m.frames };
  job = null;
  progress(1);
  say('Done: ' + m.frames + ' frames at ' + src.w + 'x' + src.h + ', ' + job_opts_text(stats.opts) + '.');
  showResult();
  window.pg.stats = stats;
  window.pg.done = true;
  ui.go.disabled = false;
}

function job_opts_text(o) { return o.replace(/=/g, ' ').replace(/:/g, ', '); }

// ---- the viewer ----

let splitX = 0.5, frameNo = 0, playTimer = null, imgS = null, imgR = null;

function showResult() {
  const c = ui.view;
  c.width = shown.w; c.height = shown.h;
  imgS = new ImageData(shown.w, shown.h);
  imgR = new ImageData(shown.w, shown.h);
  ui.seek.max = shown.n - 1;
  ui.seek.value = 0;
  ui.result.hidden = false;
  showFrame(0);
}

function showFrame(i) {
  frameNo = i;
  const fsz = shown.w * shown.h * 3 / 2;
  i420ToRgba(shown.src[i], 0, shown.w, shown.h, imgS);
  i420ToRgba(shown.rec, i * fsz, shown.w, shown.h, imgR);
  ui.seek.value = i;
  ui.fno.textContent = (i + 1) + ' / ' + shown.n;
  paint();
}

function paint() {
  const cx = ui.view.getContext('2d');
  const x = Math.round(splitX * shown.w);
  cx.putImageData(imgS, 0, 0);
  if (x < shown.w) cx.putImageData(imgR, 0, 0, x, 0, shown.w - x, shown.h);
  ui.split.style.left = (splitX * 100) + '%';
}

function stopPlay() {
  if (playTimer) clearInterval(playTimer);
  playTimer = null;
  ui.play.textContent = 'Play';
}

ui.play.onclick = () => {
  if (playTimer) return stopPlay();
  ui.play.textContent = 'Pause';
  playTimer = setInterval(() => showFrame((frameNo + 1) % shown.n), 1000 / shown.fps);
};
ui.seek.oninput = () => { stopPlay(); showFrame(+ui.seek.value); };

function dragSplit(e) {
  const r = ui.view.getBoundingClientRect();
  splitX = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  if (shown) paint();
}
ui.view.addEventListener('pointerdown', (e) => {
  ui.view.setPointerCapture(e.pointerId);
  dragSplit(e);
  ui.view.onpointermove = dragSplit;
});
ui.view.addEventListener('pointerup', () => { ui.view.onpointermove = null; });

// ---- picking a clip ----

function setClip(file) {
  const kind = /\.y4m$/i.test(file.name) ? 'y4m' : 'video';
  clip = { kind, file, name: file.name };
  say('Selected ' + file.name + ' (' + fmtBytes(file.size) + ').' + (window.pg.ready ? ' Press Encode.' : ' Waiting for the encoder...'));
  ui.go.disabled = !window.pg.ready;
}

ui.file.onchange = () => { if (ui.file.files[0]) setClip(ui.file.files[0]); };
ui.drop.addEventListener('dragover', (e) => { e.preventDefault(); ui.drop.classList.add('over'); });
ui.drop.addEventListener('dragleave', () => ui.drop.classList.remove('over'));
ui.drop.addEventListener('drop', (e) => {
  e.preventDefault();
  ui.drop.classList.remove('over');
  if (e.dataTransfer.files[0]) setClip(e.dataTransfer.files[0]);
});
ui.crf.oninput = () => { ui.crfval.textContent = ui.crf.value; };
ui.go.onclick = encode;

// ?clip=<url> loads a clip without a drop; &auto=1 also starts the encode
async function fromQuery() {
  const q = new URLSearchParams(location.search);
  if (q.get('preset')) ui.preset.value = q.get('preset');
  if (q.get('crf')) { ui.crf.value = q.get('crf'); ui.crfval.textContent = ui.crf.value; }
  const url = q.get('clip');
  if (!url) return;
  const r = await fetch(url);
  if (!r.ok) return fail('Could not fetch ' + url);
  setClip(new File([await r.blob()], url.split('/').pop()));
  if (q.get('auto')) {
    while (!window.pg.ready && !window.pg.error) await new Promise((res) => setTimeout(res, 50));
    encode();
  }
}

startWorker();
fromQuery();

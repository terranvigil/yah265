#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""vbv_check.py - does the coded stream fit the buffer it claims to?

The hard gate for phase 12 item 2. For each clip and each (maxrate, bufsize)
pair it encodes with `--vbv-maxrate`/`--vbv-bufsize`, reads N265_VBV_TRACE,
and RE-DERIVES the CPB trajectory from the coded picture sizes alone:

    fill(0)     = init * bufsize
    fill(k + 1) = min(fill(k) - bits(k) + maxrate / fps, bufsize)

An underflow is any k with fill(k) - bits(k) < 0. The encoder's own
fill_in / fill_out columns are checked against that walk rather than
trusted - a checker that reads the encoder's opinion of its own buffer
checks nothing - and a disagreement is reported as `DESYNC`, which is a
failure in its own right.

Phase 12 item 5 adds a THIRD walk, and it is the only one that reads what
a decoder actually receives. The coded stream is parsed - Annex B start
codes, access units closing at each VCL NAL, the buffering_period (D.2.2)
and pic_timing (D.2.3) SEI messages read field by field - and the CPB is
walked again from the SIGNALLED numbers alone: the removal times C.2.3
derives from the signalled delays, the buffer filling at the DECLARED
BitRate from the signalled initial delay, and draining by the access unit
sizes taken from the stream itself. Three things then have to agree, and
each catches a different bug:

    `sei` underflows == the trace walk's underflows   the signalling is
        consistent with the buffer the encoder actually planned against
    the removal times are strictly monotone           C.2.3 is satisfiable
    the stream's own AU sizes == the trace's bits     the trace is the
        stream, so `desync` above is about the FILL and not the sizes

The middle walk (the encoder's opinion) and the first (the trace's sizes)
are both encoder-side. This one would still fail if the encoder wrote a
perfect stream and lied about it in the SEI, which is the whole question
"is the HRD signalling true" reduces to.

    CLIPS=foreman_cif python3 scripts/vbv_check.py
    MODE=crf CRF=28 python3 scripts/vbv_check.py
    PAIRS='1.0x1.0s,1.0x0.5s,1.5x2.0s' JOBS=4 python3 scripts/vbv_check.py
    GOP=3 THREADS=6 JOBS=1 N265_GOP_LEN=40 python3 scripts/vbv_check.py

A pair is `<rate multiple>x<buffer seconds>`: the rate multiplies the leg's
base rate (the ladder's rung 2 under ABR, the achieved rate under CRF) and
the buffer is that many SECONDS of the resulting maxrate, which is how a
VBV is specified in practice and what makes 0.5 s the hard cell.

Exit status is 1 if any cell underflows or desyncs, which is the gate.
"""
import json, math, os, re, statistics, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
N = os.environ.get("N265", os.path.join(ROOT, "build", "n265"))
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
MODE = os.environ.get("MODE", "abr")            # abr | crf
CRF = os.environ.get("CRF", "28")
RUNG = int(os.environ.get("RUNG", "2"))         # 1-based ladder rung
INIT = os.environ.get("VBV_INIT", "0.9")
JOBS = int(os.environ.get("JOBS", "2"))
THREADS = os.environ.get("THREADS", "")
PAIRS = os.environ.get("PAIRS", "1.0x1.0s,1.0x0.5s,1.5x2.0s")
VERBOSE = os.environ.get("VERBOSE", "") != ""
# GOP=N: encode with --gop-parallel N (phase 14 item 42). A joined stream
# is several encoders' work, so their traces interleave and none of them
# describes the buffer the stream fills; the cell is walked from the
# stream's own access unit sizes instead, which is the walk a decoder
# makes. A joined stream carries no HRD timing SEI, so that walk is off.
# Segments are --keyint frames by default and the clips are 120: set
# N265_GOP_LEN (40, say) so a clip runs past one segment.
GOP = os.environ.get("GOP", "")

ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)


def fps_of(clip):
    with open(os.path.join(ROOT, "tests", "corpus", clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0


def parse_pairs(spec):
    out = []
    for p in spec.split(","):
        m = re.fullmatch(r"\s*([\d.]+)x([\d.]+)s\s*", p)
        if not m:
            sys.exit(f"vbv_check: {p!r} is not <mult>x<seconds>s")
        out.append((float(m.group(1)), float(m.group(2))))
    return out


def run(clip, extra, trace=True, keep=False):
    """Encode and return (bytes, stderr, path). The path is None unless
    `keep`, in which case the CALLER unlinks it - the SEI leg has to read
    the stream the encode actually wrote."""
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    fd, out = tempfile.mkstemp(suffix=".hevc")
    os.close(fd)
    env = dict(os.environ)
    if trace:
        env["N265_VBV_TRACE"] = "1"
    cmd = [N, src, "--frames", str(FRAMES)] + ARGS.split() + extra
    if THREADS:
        cmd += ["--threads", THREADS]
    if GOP:
        cmd += ["--gop-parallel", GOP] + ([] if THREADS else
                                          ["--threads", "6"])
    cmd += ["-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    size = os.path.getsize(out)
    if r.returncode != 0:
        os.unlink(out)
        return None, r.stderr, None
    if not keep:
        os.unlink(out)
        out = None
    return size, r.stderr, out


ROW = re.compile(r"^VBV,(-?\d+),(-?\d+),(\d+),([-\d.e+]+),([-\d.e+]+),"
                 r"([-\d.e+]+),([-\d.e+]+),([-\d.e+]+),([-\d.e+]+)$", re.M)


def walk(rows, maxrate, bufsize, init, fps):
    """The CPB trajectory, re-derived from the coded sizes alone."""
    inflow = maxrate / fps
    fill = min(init * bufsize, bufsize)
    minfill = fill
    under = 0
    desync = 0
    for r in rows:
        bits = r["bits"]
        if r["fill_in"] is not None and abs(fill - r["fill_in"]) > 1.0:
            desync += 1
        fill -= bits
        minfill = min(minfill, fill)
        if fill < 0.0:
            under += 1
        fill = min(fill + inflow, bufsize)
        if r["fill_out"] is not None and abs(fill - r["fill_out"]) > 1.0:
            desync += 1
    return minfill, under, desync


# ---- the stream's own half: the HRD SEI messages, and C.2's walk ----
#
# The delays are u(v) at the lengths the SPS's hrd_parameters declares, and
# this encoder declares all three as 24 bits (headers.c, write_hrd). The
# payload SIZES are what prove it here rather than a second SPS parser: a
# buffering period at any other delay length is not 10 bytes and a pic
# timing is not 6, so a stream that moved the lengths is reported as an
# unreadable message instead of being read with the wrong ruler.
BP_BYTES, PT_BYTES, DELAY_BITS = 10, 6, 24


class Bits:
    """An MSB-first reader over one SEI payload."""

    def __init__(self, b):
        self.b, self.pos = b, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            if self.pos >= len(self.b) * 8:
                raise ValueError("SEI payload ran out")
            v = (v << 1) | ((self.b[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def ue(self):
        lz = 0
        while not self.u(1):
            lz += 1
            if lz > 31:
                raise ValueError("runaway Exp-Golomb")
        return (1 << lz) - 1 + (self.u(lz) if lz else 0)


def unescape(b):
    """7.4.2: drop the emulation prevention bytes."""
    out, zeros = bytearray(), 0
    for c in b:
        if zeros >= 2 and c == 3:
            zeros = 0
            continue
        out.append(c)
        zeros = zeros + 1 if c == 0 else 0
    return bytes(out)


def sei_messages(rbsp):
    """D.2.1's sei_rbsp: (payload type, payload bytes) in order."""
    out, i = [], 0
    while i < len(rbsp) - 1:
        t = 0
        while rbsp[i] == 255:
            t += 255
            i += 1
        t += rbsp[i]
        i += 1
        n = 0
        while rbsp[i] == 255:
            n += 255
            i += 1
        n += rbsp[i]
        i += 1
        out.append((t, rbsp[i:i + n]))
        i += n
        # the rest is rbsp_trailing_bits once the last message has been read
        if i >= len(rbsp) or rbsp[i] == 0x80:
            break
    return out


def au_walk(path):
    """The access units of an Annex B stream, in coding order.

    An AU closes at its VCL NAL (7.4.2.4.4: the parameter sets and the
    prefix SEIs of an AU precede its first VCL NAL), and its size counts
    the start codes, which is exactly what the encoder charges the CPB.
    Returns one dict per AU: `bytes`, `vcl` (the nal_unit_type of its
    slice) and the buffering period / pic timing it carries."""
    data = open(path, "rb").read()
    starts = [m.start() for m in re.finditer(b"\x00\x00\x00\x01", data)]
    if not starts:
        raise ValueError("no Annex B start codes")
    aus, cur = [], dict(bytes=0, vcl=None, bp=None, pt=None, nsei=0)
    for k, off in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(data)
        nal = data[off + 4:end]
        typ = (nal[0] >> 1) & 0x3f
        cur["bytes"] += end - off
        if typ == 39:                       # PREFIX_SEI
            for t, pay in sei_messages(unescape(nal[2:])):
                cur["nsei"] += 1
                if t == 0:
                    if len(pay) != BP_BYTES:
                        raise ValueError("buffering period is %d bytes, not %d"
                                         % (len(pay), BP_BYTES))
                    r = Bits(pay)
                    r.ue()                  # bp_seq_parameter_set_id
                    irap = r.u(1)
                    r.u(1)                  # concatenation_flag
                    r.u(DELAY_BITS)         # au_cpb_removal_delay_delta_minus1
                    cur["bp"] = (r.u(DELAY_BITS), r.u(DELAY_BITS), irap)
                elif t == 1:
                    if len(pay) != PT_BYTES:
                        raise ValueError("pic timing is %d bytes, not %d"
                                         % (len(pay), PT_BYTES))
                    r = Bits(pay)
                    cur["pt"] = (r.u(DELAY_BITS) + 1, r.u(DELAY_BITS))
        elif typ < 32:                      # a VCL NAL closes the AU
            cur["vcl"] = typ
            aus.append(cur)
            cur = dict(bytes=0, vcl=None, bp=None, pt=None, nsei=0)
    if cur["bytes"]:
        raise ValueError("trailing NALs after the last slice")
    return aus


def declared_pair(maxrate, bufsize):
    """E.2.3's BitRate[0] and CpbSize[0], solved the way encoder.c solves
    them: the smallest scale whose value fits, rounded UP. The SEI walk
    reads the schedule the SPS DECLARES, not the raw knobs - they differ
    by less than one unit of the scale, and the declared one is what a
    decoder would use."""
    out = []
    for v, sh in ((maxrate, 6), (bufsize, 4)):
        s = 0
        while s < 15 and math.ceil(v / float(1 << (sh + s))) > 2147483646.0:
            s += 1
        out.append(max(1, int(math.ceil(v / float(1 << (sh + s))))) << (sh + s))
    return out[0], out[1]


def hrd_walk(aus, bitrate, cpbsize, tick):
    """C.2, from the SIGNALLED delays alone.

    Removal times: the first access unit is removed at its own initial
    delay; every one after it at the removal time of the access unit that
    carried the most recent PRECEDING buffering period, plus its signalled
    au_cpb_removal_delay in clock ticks. The CPB fills at BitRate from t=0,
    stops when full (cbr_flag is 0), and the whole access unit leaves at
    its removal time."""
    fill, t_prev, last_bp, t_bp = 0.0, 0.0, None, 0.0
    under, mono, minfill = 0, 0, None
    t_last = None
    for n, au in enumerate(aus):
        if au["pt"] is None:
            raise ValueError("access unit %d carries no pic_timing" % n)
        cpb_delay, _ = au["pt"]
        if n == 0:
            if au["bp"] is None:
                raise ValueError("the first access unit carries no "
                                 "buffering period")
            t_r = au["bp"][0] / 90000.0
        else:
            if last_bp is None:
                raise ValueError("no buffering period before access unit %d" % n)
            t_r = t_bp + tick * cpb_delay
        if au["bp"] is not None:
            last_bp, t_bp = n, t_r
        if t_last is not None and not t_r > t_last + 1e-12:
            mono += 1
        t_last = t_r
        fill = min(cpbsize, fill + bitrate * (t_r - t_prev))
        t_prev = t_r
        fill -= au["bytes"] * 8.0
        if minfill is None or fill < minfill:
            minfill = fill
        if fill < 0.0:
            under += 1
    return minfill if minfill is not None else 0.0, under, mono


def cell(clip, mult, secs, base_rate):
    """One (clip, pair) cell. base_rate is bit/s."""
    fps = fps_of(clip)
    # The knobs are kbit/s and kbit and the CLI rounds them, so the walk has
    # to be re-derived from what the ENCODER was told and not from what this
    # script wanted. Getting that backwards is a DESYNC on every row.
    kbps = round(mult * base_rate / 1000.0)
    kbit = round(secs * kbps)
    maxrate = kbps * 1000.0
    bufsize = kbit * 1000.0
    extra = ["--vbv-maxrate", "%d" % kbps, "--vbv-bufsize", "%d" % kbit,
             "--vbv-init", INIT]
    if MODE == "abr":
        extra = ["--bitrate", "%.0f" % (base_rate / 1000.0)] + extra
    else:
        extra = ["--crf", CRF] + extra
    size, err, path = run(clip, extra, keep=True)
    if size is None:
        return dict(clip=clip, pair=(mult, secs), err=err.strip()[-300:])
    try:
        aus = au_walk(path)
        sei_err = None
    except (ValueError, IndexError) as e:
        aus, sei_err = [], str(e)
    os.unlink(path)
    rows = [dict(code=int(m.group(1)), disp=int(m.group(2)),
                 slice=int(m.group(3)), pred=float(m.group(4)),
                 allow=float(m.group(5)), bits=float(m.group(6)),
                 fill_in=float(m.group(7)), fill_out=float(m.group(8)),
                 dqp=float(m.group(9)))
            for m in ROW.finditer(err)]
    if GOP and sei_err is None:
        # the joined stream's own sizes, in coding order (see GOP above)
        rows = [dict(code=k, disp=k, slice=au["vcl"], pred=0.0, allow=0.0,
                     bits=au["bytes"] * 8.0, fill_in=None, fill_out=None,
                     dqp=0.0) for k, au in enumerate(aus)]
    if not rows:
        return dict(clip=clip, pair=(mult, secs),
                    err="no VBV rows in the trace")
    minfill, under, desync = walk(rows, maxrate, bufsize, float(INIT), fps)
    clamped = sum(1 for r in rows if r["dqp"] > 1e-9)
    # the third walk: the stream's own SEI messages. The access unit sizes
    # it reads are checked against the trace's bits first, so a difference
    # in the CPB can only ever be about the SIGNALLING.
    sei_under = sei_mono = sei_bp = 0
    sei_minpct = 0.0
    sei_size = 0
    if GOP and sei_err is None:
        sei_err = "off (a joined stream carries no HRD timing SEI)"
    elif sei_err is None:
        try:
            if len(aus) != len(rows):
                raise ValueError("%d access units, %d trace rows"
                                 % (len(aus), len(rows)))
            for au, r in zip(aus, rows):
                if abs(au["bytes"] * 8.0 - r["bits"]) > 0.5:
                    raise ValueError("access unit %d is %d bits in the stream "
                                     "and %.0f in the trace"
                                     % (r["code"], au["bytes"] * 8, r["bits"]))
            br, cpb = declared_pair(maxrate, bufsize)
            sei_min, sei_under, sei_mono = hrd_walk(aus, br, cpb, 1.0 / fps)
            sei_minpct = 100.0 * sei_min / bufsize
            sei_bp = sum(1 for a in aus if a["bp"] is not None)
            sei_size = sum(a["nsei"] for a in aus)
        except (ValueError, IndexError) as e:
            sei_err = str(e)
    return dict(clip=clip, pair=(mult, secs), maxrate=maxrate,
                bufsize=bufsize, frames=len(rows), bytes=size,
                achieved=size * 8.0 * fps / len(rows),
                minfill=minfill, minpct=100.0 * minfill / bufsize,
                under=under, desync=desync, clamped=clamped,
                sei_under=sei_under, sei_mono=sei_mono, sei_bp=sei_bp,
                sei_minpct=sei_minpct, sei_msgs=sei_size, sei_err=sei_err,
                maxdqp=max(r["dqp"] for r in rows), err=None)


def base_rate_of(clip):
    """The rate every pair is a multiple of."""
    if MODE == "abr":
        rungs = ladders[clip]
        return rungs[min(RUNG, len(rungs)) - 1] * 1000.0
    # capped CRF: the rate the pinned CRF achieves with no VBV at all, so
    # "maxrate = 1.2x the achieved rate" is a statement about this clip and
    # not about a ladder it is not being run on.
    fps = fps_of(clip)
    size, _, _ = run(clip, ["--crf", CRF], trace=False)
    if size is None:
        return 0.0
    return size * 8.0 * fps / FRAMES


def sei_note(r):
    """What the stream's own messages said about this cell."""
    if r["sei_err"]:
        return "UNREADABLE (%s)" % r["sei_err"]
    return ("underflow %d (%.1f%%)  bp %d  msgs %d%s"
            % (r["sei_under"], r["sei_minpct"], r["sei_bp"], r["sei_msgs"],
               "  NON-MONOTONE %d" % r["sei_mono"] if r["sei_mono"] else ""))


def main():
    pairs = parse_pairs(PAIRS)
    if MODE == "crf":
        # the item's capped-CRF cell: maxrate 1.2x the achieved rate
        pairs = parse_pairs(os.environ.get("PAIRS_CRF", "1.2x1.0s"))
    print(f"vbv_check: mode={MODE} frames={FRAMES} args={ARGS!r} "
          f"init={INIT} pairs={pairs}", flush=True)
    bases = {}
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        for c, b in zip(clips, ex.map(base_rate_of, clips)):
            bases[c] = b
            print(f"  {c:<18} base {b/1000:8.0f} kbit/s", flush=True)

    jobs = [(c, m, s) for c in clips for (m, s) in pairs]
    rows = []
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        for r in ex.map(lambda a: cell(a[0], a[1], a[2], bases[a[0]]), jobs):
            rows.append(r)
            if r["err"]:
                print(f"  {r['clip']:<18} {r['pair']}  ERROR {r['err']}",
                      flush=True)
            else:
                print("  %-18s %3.1fx/%.1fs  min fill %9.0f (%6.1f%%)  "
                      "underflow %d  desync %d  clamped %d/%d  max dQP %.2f"
                      "  | SEI %s"
                      % (r["clip"], r["pair"][0], r["pair"][1], r["minfill"],
                         r["minpct"], r["under"], r["desync"], r["clamped"],
                         r["frames"], r["maxdqp"], sei_note(r)), flush=True)

    for label, key in (("VBV underflow table", "under"),
                       ("HRD SEI underflow table", "sei_under")):
        print("\n=== %s (%s, %d frames) ===" % (label, MODE, FRAMES))
        print("| clip | " + " | ".join("%.1fx/%.1fs" % p for p in pairs) + " |")
        for c in clips:
            cells = []
            for p in pairs:
                r = next((x for x in rows if x["clip"] == c and x["pair"] == p),
                         None)
                if r is None or r["err"] or (key == "sei_under"
                                             and r["sei_err"]):
                    cells.append("ERR")
                else:
                    cells.append("%d / %.0f%%"
                                 % (r[key], r["minpct"] if key == "under"
                                            else r["sei_minpct"]))
            print("| %s | %s |" % (c, " | ".join(cells)))
    print("(cell is `underflows / min fill as a %% of the buffer`; the gate "
          "is zero underflows everywhere, and the two tables have to AGREE - "
          "the second is walked from the stream's own buffering_period and "
          "pic_timing SEIs, the first from the encoder's trace)")
    bad = 0
    for r in rows:
        if r["err"]:
            bad += 1
            continue
        bad += r["under"] + r["desync"] + r["sei_mono"]
        if r["sei_err"] and GOP:
            pass                    # no SEI leg on a joined stream
        elif r["sei_err"]:
            bad += 1
            print("  SEI ERROR %-18s %.1fx/%.1fs  %s"
                  % (r["clip"], r["pair"][0], r["pair"][1], r["sei_err"]))
        elif r["sei_under"] != r["under"]:
            bad += 1
            print("  SEI DISAGREES %-18s %.1fx/%.1fs  signalled %d "
                  "underflows, the trace walk %d"
                  % (r["clip"], r["pair"][0], r["pair"][1], r["sei_under"],
                     r["under"]))
    ok = [r for r in rows if not r["err"]]
    if ok:
        print("total underflows %d, total desyncs %d, worst min fill %.1f%%, "
              "median clamped share %.1f%%"
              % (sum(r["under"] for r in ok), sum(r["desync"] for r in ok),
                 min(r["minpct"] for r in ok),
                 statistics.median(100.0 * r["clamped"] / r["frames"]
                                   for r in ok)))
        good = [r for r in ok if not r["sei_err"]]
        print("SEI: %d of %d cells read, %d buffering periods, %d messages, "
              "signalled underflows %d against the trace walk's %d, "
              "non-monotone removal times %d"
              % (len(good), len(ok), sum(r["sei_bp"] for r in good),
                 sum(r["sei_msgs"] for r in good),
                 sum(r["sei_under"] for r in good),
                 sum(r["under"] for r in good),
                 sum(r["sei_mono"] for r in good)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

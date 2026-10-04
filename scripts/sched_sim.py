#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay an N265_TASK_TRACE through a model of the pool and the driver.

sched_sim.py TRACE [--threads N] [--k K] [--lag D|inf] [--ecore S] [--noprio]

The trace gives every CTB's time, every deblock / publish row's time, each
frame's entropy time, each frame's references, and the driver's own serial
time per frame (the gaps between dispatches less its joins). The model:

- a frame's encode rows run CTB by CTB; CTB (r, c) needs (r, c-1) and,
  for r > 0, CTBs 0..c+1 of row r-1 (the two-CTB lag); a row starts only
  when each reference has published r + ref_lag rows (the start hint);
- deblock row r needs deblock row r-1 and encode rows 0..r+1; publish row
  r needs publish row r-1 and deblock rows 0..r+1; entropy needs every
  publish row; the frame retires when entropy is done;
- a worker that finishes a CTB continues its row if the next CTB is ready,
  else the row yields and the worker claims the best ready unit: the
  lowest claim class (reference frames first), then the oldest frame,
  then the lowest task index;
- the driver, per frame in coding order: waits for a free frame_work (the
  frame K back retired), spends its pre-drain time, waits for the rate
  control drain (every frame <= k-1-D retired; D = inf is no drain, CRF),
  spends its post-drain time, and dispatches.
`--ecore S`: workers past the sixth run at speed S (this box's efficiency
cores); 1.0 treats every worker alike.
"""
import sys, heapq, argparse
from collections import defaultdict


def load(path):
    ctu = {}
    lagw = defaultdict(float)
    rowspan = defaultdict(float)
    dbk, pub, ent = {}, {}, {}
    disp, refs, joins = {}, defaultdict(list), []
    rstall = 0.0
    for ln in open(path):
        t, k, f, r, c, w, a, t0, t1 = ln.split()
        t, k, f, r, c, a = int(t), int(k), int(f), int(r), int(c), int(a)
        t0, t1 = float(t0), float(t1)
        if t == 2:
            ctu[(f, r, c)] = t1 - t0
        elif t == 7:
            lagw[(f, r)] += t1 - t0
        elif t == 1:
            if k == 0:
                rowspan[(f, r)] += t1 - t0
            elif k == 1:
                dbk[(f, r)] = t1 - t0
            elif k == 2:
                pub[(f, r)] = t1 - t0
            else:
                ent[f] = t1 - t0
        elif t == 4:
            disp[f] = dict(st=k, R=r, nonref=c, poc=a, t=t0)
        elif t == 6:
            refs[f].append(a)
        elif t == 5:
            joins.append((t0, t1, f))
        elif t == 3:
            rstall += t1 - t0
    frames = sorted(disp)
    R = max(d['R'] for d in disp.values())
    C = 1 + max(c for (_, _, c) in ctu)
    poc2f = {}
    F = []
    for f in frames:
        d = disp[f]
        rs = [poc2f[p] for p in refs[f] if p in poc2f]
        if not d['nonref']:
            poc2f[d['poc']] = f
        cost = [[ctu.get((f, r, c), 0.0) for c in range(C)] for r in range(R)]
        for r in range(R):
            ov = rowspan[(f, r)] - sum(cost[r]) - lagw[(f, r)]
            if ov > 0:
                cost[r][0] += ov
        F.append(dict(f=f, st=d['st'], nonref=d['nonref'], refs=rs, cost=cost,
                      dbk=[dbk.get((f, r), 0.0) for r in range(R)],
                      pub=[pub.get((f, r), 0.0) for r in range(R)],
                      ent=ent.get(f, 0.0), t=d['t']))
    # the driver: busy time per frame, split at the last join before the
    # dispatch (the drain under ABR / VBV; under CRF the K gate)
    joins.sort()
    prev = 0.0
    ji = 0
    for x in F:
        busy, last_end = 0.0, None
        tt = prev
        while ji < len(joins) and joins[ji][1] <= x['t'] + 1e-9:
            j0, j1, _ = joins[ji]
            if j0 >= prev - 1e-9:
                busy += max(0.0, j0 - tt)
                tt = j1
                last_end = j1
            ji += 1
        busy += max(0.0, x['t'] - tt)
        post = x['t'] - last_end if last_end is not None and last_end >= prev else 0.0
        post = min(post, busy)
        x['pre'], x['post'] = busy - post, post
        prev = x['t']
    end = max(j[1] for j in joins) if joins else 0.0
    return dict(F=F, R=R, C=C, wall=end, rstall=rstall)


def simulate(T, N, K, D, ecore=1.0, ref_lag=2, prio=True, inflight_log=None):
    F, R, C = T['F'], T['R'], T['C']
    n = len(F)
    speed = [1.0 if i < 6 else ecore for i in range(N)]
    # per-frame state
    ectb = [[0] * R for _ in range(n)]       # CTBs done per row
    erun = [[False] * R for _ in range(n)]   # row held by a worker
    erows = [0] * n
    dbk = [0] * n
    pub = [0] * n
    dbk_run = [False] * n
    pub_run = [False] * n
    ent_run = [False] * n
    done = [False] * n
    disp = [False] * n
    tdone = [None] * n
    events = []          # (time, seq, kind, payload)
    seq = 0
    idle = list(range(N))
    busy_t = [0.0] * N
    now = 0.0
    live = []            # dispatched, not retired
    retired_upto = -1    # every frame <= this retired (in-order join)
    drv = dict(k=0, state='gateK', t=0.0)
    cls = [0 if (not prio or not F[i]['nonref']) else 1 for i in range(n)]

    def push(t, kind, p):
        nonlocal seq
        heapq.heappush(events, (t, seq, kind, p))
        seq += 1

    def ctb_ready(i, r):
        c = ectb[i][r]
        if c >= C:
            return False
        if r > 0 and ectb[i][r - 1] < min(c + 2, C):
            return False
        if c == 0:
            need = min(R, r + ref_lag)
            for g in F[i]['refs']:
                if pub[g] < need:
                    return False
        return True

    def best_unit():
        best = None
        for i in live:
            key0 = (cls[i], i)
            if best is not None and key0 > best[0][:2]:
                continue
            u = None
            for r in range(R):
                if not erun[i][r] and ectb[i][r] < C and ctb_ready(i, r):
                    u = ((cls[i], i, r), ('row', i, r))
                    break
            if u is None and not dbk_run[i] and dbk[i] < R and \
                    erows[i] >= min(dbk[i] + 2, R):
                u = ((cls[i], i, R + dbk[i]), ('dbk', i, dbk[i]))
            if u is None and not pub_run[i] and pub[i] < R and \
                    dbk[i] >= min(pub[i] + 2, R):
                u = ((cls[i], i, 2 * R + pub[i]), ('pub', i, pub[i]))
            if u is None and not ent_run[i] and pub[i] == R:
                u = ((cls[i], i, 3 * R), ('ent', i, 0))
            if u is not None and (best is None or u[0] < best[0]):
                best = u
        return best[1] if best else None

    def start(w, u):
        kind, i, r = u
        if kind == 'row':
            erun[i][r] = True
            d = F[i]['cost'][r][ectb[i][r]]
        elif kind == 'dbk':
            dbk_run[i] = True
            d = F[i]['dbk'][r]
        elif kind == 'pub':
            pub_run[i] = True
            d = F[i]['pub'][r]
        else:
            ent_run[i] = True
            d = F[i]['ent']
        d /= speed[w]
        busy_t[w] += d
        push(now + d, 'w', (w, u))

    def assign():
        while idle:
            u = best_unit()
            if u is None:
                return
            # fastest idle worker first
            idle.sort()
            start(idle.pop(0), u)

    def driver_step():
        nonlocal retired_upto
        while drv['k'] < n:
            k = drv['k']
            st = drv['state']
            if st == 'gateK':
                if k - K > retired_upto:
                    return
                drv['state'] = 'pre'
                push(now + F[k]['pre'], 'd', 'pre')
                return
            if st == 'pre_done':
                if D is not None and k - 1 - D > retired_upto:
                    return
                drv['state'] = 'post'
                push(now + F[k]['post'], 'd', 'post')
                return
            if st == 'post_done':
                disp[k] = True
                live.append(k)
                if inflight_log is not None:
                    inflight_log.append((now, len(live)))
                drv['k'] += 1
                drv['state'] = 'gateK'
                continue
            return

    driver_step()
    assign()
    while events:
        now, _, kind, p = heapq.heappop(events)
        if kind == 'd':
            drv['state'] = p + '_done'
        else:
            w, (uk, i, r) = p
            cont = None
            if uk == 'row':
                ectb[i][r] += 1
                erun[i][r] = False
                if ectb[i][r] == C:
                    erows[i] += 1
                elif ctb_ready(i, r):
                    cont = ('row', i, r)
            elif uk == 'dbk':
                dbk[i] += 1
                dbk_run[i] = False
            elif uk == 'pub':
                pub[i] += 1
                pub_run[i] = False
            else:
                ent_run[i] = False
                done[i] = True
                tdone[i] = now
                live.remove(i)
                if inflight_log is not None:
                    inflight_log.append((now, len(live)))
                while retired_upto + 1 < n and done[retired_upto + 1]:
                    retired_upto += 1
            if cont:
                start(w, cont)
            else:
                idle.append(w)
        driver_step()
        assign()
    wall = now
    work = sum(busy_t)
    return dict(wall=wall, util=work / (wall * N) if wall else 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--threads', type=int, nargs='+', default=[3])
    ap.add_argument('--k', type=int, default=12)
    ap.add_argument('--lag', nargs='+', default=['0'])
    ap.add_argument('--ecore', type=float, default=1.0)
    ap.add_argument('--noprio', action='store_true')
    a = ap.parse_args()
    T = load(a.trace)
    F = T['F']
    ctbw = sum(sum(map(sum, x['cost'])) for x in F)
    oth = sum(sum(x['dbk']) + sum(x['pub']) + x['ent'] for x in F)
    drvt = sum(x['pre'] + x['post'] for x in F)
    post = sum(x['post'] for x in F)
    print(f"# {a.trace}: {len(F)} frames, R {T['R']} C {T['C']}, measured wall "
          f"{T['wall']:.3f}s; CTB work {ctbw:.2f}s, dbk/pub/ent {oth:.2f}s, "
          f"driver {drvt:.2f}s (post-drain {post:.2f}s), ref stalls {T['rstall']:.3f}s")
    for lg in a.lag:
        D = None if lg == 'inf' else int(lg)
        for N in a.threads:
            r = simulate(T, N, a.k, D, a.ecore, prio=not a.noprio)
            print(f"threads {N:2d} K {a.k} lag {lg:>3}: wall {r['wall']:.3f}s "
                  f"util {100 * r['util']:.1f}%")


if __name__ == '__main__':
    main()

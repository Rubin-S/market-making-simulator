"""Step 9: every pre-registered hypothesis with a 95% CI, a one-sided p, and a Holm-adjusted p (Addendum A7).
Run on dev first (a preview, to test the code on real data), then ONCE on the holdout with --holdout.
Needs, for the same split: research/albers.py, research/run_probes.py, research/run_arms.py and
research/economics.py already run (it reads build/albers, build/probes and results/hypotheses_<tag>.csv).

Unit = one minute of one day (minute 0 is burn-in and is dropped). Orders count in the minute they were
sent, fills in the minute they happened. CIs: the same 5-minute moving-block bootstrap as economics.py
(2,000 resamples, seed 20261020, within each day). Confirmed = the 95% CI is on the predicted side.
A hypothesis with several parts needs every part; its p is the largest part p (intersection-union).
Holm runs over the 11 pre-registered hypotheses. H-Q4c(i)m (S3m, post-hoc) is shown but kept out of Holm.
Output: results/verdicts_<tag>.csv.  Usage (repo root): .venv/bin/python research/verdicts.py [--holdout]"""
import calendar, csv, json, math, os, sys
from collections import defaultdict
import numpy as np
from days import split

np.seterr(invalid='ignore', divide='ignore')   # a resample can hold no orders in a bin: its value is NaN and dropped

DAYS, TAG = split()
RULES = ['riskadverse', 'power3', 'prorata', 'power2']
HEAD, MIN_CELL, MIN_BIN = 'prorata', 30, 100
B, L, SEED, NU = 2000, 5, 20261020, 119             # NU = minutes 1..119 of the 00:00-02:00 UTC file


def unit(day, ts_us):                               # minute of the day, 1..119, or -1 (burn-in / outside)
    t0 = calendar.timegm((int(day[:4]), int(day[4:6]), int(day[6:]), 0, 0, 0)) * 1_000_000
    u = (int(ts_us) - t0) // 60_000_000
    return u - 1 if 1 <= u <= NU else -1


rng = np.random.default_rng(SEED)
IDX = (rng.integers(0, NU - L + 1, size=(len(DAYS), B, -(-NU // L)))[..., None] + np.arange(L)).reshape(len(DAYS), B, -1)[..., :NU]


def boot(a):
    """a: (days, NU, k) per-unit sums -> point (k,), resampled (B, k), pooled over days"""
    return a.sum((0, 1)), sum(a[i][IDX[i]].sum(1) for i in range(len(DAYS)))


def ci(x):
    x = x[np.isfinite(x)]
    return np.percentile(x, 2.5), np.percentile(x, 97.5)


def need(path):
    if not os.path.exists(path):
        sys.exit(f'missing {path}: run the earlier scripts with the same split first')
    return path


# ---- read the S0 touch runs (albers.py) ----
def mo(f):
    s, px = (1 if f['side'] == 'bid' else -1), int(f['px_tick'])
    return s * (int(f['mid_1s_half']) / 2 - px) / px * 1e4


ORD, FIL = defaultdict(list), defaultdict(list)    # rule -> [(day_i, unit, row)]
for i, day in enumerate(DAYS):
    for rule in RULES:
        d = f'build/albers/{day}_{rule}'
        od = {}
        for r in csv.DictReader(open(need(f'{d}/orders.csv'))):
            od[r['order_id']] = r
            u = unit(day, r['t_new_us'])
            if r['masked'] == '0' and r['outcome'] in 'fc' and u >= 0:
                ORD[rule].append((i, u, r))
        for r in csv.DictReader(open(need(f'{d}/fills.csv'))):
            u = unit(day, r['ts_exch_us'])
            if r['masked'] == '0' and r['mid_1s_half'] and u >= 0:
                r['q_size_lot'] = int(od[r['order_id']]['q_size_lot'])
                FIL[rule].append((i, u, r))


def arr(items, k, fn):                              # fn(row) -> length-k vector added to its unit
    a = np.zeros((len(DAYS), NU, k))
    for i, u, r in items:
        a[i, u] += fn(r)
    return a


H = []                                              # one dict per test row


def add(name, test, pt, bs, pred, lo_thr=None, hi_thr=None, part_of=None, note=''):
    """pred '<' (CI below hi_thr), '>' (CI above lo_thr) or 'in' (CI inside [lo_thr, hi_thr])"""
    lo, hi = ci(bs)
    if pred == '<':
        ok, p = hi < hi_thr, float(np.mean(bs >= hi_thr))
    elif pred == '>':
        ok, p = lo > lo_thr, float(np.mean(bs <= lo_thr))
    else:
        ok, p = lo >= lo_thr and hi <= hi_thr, float(max(np.mean(bs <= lo_thr), np.mean(bs >= hi_thr)))
    H.append(dict(hypothesis=name, part_of=part_of or name, test=test, estimate=round(float(pt), 4),
                  ci_lo=round(float(lo), 4), ci_hi=round(float(hi), 4),
                  predicted=f'< {hi_thr:g}' if pred == '<' else f'> {lo_thr:g}' if pred == '>' else f'in [{lo_thr:g}, {hi_thr:g}]',
                  ok=ok, p=p, note=note))


# H-Q1a: headline fill rate in [0.45, 0.65], and riskadverse < power2 (paired)
fr = {}
for rule in RULES:
    pt, bs = boot(arr(ORD[rule], 2, lambda r: np.array([r['outcome'] == 'f', 1.0])))
    fr[rule] = (pt[0] / pt[1], bs[:, 0] / bs[:, 1])
add('H-Q1a', 'S0 fill rate (prorata)', *fr[HEAD], 'in', 0.45, 0.65)
add('H-Q1a', 'fill rate riskadverse - power2', fr['riskadverse'][0] - fr['power2'][0],
    fr['riskadverse'][1] - fr['power2'][1], '<', hi_thr=0.0)


# H-Q1b: OLS slope of P(fill) on own-side imbalance, over the 10 bins with >= 100 orders (bins fixed at point)
def binvec(r):
    v = np.zeros(20); k = min(int((float(r['imb_own']) + 1) * 5), 9)
    v[k] += 1; v[10 + k] += r['outcome'] == 'f'
    return v


pt, bs = boot(arr(ORD[HEAD], 20, binvec))
keep = [k for k in range(10) if pt[k] >= MIN_BIN]
x = np.array([-1 + 0.2 * k + 0.1 for k in keep]); xc = x - x.mean()


def slope(S):
    y = S[..., [10 + k for k in keep]] / S[..., keep]
    return ((y - y.mean(-1, keepdims=True)) * xc).sum(-1) / (xc ** 2).sum()


add('H-Q1b', 'imbalance slope of P(fill) (prorata)', slope(pt), slope(bs), '<', hi_thr=0.0)

# H-Q3a: mean 1 s markout of S0 fills < 0
pt, bs = boot(arr(FIL[HEAD], 2, lambda r: np.array([mo(r), 1.0])))
add('H-Q3a', 'S0 mean 1 s markout bp (prorata)', pt[0] / pt[1], bs[:, 0] / bs[:, 1], '<', hi_thr=0.0)


# H-Q3b: P(fill | 5 s return < 0) - P(fill | > 0) >= 0.4
def retvec(r):
    v = np.zeros(4)
    if r['mid_5s_half']:
        ret = (1 if r['side'] == 'bid' else -1) * (int(r['mid_5s_half']) - int(r['mid_new_half']))
        f = r['outcome'] == 'f'
        if ret < 0: v[0] += 1; v[1] += f
        elif ret > 0: v[2] += 1; v[3] += f
    return v


pt, bs = boot(arr(ORD[HEAD], 4, retvec))
g = lambda S: S[..., 1] / S[..., 0] - S[..., 3] / S[..., 2]
add('H-Q3b', 'P(fill|r<0) - P(fill|r>0) (prorata)', g(pt), g(bs), '>', lo_thr=0.4)


# H-Q3c / H-Q3d: queue-size terciles (fixed at point, per rule) x position at fill
def cellfn(c1, c2):
    def f(r):
        v = np.zeros(18)
        size = 0 if r['q_size_lot'] <= c1 else 1 if r['q_size_lot'] <= c2 else 2
        qp = float(r['qp_before']); pos = 0 if qp < 1 / 3 else 1 if qp < 2 / 3 else 2
        c = 3 * size + pos
        v[c] += mo(r); v[9 + c] += 1
        return v
    return f


front_back = {}
for rule in RULES:
    qs = sorted(r['q_size_lot'] for _, _, r in FIL[rule])
    pt, bs = boot(arr(FIL[rule], 18, cellfn(qs[len(qs) // 3], qs[2 * len(qs) // 3])))
    with np.errstate(invalid='ignore', divide='ignore'):
        means_pt, means_bs = pt[:9] / pt[9:], bs[:, :9] / bs[:, 9:]
    fb_pt, fb_bs = means_pt[6] - means_pt[8], means_bs[:, 6] - means_bs[:, 8]     # large: front - back
    front_back[rule] = ci(fb_bs)[0] > 0
    if rule == HEAD:
        fb = (fb_pt, fb_bs)
        cells = [c for c in range(9) if pt[9 + c] >= MIN_CELL]
        mx = (means_pt[cells].max(), means_bs[:, cells].max(1))
agree = all(front_back.values())
add('H-Q3c', 'large queue: front - back markout bp (prorata)', *fb, '>', lo_thr=0.0,
    note='all 4 rules agree' if agree else 'model-dependent (not all 4 rules agree)')
add('H-Q3d', f'max cell mean markout bp ({len(cells)} cells n>={MIN_CELL})', *mx, '<', hi_thr=0.0)

# H-C1: probes, prorata 10 ms: k12 < 0.5 k01, k01 = ln(l0/l1), k12 = ln(l1/l2)
F3, rest = np.zeros((len(DAYS), NU, 3)), np.zeros(3)
for i, day in enumerate(DAYS):
    for dep in range(3):
        d = f'build/probes/{day}_prorata_10ms_d{dep}'
        s = json.load(open(need(f'{d}/summary.json')))
        rest[dep] += s['resting_s_bid'] + s['resting_s_ask']
        for r in csv.DictReader(open(need(f'{d}/fills.csv'))):
            u = unit(day, r['ts_exch_us'])
            if r['masked'] == '0' and u >= 0:
                F3[i, u, dep] += 1
pt, bs = boot(F3)


def c1(S):
    with np.errstate(invalid='ignore', divide='ignore'):
        lam = S / rest
        return np.log(lam[..., 1] / lam[..., 2]) - 0.5 * np.log(lam[..., 0] / lam[..., 1])


add('H-C1', 'k12 - 0.5 k01 (probes, prorata 10 ms)', c1(pt), c1(bs), '<', hi_thr=0.0,
    note='probe resting time spread evenly over minutes (probes rest almost all the time)')

# ---- the economic hypotheses come from economics.py (same bootstrap scheme) ----
E = {r['hypothesis']: r for r in csv.DictReader(open(need(f'results/hypotheses_{TAG}.csv')))}
for k, name in (('H-Q3e', 'H-Q3e'), ('H-Q4a', 'H-Q4a'), ('H-Q4c(i)', 'H-Q4c'), ('H-Q4c(ii)', 'H-Q4c'),
                ('H-L1 s1t', 'H-L1'), ('H-L1 s2', 'H-L1'), ('H-L1 s3', 'H-L1'), ('H-L1 s1', None),
                ('H-Q4c(i)m', None)):
    e = E[k]
    H.append(dict(hypothesis=k, part_of=name or 'not in Holm', test=e['test'], estimate=float(e['estimate']),
                  ci_lo=float(e['ci_lo']), ci_hi=float(e['ci_hi']), predicted=e['predicted'],
                  ok=e['verdict'] in ('confirmed', 'on predicted side'), p=float(e['p_one_sided']), note=e['note']))

# ---- per hypothesis: all parts, p = max part p, then Holm over the 11 ----
PRIMARY = ['H-Q1a', 'H-Q1b', 'H-Q3a', 'H-Q3b', 'H-Q3c', 'H-Q3d', 'H-Q3e', 'H-Q4a', 'H-Q4c', 'H-L1', 'H-C1']
summ = {h: dict(ok=all(r['ok'] for r in H if r['part_of'] == h), p=max(r['p'] for r in H if r['part_of'] == h))
        for h in PRIMARY}
order = sorted(PRIMARY, key=lambda h: summ[h]['p'])
run = 0.0
for j, h in enumerate(order):
    run = max(run, min(1.0, (len(PRIMARY) - j) * summ[h]['p']))
    summ[h]['holm'] = run
good, bad = ('confirmed', 'not confirmed') if TAG == 'holdout' else ('on predicted side (preview)', 'not shown (preview)')
out = []
for r in H:
    s = summ.get(r['part_of'])
    out.append(dict(hypothesis=r['hypothesis'], part_of=r['part_of'], test=r['test'], estimate=round(r['estimate'], 4),
                    ci_lo=round(r['ci_lo'], 4), ci_hi=round(r['ci_hi'], 4), predicted=r['predicted'],
                    part_on_side=r['ok'], p_part=round(r['p'], 4),
                    verdict=(good if s['ok'] else bad) if s else 'reported only',
                    p_hyp=round(s['p'], 4) if s else '', p_holm=round(s['holm'], 4) if s else '', note=r['note']))
os.makedirs('results', exist_ok=True)
with open(f'results/verdicts_{TAG}.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(out[0])); w.writeheader(); w.writerows(out)

print(f'{TAG}: {len(DAYS)} days, 95% CI = 5-min moving-block bootstrap ({B}x), Holm over {len(PRIMARY)}')
print(f"{'part':12s} {'test':46s} {'est':>8s} {'95% CI':>19s} {'predicted':>14s} {'p':>6s}")
for r in out:
    print(f"{r['hypothesis']:12s} {r['test'][:46]:46s} {r['estimate']:8.3f} [{r['ci_lo']:7.3f},{r['ci_hi']:7.3f}] "
          f"{r['predicted']:>14s} {r['p_part']:6.3f}  {'ok' if r['part_on_side'] else '--'} {r['note']}")
print(f"\n{'hypothesis':10s} {'p':>6s} {'Holm p':>7s}  verdict")
for h in PRIMARY:
    print(f"{h:10s} {summ[h]['p']:6.3f} {summ[h]['holm']:7.3f}  {good if summ[h]['ok'] else bad}")
print(f'\nwrote results/verdicts_{TAG}.csv')

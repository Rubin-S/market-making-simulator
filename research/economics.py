"""Step 8: economics of every arm on the DEV days, with bootstrap CIs (README "Statistics" + Addendum A6).
Reads the runs research/run_arms.py left in build/arms/ (each needs pnl_min.csv, fills.csv, inv_hist.csv).

Unit = one minute of one day (pnl_min.csv rows). For each unit: fills, notional, spread capture (SC),
1 s markout (MO), fees, and inventory (INV) = equity change - SC - MO + fees, so the units add up
EXACTLY to each run's total P&L (checked). INV here is the time-local move on the position held.
Primary CI: 5-minute moving-block bootstrap = blocks of 5 consecutive minutes, starts drawn uniformly,
resampled within each day to that day's length; 2,000 resamples, seed 20261020; the SAME draws are used
for every run, so differences between arms are paired. Also shown: day-block bootstrap (resample days).
Per-fill numbers are bp of notional. Fee tiers: VIP-0 = +2 bp, 0 fee, -0.5 bp rebate (per fill side).
Outputs: results/economics_dev.csv (per arm x rule x latency), results/hypotheses_dev.csv (headline).
DEV ONLY: "confirmed" is decided on the holdout. Here it is a preview.
Usage (repo root): .venv/bin/python research/economics.py"""
import bisect, csv, glob, json, os, re, sys
from collections import defaultdict
import numpy as np

U = 0.1 * 0.001                                    # USDT per tick*lot
B, L, SEED = 2000, 5, 20261020
FEES = {'vip0': 2.0, 'fee0': 0.0, 'rebate': -0.5}  # maker fee, bp of notional
EPS = 1e-6                                         # summary.json prints totals to 6 decimals
F, NOT, SC, MO, INV, FEE = range(6)                # columns of a unit array

runs = defaultdict(dict)                            # (arm, rule, lat) -> day -> (units x 6) array
eq2 = defaultdict(lambda: [0.0, 0.0])               # (arm, rule, lat) -> [sum q^2 * s, sum s]
for d in sorted(glob.glob('build/arms/2026*_*')):
    m = re.match(r'.*/(\d{8})_(\w+?)_(riskadverse|prorata|power2|naive)_(\d+)ms$', d)
    day, arm, rule, lat = m.group(1), m.group(2), m.group(3), int(m.group(4))
    rows = list(csv.DictReader(open(f'{d}/pnl_min.csv')))
    t = [int(r['t_us']) for r in rows]
    eq = np.array([float(r['equity_usdt']) for r in rows])
    M = int(rows[-1]['mid_half'])
    a = np.zeros((len(rows) - 1, 6))
    bounds = t[1:-1]                                # a fill at ts goes to unit bisect_right(bounds, ts)
    for f in csv.DictReader(open(f'{d}/fills.csv')):
        k = bisect.bisect_right(bounds, int(f['ts_exch_us']))
        s, px, q = (1 if f['side'] == 'bid' else -1), int(f['px_tick']), int(f['qty_lot'])
        m0 = int(f['mid_before_half'])
        m1 = int(f['mid_1s_half']) if f['mid_1s_half'] else M
        a[k, F] += 1
        a[k, NOT] += px * q * U
        a[k, SC] += s * q * (m0 - 2 * px) / 2 * U
        a[k, MO] += s * q * (m1 - m0) / 2 * U
        a[k, FEE] += 0.0002 * px * q * U
    a[:, INV] = np.diff(eq) - a[:, SC] - a[:, MO] + a[:, FEE]
    total = json.load(open(f'{d}/summary.json'))['pnl_usdt']['total']
    if abs(np.diff(eq).sum() - total) > EPS:
        sys.exit(f'{d}: minute units add to {np.diff(eq).sum():.9f}, summary total {total:.9f}')
    runs[(arm, rule, lat)][day] = a
    for r in csv.DictReader(open(f'{d}/inv_hist.csv')):
        e = eq2[(arm, rule, lat)]
        e[0] += int(r['abs_pos_lots']) ** 2 * float(r['seconds']); e[1] += float(r['seconds'])
if not runs:
    sys.exit('no runs in build/arms: run research/run_arms.py first')
DAYS = sorted({d for v in runs.values() for d in v})
n_units = {d: len(next(v[d] for v in runs.values() if d in v)) for d in DAYS}
print(f'minute units add up exactly on all {sum(len(v) for v in runs.values())} runs; '
      f'days {DAYS}, units {list(n_units.values())}')

# ---- the draws, shared by every run ----
rng = np.random.default_rng(SEED)
IDX = {}                                            # day -> (B x n) unit indices, moving blocks of L
for d in DAYS:
    n = n_units[d]; nb = -(-n // L)
    starts = rng.integers(0, n - L + 1, size=(B, nb))
    IDX[d] = (starts[:, :, None] + np.arange(L)).reshape(B, -1)[:, :n]
DAYIDX = rng.integers(0, len(DAYS), size=(B, len(DAYS)))


def pooled(key):
    """point sums (6,), block-bootstrap sums (B, 6), day-bootstrap sums (B, 6), pooled over days"""
    v = runs[key]
    pt = sum(v[d].sum(0) for d in DAYS)
    bs = sum(v[d][IDX[d]].sum(1) for d in DAYS)
    per_day = np.stack([v[d].sum(0) for d in DAYS])
    ds = per_day[DAYIDX].sum(1)
    return pt, bs, ds


def metrics(S, hours):
    """S: (..., 6) sums -> dict of metrics (same leading shape)"""
    n = S[..., NOT]
    with np.errstate(invalid='ignore', divide='ignore'):
        g = (S[..., SC] + S[..., MO] + S[..., INV])
        out = dict(fstar=(S[..., SC] + S[..., MO]) / n * 1e4, gross=g / n * 1e4, gross_h=g / hours,
                   fills_h=S[..., F] / hours)
    for k, fee in FEES.items():
        out[f'net_{k}'] = out['gross'] - fee
    return out


def ci(x):
    x = x[np.isfinite(x)]
    return (np.percentile(x, 2.5), np.percentile(x, 97.5)) if len(x) else (np.nan, np.nan)


HOURS = {k: sum(len(v[d]) for d in DAYS) / 60 for k, v in runs.items()}   # minute units after burn-in
cache = {}
for key in runs:
    pt, bs, ds = pooled(key)
    cache[key] = (metrics(pt, HOURS[key]), metrics(bs, HOURS[key]), metrics(ds, HOURS[key]))

rows = []
for key in sorted(runs, key=lambda k: (k[0], k[1], k[2])):
    P, Bm, Dm = cache[key]
    v = runs[key]
    net = np.concatenate([v[d][:, SC] + v[d][:, MO] + v[d][:, INV] - v[d][:, FEE] for d in DAYS])
    sharpe = net.mean() / net.std(ddof=1) * np.sqrt(60) if net.std() > 0 else np.nan
    sb = np.concatenate([(v[d][:, SC] + v[d][:, MO] + v[d][:, INV] - v[d][:, FEE])[IDX[d]] for d in DAYS], axis=1)
    with np.errstate(invalid='ignore', divide='ignore'):
        sharpe_b = sb.mean(1) / sb.std(1, ddof=1) * np.sqrt(60)
    dd = 0.0
    for d in DAYS:                                  # worst intraday drawdown of net VIP-0 equity
        c = np.concatenate([[0], np.cumsum(v[d][:, SC] + v[d][:, MO] + v[d][:, INV] - v[d][:, FEE])])
        dd = max(dd, float((np.maximum.accumulate(c) - c).max()))
    r = dict(arm=key[0], rule=key[1], latency_ms=key[2], fills=int(sum(v[d][:, F].sum() for d in DAYS)),
             fills_per_hour=round(float(P['fills_h']), 1))
    for mname in ['fstar', 'gross', 'net_vip0', 'net_fee0', 'net_rebate', 'gross_h']:
        r[mname] = round(float(P[mname]), 4)
        lo, hi = ci(Bm[mname]); r[f'{mname}_lo'], r[f'{mname}_hi'] = round(lo, 4), round(hi, 4)
        lo, hi = ci(Dm[mname]); r[f'{mname}_daylo'], r[f'{mname}_dayhi'] = round(lo, 4), round(hi, 4)
    lo, hi = ci(sharpe_b)
    r.update(sharpe_hourly=round(float(sharpe), 3), sharpe_lo=round(lo, 3), sharpe_hi=round(hi, 3),
             max_dd_usdt=round(dd, 4), mean_q2_lots2=round(eq2[key][0] / eq2[key][1], 2))
    rows.append(r)
os.makedirs('results', exist_ok=True)
with open('results/economics_dev.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)

# ---- hypotheses at the headline (prorata, 10 ms); dev preview ----
def get(arm, m, rule='prorata', lat=10):
    P, Bm, Dm = cache[(arm, rule, lat)]
    return P[m], Bm[m], Dm[m]


def hyp(name, desc, pt, bs, ds, side, thr=0.0, note=''):
    lo, hi = ci(bs); dlo, dhi = ci(ds)
    ok = lo > thr if side == '>' else hi < thr
    verdict = 'not testable' if 'not testable' in note else 'on predicted side' if ok else 'not shown'
    p = float(np.mean(bs <= thr)) if side == '>' else float(np.mean(bs >= thr))
    return dict(hypothesis=name, test=desc, estimate=round(float(pt), 4), ci_lo=round(lo, 4), ci_hi=round(hi, 4),
                day_ci_lo=round(dlo, 4), day_ci_hi=round(dhi, 4), predicted=f'{side} {thr:g}',
                dev_preview=verdict, p_one_sided=round(p, 4), note=note)


H = []
H.append(hyp('H-Q3e', 'S0 f* (bp)', *get('s0', 'fstar'), '<'))
d = [x - y for x, y in zip(get('s2', 'net_vip0'), get('s0', 'net_vip0'))]
H.append(hyp('H-Q4a', 'S2 - S0 net VIP-0 per fill (bp)', *d, '<'))
d = [x - y for x, y in zip(get('s3', 'net_vip0'), get('s3r', 'net_vip0'))]
H.append(hyp('H-Q4c(i)', 'S3 - S3r net per fill (bp)', *d, '>', 0.1))
H.append(hyp('H-Q4c(ii)', 'S3 net VIP-0 per fill (bp)', *get('s3', 'net_vip0'), '<'))
d = [x - y for x, y in zip(get('s3', 'net_vip0'), get('s3m', 'net_vip0'))]
H.append(hyp('H-Q4c(i)m', 'S3 - S3m net per fill (bp)', *d, '>', 0.1, 'post-hoc control (Addendum A5)'))


def cut(arm):                                       # how much gross P&L per hour falls from 0 to 50 ms
    return [x - y for x, y in zip(get(arm, 'gross_h', lat=0), get(arm, 'gross_h', lat=50))]


for arm in ['s1', 's1t', 's2', 's3']:
    d = [x - y for x, y in zip(cut(arm), cut('s0'))]
    H.append(hyp(f'H-L1 {arm}', f'cut(0->50 ms, USDT/h): {arm} - S0', *d, '>',
                 note='S1 never quotes (0 fills): not testable' if arm == 's1' else ''))
with open('results/hypotheses_dev.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(H[0])); w.writeheader(); w.writerows(H)

# ---- print ----
print(f'\nprorata 10 ms (strawman: naive 10 ms), 4 dev days, 95% CI = 5-min moving-block bootstrap ({B}x)')
print(f"{'arm':9s} {'fills/h':>8s} {'f* bp [CI]':>24s} {'net VIP0 bp [CI]':>26s} {'net -0.5bp':>11s} "
      f"{'gross USDT/h [CI]':>27s} {'Sharpe/h':>8s} {'maxDD':>7s} {'E[q2]':>6s}")
for r in rows:
    if (r['rule'], r['latency_ms']) in (('prorata', 10), ('naive', 10)) and r['fills']:
        print(f"{r['arm']:9s} {r['fills_per_hour']:8.1f} {r['fstar']:7.3f} [{r['fstar_lo']:6.3f},{r['fstar_hi']:6.3f}] "
              f"{r['net_vip0']:8.3f} [{r['net_vip0_lo']:7.3f},{r['net_vip0_hi']:7.3f}] {r['net_rebate']:11.3f} "
              f"{r['gross_h']:8.2f} [{r['gross_h_lo']:7.2f},{r['gross_h_hi']:7.2f}] {r['sharpe_hourly']:8.2f} "
              f"{r['max_dd_usdt']:7.2f} {r['mean_q2_lots2']:6.1f}")
print(f"\n{'hypothesis':12s} {'test':38s} {'est':>8s} {'95% CI':>19s} {'day CI':>19s} {'pred':>6s} {'p':>6s}  dev preview")
for h in H:
    print(f"{h['hypothesis']:12s} {h['test']:38s} {h['estimate']:8.3f} [{h['ci_lo']:7.3f},{h['ci_hi']:7.3f}] "
          f"[{h['day_ci_lo']:7.3f},{h['day_ci_hi']:7.3f}] {h['predicted']:>6s} {h['p_one_sided']:6.3f}  "
          f"{h['dev_preview']} {h['note']}")
print('\nwrote results/economics_dev.csv, results/hypotheses_dev.csv  (DEV preview; confirmed only on the holdout)')

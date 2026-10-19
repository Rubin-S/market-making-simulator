"""Step 7a: choose gamma by the PRE-REGISTERED inventory rule (README + Addendum A4), DEV days only.
  gamma = the smallest value on the grid whose 99th-percentile |position| <= Q_max = 10 lots.
How (Addendum A4): arm S1t (Rule T, alpha = 0), prorata, 10 ms round trip, the Q_max cap LIFTED
(qmax = 1e6) so the quantile measures what gamma does on its own (with the cap on, every gamma
passes trivially). |position| is time-weighted (after burn-in, outside funding), pooled over the
4 dev days. Grid: gamma = 10^(-8 + j/3), j = 0..15 (1e-8 .. 1e-3).
This script reads ONLY inventory. It never opens fills.csv or the P&L in summary.json.
Output: config/gamma_dev.json (+ sha256) and a table on screen.
Usage (repo root): .venv/bin/python research/choose_gamma.py"""
import csv, hashlib, json, os, subprocess, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(__file__))
from arm_params import CALIB, write_params

DEV = ['20260401', '20260501', '20260601', '20260701']
RULE, LAT, QMAX = 'prorata', 10, 10
GRID = [10 ** (-8 + j / 3) for j in range(16)]

for d in DEV:
    if not os.path.exists(f'data/{d}.evt'):
        sys.exit(f'missing data/{d}.evt')
calib = json.load(open(CALIB))
os.makedirs('build/gamma', exist_ok=True)


def quantile(hist, p):                             # hist: |q| -> seconds
    tot, acc = sum(hist.values()), 0.0
    for q in sorted(hist):
        acc += hist[q]
        if acc >= p * tot:
            return q
    return max(hist)


rows = []
for gm in GRID:
    prm = write_params(f'build/gamma/s1t_{gm:.3e}.txt', 's1t', gm, RULE, LAT, calib)
    hist, skew = defaultdict(float), None
    for day in DEV:
        out = f'build/gamma/{day}_{gm:.3e}'
        subprocess.run(['build/mm_sim', f'data/{day}.evt', out, RULE, str(LAT), '1000000', '0', prm],
                       check=True, stdout=subprocess.DEVNULL)
        skew = json.load(open(f'{out}/summary.json'))['skew_ticks_per_lot']
        for r in csv.DictReader(open(f'{out}/inv_hist.csv')):
            hist[int(r['abs_pos_lots'])] += float(r['seconds'])
    rows.append(dict(gamma=gm, skew_ticks_per_lot=skew,
                     lean_back_at_lots=0.5 / skew, stop_side_at_lots=1.5 / skew,
                     q50=quantile(hist, 0.50), q90=quantile(hist, 0.90), q99=quantile(hist, 0.99),
                     q_max_seen=max(hist)))
    r = rows[-1]
    print(f"gamma {gm:9.3e}  skew {skew:8.4f} ticks/lot  stop side at |q| > {r['stop_side_at_lots']:9.2f}  "
          f"|q| p50 {r['q50']:5d}  p90 {r['q90']:5d}  p99 {r['q99']:5d}  max {r['q_max_seen']:5d}", flush=True)

ok = [r for r in rows if r['q99'] <= QMAX]
if not ok:
    sys.exit('no gamma on the grid meets p99 |q| <= Q_max: report this, do not widen the grid silently')
pick = ok[0]
res = dict(rule='smallest gamma with time-weighted p99 |q| <= Q_max (Addendum A4)', arm='s1t',
           queue_rule=RULE, latency_ms=LAT, qmax_lots=QMAX, cap_during_selection='lifted (1e6)',
           calib_sha256=hashlib.sha256(open(CALIB, 'rb').read()).hexdigest(),
           gamma=pick['gamma'], skew_ticks_per_lot=pick['skew_ticks_per_lot'], q99_lots=pick['q99'],
           grid=rows)
with open('config/gamma_dev.json', 'w') as f:
    json.dump(res, f, indent=2)
h = hashlib.sha256(open('config/gamma_dev.json', 'rb').read()).hexdigest()
print(f"\nPICK gamma = {pick['gamma']:.3e} (skew {pick['skew_ticks_per_lot']:.4f} ticks/lot, p99 |q| = {pick['q99']})")
print(f'wrote config/gamma_dev.json  sha256 {h}')

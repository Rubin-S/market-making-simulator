"""Step 7: markouts of every arm on the DEV days, split by fill reason (queue / through / cross).
Reads the runs that research/run_arms.py left in build/arms/ (run that first).
markout(tau) = s * (mid tau after the fill - fill price) / fill price, in bp, BEFORE fees
(s = +1 for a buy). tau = 100 ms, 1 s, 5 s, 30 s. Masked fills and horizons past the data end are dropped.
Output: results/markouts_dev.csv (arm x rule x latency x source x horizon: n, mean bp).
Usage (repo root): .venv/bin/python research/markouts.py"""
import csv, glob, os, re
from collections import defaultdict

H = [('mid_100ms_half', '100ms'), ('mid_1s_half', '1s'), ('mid_5s_half', '5s'), ('mid_30s_half', '30s')]
acc = defaultdict(lambda: [0, 0.0])                 # (arm, rule, lat, src, horizon) -> [n, sum bp]
dirs = sorted(glob.glob('build/arms/2026*_*'))
if not dirs:
    raise SystemExit('no runs in build/arms: run research/run_arms.py first')
for d in dirs:
    m = re.match(r'.*/(\d{8})_(\w+?)_(riskadverse|prorata|power2|naive)_(\d+)ms$', d)
    _, arm, rule, lat = m.groups()
    for f in csv.DictReader(open(f'{d}/fills.csv')):
        if f['masked'] == '1':
            continue
        s, px = (1 if f['side'] == 'bid' else -1), int(f['px_tick'])
        for col, h in H:
            if f[col]:
                v = s * (int(f[col]) / 2 - px) / px * 1e4
                for src in (f['src'], 'all'):
                    a = acc[(arm, rule, int(lat), src, h)]
                    a[0] += 1; a[1] += v

rows = [dict(arm=k[0], rule=k[1], latency_ms=k[2], source=k[3], horizon=k[4], n=n, mean_bp=round(t / n, 4))
        for k, (n, t) in sorted(acc.items())]
os.makedirs('results', exist_ok=True)
with open('results/markouts_dev.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]))
    w.writeheader(); w.writerows(rows)

print('mean markout bp (before fees) at the headline: prorata (strawman: naive), 10 ms, 4 dev days pooled')
print(f"{'arm':9s} {'source':8s} {'n':>7s} " + ' '.join(f'{h:>8s}' for _, h in H))
for arm in ['s0', 's1', 's1t', 's2', 's3', 's3r', 'strawman']:
    rule = 'naive' if arm == 'strawman' else 'prorata'
    for src in ('all', 'queue', 'through', 'cross'):
        cells = [acc.get((arm, rule, 10, src, h)) for _, h in H]
        if not cells[1]:
            continue
        print(f"{arm:9s} {src:8s} {cells[1][0]:7d} " +
              ' '.join(f'{c[1] / c[0]:8.3f}' if c else f"{'-':>8s}" for c in cells))
print('wrote results/markouts_dev.csv')

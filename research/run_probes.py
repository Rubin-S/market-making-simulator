"""Step 5a: probe orders on the DEV days only, to measure fill intensity lambda(depth).
A probe = mm_sim in probe mode: 1 lot each side, `depth` ticks behind best, re-posted
when that level moves, no position cap. lambda = fills / seconds resting (funding windows skipped).
Grid: 4 dev days x 4 queue rules x 3 latencies (round trip) x 3 depths = 144 runs.
Output: results/probes_dev.csv (one row per run) and a pooled table on screen.
Usage (repo root): .venv/bin/python research/run_probes.py"""
import csv, json, os, subprocess, sys
from collections import defaultdict

DEV = ['20260401', '20260501', '20260601', '20260701']          # pre-registered dev days, never holdout
RULES = ['riskadverse', 'power3', 'prorata', 'power2']
LAT = [0, 10, 50]                                                # ms round trip (prereg ladder)
DEPTH = [0, 1, 2]

for d in DEV:
    if not os.path.exists(f'data/{d}.evt'):
        sys.exit(f'missing data/{d}.evt')
os.makedirs('results', exist_ok=True)
rows = []
for day in DEV:
    for rule in RULES:
        for lat in LAT:
            for dep in DEPTH:
                out = f'build/probes/{day}_{rule}_{lat}ms_d{dep}'
                subprocess.run(['build/mm_sim', f'data/{day}.evt', out, rule, str(lat), '1000000', str(dep)],
                               check=True, stdout=subprocess.DEVNULL)
                s = json.load(open(f'{out}/summary.json'))
                fills = [r for r in csv.DictReader(open(f'{out}/fills.csv')) if r['masked'] == '0']
                rest = s['resting_s_bid'] + s['resting_s_ask']
                rows.append(dict(day=day, rule=rule, latency_ms=lat, depth=dep, fills=len(fills),
                                 cancels=s['cancels'], resting_s=round(rest, 3),
                                 lambda_per_s=round(len(fills) / rest, 6) if rest else 0.0,
                                 fill_rate=s['fill_rate']))
            print(f'{day} {rule:11s} {lat:2d} ms done', flush=True)

with open('results/probes_dev.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]))
    w.writeheader(); w.writerows(rows)

# pooled over the 4 days: lambda = total fills / total resting seconds
pool = defaultdict(lambda: [0, 0.0])
for r in rows:
    p = pool[(r['rule'], r['latency_ms'], r['depth'])]
    p[0] += r['fills']; p[1] += r['resting_s']
print('\nlambda (fills per resting second), 4 dev days pooled')
print(f"{'rule':12s} {'lat':>4s} {'d=0':>8s} {'d=1':>8s} {'d=2':>8s}   d0>d1>=d2?")
for rule in RULES:
    for lat in LAT:
        lam = [pool[(rule, lat, d)][0] / pool[(rule, lat, d)][1] for d in DEPTH]
        ok = lam[0] > lam[1] >= lam[2]
        print(f'{rule:12s} {lat:3d}  {lam[0]:8.4f} {lam[1]:8.4f} {lam[2]:8.4f}   {"yes" if ok else "NO"}')
print('\nwrote results/probes_dev.csv')

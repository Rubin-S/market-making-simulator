"""Step 7: run every arm on the DEV days and check the P&L split adds up on every run.
Grid (README + Addenda A1, A4):
  arms     S0 naive touch, S1 GLFT (Rule A), S1t / S2 / S3 / S3r / S3m (Rule T; S3m = Addendum A5)
  rules    riskadverse (conservative bound), prorata (headline), power2 (optimistic bound)
  latency  0 / 10 / 50 ms round trip (10 = headline, 0 = physically impossible upper bound)
  + strawman: S0 under the naive trade-through fill rule, at the same 3 latencies
  4 dev days each: 7 x 3 x 3 x 4 + 3 x 4 = 264 runs, JOBS at a time (default 4).
gamma comes ONLY from config/gamma_dev.json (inventory rule); A and k match each run's rule and latency.
Q_max = 10 stays ON here (it was lifted only to choose gamma).
Outputs:
  results/arms_runs_dev.csv  one row per run (day, arm, rule, latency) with the exact P&L split
  results/arms_dev.csv       pooled over the 4 days, per arm x rule x latency
Every P&L number is in bp of traded notional (sum of price x size over all fills), BEFORE fees unless
the column says vip0. f_star = break-even maker fee = (spread capture + 1 s markout) / notional (prereg).
Usage (repo root): .venv/bin/python research/run_arms.py"""
import csv, json, os, subprocess, sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(__file__))
from arm_params import CALIB, write_params

DEV = ['20260401', '20260501', '20260601', '20260701']
ARMS = ['s0', 's1', 's1t', 's2', 's3', 's3r', 's3m']
RULES = ['riskadverse', 'prorata', 'power2']
LATS = [0, 10, 50]
QMAX = 10
USDT_PER_TICK_LOT = 0.1 * 0.001
EPS = 1e-9

for d in DEV:
    if not os.path.exists(f'data/{d}.evt'):
        sys.exit(f'missing data/{d}.evt')
calib = json.load(open(CALIB))
GAMMA = json.load(open('config/gamma_dev.json'))['gamma']
os.makedirs('build/arms/params', exist_ok=True)
os.makedirs('results', exist_ok=True)

jobs = [(d, a, r, l) for d in DEV for a in ARMS for r in RULES for l in LATS] + \
       [(d, 'strawman', 'naive', l) for d in DEV for l in LATS]


def run(job):
    day, arm, rule, lat = job
    out = f'build/arms/{day}_{arm}_{rule}_{lat}ms'
    cmd = ['build/mm_sim', f'data/{day}.evt', out, rule, str(lat), str(QMAX), '0']
    if arm not in ('s0', 'strawman'):
        cmd.append(write_params(f'build/arms/params/{arm}_{rule}_{lat}ms.txt', arm, GAMMA, rule, lat, calib))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    s = json.load(open(f'{out}/summary.json'))
    p = s['pnl_usdt']
    notional = sum(int(f['px_tick']) * int(f['qty_lot']) for f in csv.DictReader(open(f'{out}/fills.csv'))) \
        * USDT_PER_TICK_LOT
    hours = sum(float(r['seconds']) for r in csv.DictReader(open(f'{out}/inv_hist.csv'))) / 3600
    return dict(day=day, arm=arm, rule=rule, latency_ms=lat, hours=round(hours, 4),
                orders=s['orders_new'], cancels=s['cancels'], gtx_rejects=s['gtx_rejects'], fills=s['fills'],
                fills_queue=s['fills_queue'], fills_through=s['fills_through'], fills_cross=s['fills_cross'],
                q_abs_p99=s['q_abs_p99_lots'], final_pos=s['final_position_lots'], notional_usdt=notional,
                spread_capture=p['spread_capture'], markout_1s=p['markout_1s'], inventory=p['inventory'],
                fees=p['fees'], total=p['total'], equity=p['equity_check'],
                split_ok=int(abs(p['total'] - p['equity_check']) < EPS))


JOBS = int(os.environ.get('JOBS', min(os.cpu_count() or 1, 4)))   # parallel runs; raise if you have RAM
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    runs = []
    for i, r in enumerate(ex.map(run, jobs), 1):
        runs.append(r)
        if i % 12 == 0 or i == len(jobs):
            print(f'{i}/{len(jobs)} runs done', flush=True)

with open('results/arms_runs_dev.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(runs[0]))
    w.writeheader(); w.writerows(runs)

# ---- pool the 4 days ----
pool = defaultdict(lambda: defaultdict(float))
for r in runs:
    P = pool[(r['arm'], r['rule'], r['latency_ms'])]
    for k in ('hours', 'orders', 'cancels', 'fills', 'notional_usdt', 'spread_capture', 'markout_1s',
              'inventory', 'fees', 'total'):
        P[k] += r[k]
    P['q_abs_p99_max'] = max(P['q_abs_p99_max'], r['q_abs_p99'])


def bp(x, n):
    return round(x / n * 1e4, 4) if n else ''


rows = []
for (arm, rule, lat), P in pool.items():
    n, gross = P['notional_usdt'], P['spread_capture'] + P['markout_1s'] + P['inventory']
    rows.append(dict(arm=arm, rule=rule, latency_ms=lat, fills=int(P['fills']),
                     fills_per_hour=round(P['fills'] / P['hours'], 1),
                     fill_rate=round(P['fills'] / (P['fills'] + P['cancels']), 4) if P['fills'] + P['cancels'] else '',
                     sc_bp=bp(P['spread_capture'], n), mo1s_bp=bp(P['markout_1s'], n), inv_bp=bp(P['inventory'], n),
                     gross_bp=bp(gross, n), f_star_bp=bp(P['spread_capture'] + P['markout_1s'], n),
                     net_vip0_bp=bp(P['total'], n), gross_usdt_per_hour=round(gross / P['hours'], 4),
                     net_vip0_usdt_per_hour=round(P['total'] / P['hours'], 4), q_abs_p99_max=int(P['q_abs_p99_max'])))
order = {a: i for i, a in enumerate(ARMS + ['strawman'])}
rows.sort(key=lambda r: (order[r['arm']], r['rule'], r['latency_ms']))
with open('results/arms_dev.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]))
    w.writeheader(); w.writerows(rows)

# ---- print ----
bad = [r for r in runs if not r['split_ok']]
print(f'\nP&L split exact on {len(runs) - len(bad)}/{len(runs)} runs' + ('' if not bad else f'  FAILED: {bad[:3]}'))
print(f'gamma = {GAMMA:.3e} (config/gamma_dev.json), Q_max = {QMAX}, 4 dev days pooled, bp of notional\n')
hdr = f"{'arm':9s} {'rule':11s} {'lat':>3s} {'fills':>7s} {'fills/h':>8s} {'fill%':>6s} {'SC':>7s} {'MO1s':>7s} " \
      f"{'INV':>7s} {'gross':>7s} {'f*':>7s} {'netVIP0':>8s} {'p99|q|':>6s}"
print(hdr)
for r in rows:
    if r['rule'] in ('prorata', 'naive') or r['latency_ms'] == 10:
        def c(k, w=7): return f"{r[k]:{w}.3f}" if r[k] != '' else f"{'-':>{w}s}"
        print(f"{r['arm']:9s} {r['rule']:11s} {r['latency_ms']:3d} {r['fills']:7d} {r['fills_per_hour']:8.1f} "
              f"{(r['fill_rate'] if r['fill_rate'] != '' else 0):6.3f} {c('sc_bp')} {c('mo1s_bp')} {c('inv_bp')} "
              f"{c('gross_bp')} {c('f_star_bp')} {c('net_vip0_bp', 8)} {r['q_abs_p99_max']:6d}")
print('\n(shown: every prorata/naive row + every 10 ms row; the full grid is in results/arms_dev.csv)')
print('wrote results/arms_runs_dev.csv, results/arms_dev.csv')
sys.exit(1 if bad else 0)

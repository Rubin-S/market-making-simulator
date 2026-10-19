"""Step 6: replicate the Albers et al. live Binance touch-quoting facts in our simulator (DEV days only).
Strategy: S0 naive touch (1 lot each side at best, cancel only when the touch moves, qmax 10).
Grid: 4 dev days x 4 queue rules x 10 ms round trip (the pre-registered headline latency) = 16 runs.
Every number is POOLED over the 4 days. Orders sent inside a funding window and masked fills are dropped.

Pre-registered checks (README, prereg-v1):
  H-Q1a  touch fill rate = filled / (filled + cancelled) in [0.45, 0.65]; riskadverse <= power2
  H-Q1b  slope of P(fill) on own-side imbalance < 0   (Albers: -0.317)
  H-Q3a  mean 1 s markout of a fill < 0 before fees
  H-Q3b  P(fill | own 5 s return < 0) - P(fill | > 0) >= 0.4   (Albers: ~0.85 vs ~0.18)
  H-Q3c  large queues: front-of-queue fills have better markouts than back ("model-dependent" unless all 4 agree)
  H-Q3d  no queue-size x position cell (n >= 30) has a positive mean 1 s markout
  H-Q3e  naive touch needs a rebate to break even (gross P&L per fill < 0 bp)
Roadmap gate: if the headline (prorata) fill rate is outside 45-65% or the slope is not negative,
debug the engine before Step 7.

Definitions (s = +1 for our bid, -1 for our ask):
  imbalance  imb_own = (own-side top size - other side) / sum, when we SENT the order
  5 s return s * (mid 5 s after sending - mid at sending): negative = price moved INTO our order
  markout    s * (mid 1 s after the fill - fill price) / fill price, in bp (before fees)
  position   qp_before = queue position just before the fill event (0 front, 1 back)
Usage (repo root): .venv/bin/python research/albers.py"""
import csv, json, os, subprocess, sys
from collections import defaultdict

DEV = ['20260401', '20260501', '20260601', '20260701']
RULES = ['riskadverse', 'power3', 'prorata', 'power2']
LAT, QMAX = 10, 10
HEADLINE = 'prorata'
MIN_CELL = 30
USDT_PER_TICK_LOT = 0.1 * 0.001

for d in DEV:
    if not os.path.exists(f'data/{d}.evt'):
        sys.exit(f'missing data/{d}.evt')
os.makedirs('results', exist_ok=True)


def mean(v):
    return sum(v) / len(v) if v else float('nan')


def ols(x, y):                                   # slope and R^2 of y = a + b x
    mx, my = mean(x), mean(y)
    sxx = sum((a - mx) ** 2 for a in x)
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    syy = sum((b - my) ** 2 for b in y)
    b = sxy / sxx
    return b, (sxy * sxy / (sxx * syy) if syy > 0 else float('nan'))


# ---- 1. run S0 and collect orders + fills ----
orders, fills, gross, notional = defaultdict(list), defaultdict(list), defaultdict(float), defaultdict(float)
for day in DEV:
    for rule in RULES:
        out = f'build/albers/{day}_{rule}'
        subprocess.run(['build/mm_sim', f'data/{day}.evt', out, rule, str(LAT), str(QMAX), '0'],
                       check=True, stdout=subprocess.DEVNULL)
        s = json.load(open(f'{out}/summary.json'))
        gross[rule] += s['pnl_usdt']['total'] + s['pnl_usdt']['fees']        # P&L before fees
        od = {}
        for r in csv.DictReader(open(f'{out}/orders.csv')):
            od[r['order_id']] = r
            if r['masked'] == '0' and r['outcome'] in 'fc':
                orders[rule].append(r)
        for r in csv.DictReader(open(f'{out}/fills.csv')):
            notional[rule] += int(r['px_tick']) * int(r['qty_lot']) * USDT_PER_TICK_LOT
            if r['masked'] == '0' and r['mid_1s_half']:
                r['q_size_lot'] = int(od[r['order_id']]['q_size_lot'])
                fills[rule].append(r)
        print(f'{day} {rule:11s} done', flush=True)

# ---- 2. the numbers, per rule ----
rows, table1, drift = [], [], []
front_beats_back = {}
for rule in RULES:
    O, F = orders[rule], fills[rule]
    filled = [1 if o['outcome'] == 'f' else 0 for o in O]
    fill_rate = mean(filled)

    # imbalance: 10 bins on [-1, 1], P(fill) per bin, OLS over bins with >= 100 orders
    bins = defaultdict(list)
    for o, y in zip(O, filled):
        bins[min(int((float(o['imb_own']) + 1) * 5), 9)].append(y)
    xs = [(-1 + 0.2 * k + 0.1) for k in sorted(bins) if len(bins[k]) >= 100]
    ys = [mean(bins[k]) for k in sorted(bins) if len(bins[k]) >= 100]
    beta, r2 = ols(xs, ys)

    # 5 s return after sending
    up, down = [], []
    for o, y in zip(O, filled):
        if not o['mid_5s_half']:
            continue
        r = (1 if o['side'] == 'bid' else -1) * (int(o['mid_5s_half']) - int(o['mid_new_half']))
        if r < 0: down.append(y)
        elif r > 0: up.append(y)
    p_down, p_up = mean(down), mean(up)

    # markouts (bp, before fees)
    def mo(f, key='mid_1s_half'):
        sgn = 1 if f['side'] == 'bid' else -1
        px = int(f['px_tick'])
        return sgn * (int(f[key]) / 2 - px) / px * 1e4
    m1 = [mo(f) for f in F]
    by_src = {src: mean([mo(f) for f in F if f['src'] == src]) for src in ('queue', 'through', 'cross')}

    # Table 1: queue size terciles (of this rule's fills) x position at fill
    qs = sorted(f['q_size_lot'] for f in F)
    c1, c2 = qs[len(qs) // 3], qs[2 * len(qs) // 3]
    cell = defaultdict(list)
    for f, m in zip(F, m1):
        size = 'small' if f['q_size_lot'] <= c1 else 'mid' if f['q_size_lot'] <= c2 else 'large'
        qp = float(f['qp_before'])
        pos = 'front' if qp < 1 / 3 else 'middle' if qp < 2 / 3 else 'back'
        cell[(size, pos)].append(m)
    positive_cells = 0
    for size in ('small', 'mid', 'large'):
        for pos in ('front', 'middle', 'back'):
            v = cell[(size, pos)]
            table1.append(dict(rule=rule, queue_size=size, position=pos, n=len(v),
                               mean_markout_1s_bp=round(mean(v), 4) if v else ''))
            if len(v) >= MIN_CELL and mean(v) > 0:
                positive_cells += 1
    lf, lb = cell[('large', 'front')], cell[('large', 'back')]
    front_beats_back[rule] = (len(lf) >= MIN_CELL and len(lb) >= MIN_CELL and mean(lf) > mean(lb))

    # post-fill drift: mean signed mid move (ticks) from just before the fill
    for key, h in (('mid_100ms_half', '100ms'), ('mid_1s_half', '1s'), ('mid_5s_half', '5s'), ('mid_30s_half', '30s')):
        v = [(1 if f['side'] == 'bid' else -1) * (int(f[key]) - int(f['mid_before_half'])) / 2 for f in F if f[key]]
        drift.append(dict(rule=rule, horizon=h, n=len(v), mean_drift_ticks=round(mean(v), 3)))

    gross_bp = gross[rule] / notional[rule] * 1e4                          # per fill, before fees
    rows.append(dict(rule=rule, orders=len(O), fill_rate=round(fill_rate, 4), imb_slope=round(beta, 4),
                     imb_r2=round(r2, 3), p_fill_ret_down=round(p_down, 4), p_fill_ret_up=round(p_up, 4),
                     fills=len(F), markout_1s_bp=round(mean(m1), 4),
                     markout_queue_bp=round(by_src['queue'], 4), markout_through_bp=round(by_src['through'], 4),
                     markout_cross_bp=round(by_src['cross'], 4), positive_cells=positive_cells,
                     gross_per_fill_bp=round(gross_bp, 4), round_trip_bp_before_fees=round(2 * gross_bp, 4),
                     round_trip_bp_vip0=round(2 * gross_bp - 4.0, 4)))

for name, data in (('albers_dev.csv', rows), ('albers_table1_dev.csv', table1), ('albers_drift_dev.csv', drift)):
    with open(f'results/{name}', 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(data[0]))
        w.writeheader(); w.writerows(data)

# ---- 3. print ----
print(f'\nS0 touch, {LAT} ms round trip, qmax {QMAX}, 4 dev days pooled')
print(f"{'rule':12s} {'fill%':>6s} {'slope':>7s} {'R2':>5s} {'P|r<0':>6s} {'P|r>0':>6s} {'mo1s bp':>8s} "
      f"{'gross/fill':>10s} {'RT bp':>7s} {'RT VIP0':>8s}")
for r in rows:
    print(f"{r['rule']:12s} {r['fill_rate']:6.3f} {r['imb_slope']:7.3f} {r['imb_r2']:5.2f} "
          f"{r['p_fill_ret_down']:6.3f} {r['p_fill_ret_up']:6.3f} {r['markout_1s_bp']:8.3f} "
          f"{r['gross_per_fill_bp']:10.3f} {r['round_trip_bp_before_fees']:7.3f} {r['round_trip_bp_vip0']:8.3f}")
print('\nmean 1 s markout (bp) by fill source:')
for r in rows:
    print(f"  {r['rule']:12s} queue {r['markout_queue_bp']:7.3f}  through {r['markout_through_bp']:7.3f}  "
          f"cross {r['markout_cross_bp']:7.3f}")
print(f'\nTable 1 ({HEADLINE}): mean 1 s markout bp [n], rows = queue size tercile, cols = position at fill')
for size in ('small', 'mid', 'large'):
    cells = [t for t in table1 if t['rule'] == HEADLINE and t['queue_size'] == size]
    print(f'  {size:6s}' + ''.join(f"  {c['position']} {c['mean_markout_1s_bp'] if c['n'] else '-':>8} [{c['n']}]"
                                   for c in cells))
print(f'\npost-fill drift ({HEADLINE}), mean signed mid move in ticks:',
      ', '.join(f"{d['horizon']} {d['mean_drift_ticks']}" for d in drift if d['rule'] == HEADLINE))

H = {r['rule']: r for r in rows}
h = H[HEADLINE]
agree = all(front_beats_back.values())
checks = [
    ('H-Q1a fill rate in [0.45, 0.65] (prorata)', 0.45 <= h['fill_rate'] <= 0.65),
    ('H-Q1a riskadverse <= power2', H['riskadverse']['fill_rate'] <= H['power2']['fill_rate']),
    ('H-Q1b imbalance slope < 0 (prorata)', h['imb_slope'] < 0),
    ('H-Q3a mean 1 s markout < 0 (prorata)', h['markout_1s_bp'] < 0),
    ('H-Q3b P(fill|r<0) - P(fill|r>0) >= 0.4 (prorata)', h['p_fill_ret_down'] - h['p_fill_ret_up'] >= 0.4),
    ('H-Q3c large queue: front better than back (prorata)', front_beats_back[HEADLINE]),
    ('H-Q3d no cell with n >= 30 positive (prorata)', h['positive_cells'] == 0),
    ('H-Q3e naive needs a rebate (gross per fill < 0)', h['gross_per_fill_bp'] < 0),
]
print()
for name, ok in checks:
    print(f'  {"PASS" if ok else "FAIL"}  {name}')
print(f'  H-Q3c across all 4 rules: {"all agree" if agree else "model-dependent"} '
      f'({", ".join(r for r, v in front_beats_back.items() if v) or "none"} front > back)')
gate = 0.45 <= h['fill_rate'] <= 0.65 and h['imb_slope'] < 0
print(f'\nROADMAP GATE (fill rate 45-65% and slope < 0): {"PASS, go to Step 7" if gate else "FAIL, debug the engine first"}')
print('wrote results/albers_dev.csv, albers_table1_dev.csv, albers_drift_dev.csv')

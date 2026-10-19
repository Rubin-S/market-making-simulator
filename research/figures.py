"""Step 10a: the three README figures, from the frozen results (dev + holdout). No new runs.
  fig1_fill_models.png    the same S0 quotes under 4 fill models: break-even fee f* (95% CI), dev vs holdout
  fig2_albers.png         (a) P(fill) vs own-side imbalance, (b) markout decay by fill source (holdout)
  fig3_breakeven_fee.png  break-even maker fee f* per arm (holdout, 95% CI) against Binance fee tiers
Usage (repo root): .venv/bin/python research/figures.py"""
import csv, glob, os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

BG, INK = '#e8e8ea', '#1d1d1f'
A50, A30, REF, GRID = INK + '80', INK + '4d', INK + '24', INK + '12'
COL = ['#b1bcd2', '#607868', '#dbb691', '#a79fb8', '#d9c489', '#7e9c9b', '#c9a3a8']
ACCENT = '#ff4d00'
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 12, 'axes.facecolor': BG, 'figure.facecolor': BG,
                     'axes.edgecolor': A30, 'axes.labelcolor': A50, 'xtick.color': A50, 'ytick.color': A50,
                     'axes.spines.top': False, 'axes.spines.right': False, 'xtick.major.size': 0,
                     'ytick.major.size': 0, 'axes.grid': True, 'axes.grid.axis': 'y', 'grid.color': GRID,
                     'grid.linewidth': 1, 'legend.frameon': False, 'savefig.dpi': 200})
MONO = {'family': 'DejaVu Sans Mono', 'size': 11}
os.makedirs('figures', exist_ok=True)


def read(name):
    return list(csv.DictReader(open(f'results/{name}')))


def title(fig, main, sub):
    fig.text(0.04, 0.965, main, fontsize=15, color=INK, weight='bold', va='top')
    fig.text(0.04, 0.905, sub, fontsize=11.5, color=A50, va='top')


def ref(ax, y, label, color=REF, tcolor=A50):
    ax.axhline(y, color=color, lw=1.2, ls='--', zorder=3)
    ax.text(1.0, y, f' {label}', transform=ax.get_yaxis_transform(), color=tcolor, va='center', fontsize=10.5)


def eco(tag):
    return {(r['arm'], r['rule'], int(r['latency_ms'])): r for r in read(f'economics_{tag}.csv')}


E = {t: eco(t) for t in ('dev', 'holdout')}

# ---- Figure 1: the fill model sets the answer ----
models = [('s0', 'riskadverse', 'RiskAdverse\n(conservative)'), ('s0', 'prorata', 'Prorata\n(headline)'),
          ('s0', 'power2', 'Power2\n(optimistic)'), ('strawman', 'naive', 'Naive\n(strawman)')]
fig, ax = plt.subplots(figsize=(9, 5.4)); fig.subplots_adjust(left=0.1, right=0.86, top=0.8, bottom=0.16)
w = 0.36
for j, (tag, c) in enumerate((('dev', A50), ('holdout', INK))):
    for i, (arm, rule, _) in enumerate(models):
        r = E[tag][(arm, rule, 10)]
        y, lo, hi = float(r['fstar']), float(r['fstar_lo']), float(r['fstar_hi'])
        x = i + (j - 0.5) * w
        ax.bar(x, y, w * 0.92, color=c, zorder=2)
        ax.errorbar(x, y, [[y - lo], [hi - y]], color=BG if j else INK, lw=1.2, capsize=0, zorder=4)
        ax.text(x, lo - 0.03, f'{y:.2f}', ha='center', va='top', color=A50, fontdict=MONO)
ax.axhline(0, color=REF, lw=1.2)
ref(ax, -0.5, 'Best Binance\nrebate −0.5 bp', ACCENT, ACCENT)
ax.set_xticks(range(len(models)), [m[2] for m in models], fontsize=12.5, color=INK)
ax.set_ylabel('break-even maker fee f* (bp)')
ax.set_ylim(min(float(E[t][(a, r, 10)]['fstar_lo']) for t in E for a, r, _ in models) * 1.25, 0.15)
ax.text(0.01, 0.04, '■ dev (4 days)   ', transform=ax.transAxes, color=A50, fontsize=11)
ax.text(0.2, 0.04, '■ holdout (3 days)', transform=ax.transAxes, color=INK, fontsize=11)
title(fig, 'The fill model decides whether quoting the touch can pay',
      'Break-even fee f* of the same S0 quotes, 95% CI. 10 ms, BTCUSDT perp, 00:00–02:00 UTC.')
fig.savefig('figures/fig1_fill_models.png'); plt.close(fig)

# ---- Figure 2: Albers replication ----
fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 5.2)); fig.subplots_adjust(left=0.07, right=0.88, top=0.79, bottom=0.14, wspace=0.32)
from days import DEV, HOLDOUT
for days, tag, c in ((DEV, 'dev', A50), (HOLDOUT, 'holdout', INK)):
    n, f = [0] * 10, [0] * 10
    for day in days:
        for r in csv.DictReader(open(f'build/albers/{day}_prorata/orders.csv')):
            if r['masked'] == '0' and r['outcome'] in 'fc':
                k = min(int((float(r['imb_own']) + 1) * 5), 9)
                n[k] += 1; f[k] += r['outcome'] == 'f'
    ks = [k for k in range(10) if n[k] >= 100]
    xs, ys = [-0.9 + 0.2 * k for k in ks], [f[k] / n[k] for k in ks]
    a1.plot(xs, ys, color=c, lw=1.8, solid_capstyle='round')
    a1.text(xs[-1] + 0.04, ys[-1] + (0.03 if tag == 'dev' else -0.03), tag, color=c, va='center', fontsize=11)
a1.set_xlabel('own-side imbalance when the order was sent'); a1.set_ylabel('P(fill)')
a1.set_xlim(-1, 1.15); a1.set_ylim(0, 1)
a1.set_title('a. Heavy own side = filled less', loc='left', color=INK, fontsize=12.5)
M = [r for r in read('markouts_holdout.csv') if r['arm'] == 's0' and r['rule'] == 'prorata' and r['latency_ms'] == '10']
H = ['100ms', '1s', '5s', '30s']
for i, src in enumerate(['queue', 'through', 'cross']):
    v = {r['horizon']: float(r['mean_bp']) for r in M if r['source'] == src}
    ys = [v[h] for h in H]
    a2.plot(range(4), ys, color=COL[i], lw=2, solid_capstyle='round')
    a2.text(3.08, ys[-1], src, color=INK, va='center', fontsize=11)
a2.axhline(0, color=REF, lw=1.2)
a2.set_xticks(range(4), ['100 ms', '1 s', '5 s', '30 s']); a2.set_xlim(-0.2, 3.6)
a2.set_ylabel('mean markout after the fill (bp)')
a2.set_title('b. Every fill source loses, and keeps losing', loc='left', color=INK, fontsize=12.5)
title(fig, 'The simulator reproduces Albers et al.’s live Binance facts',
      'S0 touch quotes, Prorata fills, 10 ms round trip. (b) is the holdout: Aug 1, Sep 1, Oct 1 2026.')
fig.savefig('figures/fig2_albers.png'); plt.close(fig)

# ---- Figure 3: break-even fee per arm vs fee tiers ----
arms = [('s0', 'S0 touch'), ('s1t', 'S1t GLFT'), ('s2', 'S2 lean'), ('s3', 'S3 fade'), ('s3m', 'S3m ctrl'),
        ('s3r', 'S3r ctrl'), ('strawman', 'Strawman')]
fig, ax = plt.subplots(figsize=(9.5, 5.6)); fig.subplots_adjust(left=0.1, right=0.83, top=0.8, bottom=0.12)
for i, (arm, name) in enumerate(arms):
    r = E['holdout'][(arm, 'naive' if arm == 'strawman' else 'prorata', 10)]
    y, lo, hi = float(r['fstar']), float(r['fstar_lo']), float(r['fstar_hi'])
    ax.plot([i, i], [lo, hi], color=A50 if arm == 'strawman' else INK, lw=2.2, solid_capstyle='round', zorder=3)
    ax.plot(i, y, 'o', color=A50 if arm == 'strawman' else INK, ms=7, zorder=4)
    ax.text(i + 0.14, y, f'{y:.2f}', va='center', color=A50, fontdict=MONO)
for y, lab, c, tc in ((2.0, 'VIP 0 maker +2 bp', REF, A50), (0.0, 'zero fee', REF, A50),
                      (-0.5, 'best rebate −0.5 bp', ACCENT, ACCENT)):
    ref(ax, y, lab, c, tc)
ax.set_xticks(range(len(arms)), [a[1] for a in arms], fontsize=12, color=INK)
ax.set_ylabel('break-even maker fee f* (bp)'); ax.set_ylim(-1.0, 2.3); ax.set_xlim(-0.5, len(arms) - 0.4)
title(fig, 'No realistic arm breaks even, even at the best maker rebate',
      'Holdout (6 h), Prorata fills, 10 ms. Dot = break-even fee f*, line = 95% CI. Profit needs a fee below the dot.')
fig.savefig('figures/fig3_breakeven_fee.png'); plt.close(fig)
print('wrote figures/fig1_fill_models.png, figures/fig2_albers.png, figures/fig3_breakeven_fee.png')

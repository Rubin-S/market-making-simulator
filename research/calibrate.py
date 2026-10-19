"""Step 5b: calibrate on the DEV days only, then freeze config/calib_dev.json.
  sigma : signature plot of the mid (sampling 0.1 s .. 30 s); take the shortest horizon
          whose variance rate is within 10% of the 30 s one ("flat"). Units: ticks / sqrt(s).
  A, k  : from results/probes_dev.csv (Step 5a). A = lambda0 (fills per second per side),
          k01 = ln(l0/l1), k12 = ln(l1/l2) (the fit check, see Addendum A1).
  G*    : Stoikov (2018) microprice adjustment by best-level imbalance I = bid/(bid+ask), 10 bins,
          one-tick-spread states, symmetrised. Reported with a convergence check, because the mid
          often jumps several ticks per update and the Markov sum may not settle.
  G_tau : the direct version, E[mid(t+1 s) - mid(t) | I bin], symmetrised, in ticks. Used for
          alpha only if G* does not converge (see Addendum A2).
  kappa : one shrink so that alpha = kappa * G(I) predicts the 1 s mid change (least squares
          through 0). A forecast fit, never P&L.
  jumps : how far the mid moves per change (why 1 and 2 ticks back fill alike).
  gamma : not here. Step 6 sets it by the inventory rule once Rule T exists.
Masked everywhere: first 60 s of each day, +-30 s around 00:00/08:00/16:00 UTC.
Usage (repo root): .venv/bin/python research/calibrate.py"""
import csv, hashlib, json, math, os, subprocess, sys
from collections import defaultdict
import numpy as np
import polars as pl

DEV = ['20260401', '20260501', '20260601', '20260701']          # pre-registered dev days only
H = [0.1, 0.2, 0.5, 1, 2, 5, 10, 30]                             # signature-plot horizons, s
NB = 10                                                          # imbalance bins
TAU = 1_000_000                                                  # tau* = 1 s (prereg), us
BURN, FUND_EVERY, FUND = 60_000_000, 8 * 3600 * 1_000_000, 30_000_000


def masked(t, t0):
    m = t % FUND_EVERY
    return (t < t0 + BURN) | (m < FUND) | (m > FUND_EVERY - FUND)


def load(day):
    out = f'build/bbo/{day}.csv'
    if not os.path.exists(out):
        os.makedirs('build/bbo', exist_ok=True)
        subprocess.run(['build/mm_bbo', f'data/{day}.evt', out], check=True)
    d = pl.read_csv(out)
    t = d['ts_exch_us'].to_numpy()
    return t, d['bid_tick'].to_numpy(), d['ask_tick'].to_numpy(), d['bid_lot'].to_numpy(), d['ask_lot'].to_numpy()


def mid_asof(t, mid2, q):
    """mid (half ticks) as of times q: last row with t <= q."""
    return mid2[np.searchsorted(t, q, side='right') - 1]


def calibrate(days, probes_csv):
    sig_ss = defaultdict(float); sig_T = defaultdict(float)     # signature: sum of squares, seconds
    Qc = np.zeros((NB, NB)); Tc = np.zeros((NB, NB)); Rc = np.zeros(NB)   # Stoikov counts
    kap_y, bin_of, jumps = [], [], []
    for day in days:
        t, b, a, bq, aq = load(day)
        mid2 = b + a                                             # half ticks
        t0 = t[0]
        # ---- signature plot ----
        for h in H:
            g = np.arange(t0 + BURN, t[-1], int(h * 1e6))
            m = mid_asof(t, mid2, g) / 2.0                       # ticks
            dm = np.diff(m)
            ok = ~(masked(g[:-1], t0) | masked(g[1:], t0))
            sig_ss[h] += float(np.sum(dm[ok] ** 2)); sig_T[h] += float(ok.sum() * h)
        # ---- Stoikov states: one-tick spread rows only ----
        one = (a - b) == 1
        imb = bq / np.maximum(bq + aq, 1)
        s = np.minimum((imb * NB).astype(int), NB - 1)
        keep = one & ~masked(t, t0)
        i = np.nonzero(keep[:-1] & keep[1:])[0]                  # consecutive pairs, both kept
        dM = (mid2[i + 1] - mid2[i]) / 2.0                       # ticks
        jumps.append(np.abs(dM[dM != 0]))
        for src, dst, d in ((s[i], s[i + 1], dM), (NB - 1 - s[i], NB - 1 - s[i + 1], -dM)):   # + mirror
            same = d == 0
            np.add.at(Qc, (src[same], dst[same]), 1)
            np.add.at(Tc, (src[~same], dst[~same]), 1)
            np.add.at(Rc, src[~same], d[~same])
        # ---- kappa data: G* bin now vs mid change over tau* ----
        r = np.nonzero(keep & (t + TAU <= t[-1]))[0]
        fwd = (mid_asof(t, mid2, t[r] + TAU) - mid2[r]) / 2.0
        bin_of.append(s[r]); kap_y.append(fwd)
        print(f'{day}: {len(t)} rows, {len(i)} Stoikov pairs', flush=True)

    sig = {h: sig_ss[h] / sig_T[h] for h in H}                   # ticks^2 per second
    flat = next(h for h in H if abs(sig[h] / sig[30] - 1) <= 0.10)

    n = Qc.sum(1) + Tc.sum(1)
    P_Q, P_T, R1 = Qc / n[:, None], Tc / n[:, None], Rc / n
    inv = np.linalg.inv(np.eye(NB) - P_Q)
    G1, B = inv @ R1, inv @ P_T
    G, term, it, G6 = G1.copy(), G1.copy(), 0, None
    while it < 200:
        term = B @ term; G += term; it += 1
        if it == 6: G6 = G.copy()
        if np.max(np.abs(term)) < 1e-6: break
    if G6 is None: G6 = G.copy()                                 # converged in under 6 steps
    converged = bool(np.max(np.abs(term)) < 1e-6 and np.max(np.abs(G)) < 1.0)   # bounded by a tick

    sb, y = np.concatenate(bin_of), np.concatenate(kap_y)
    m = np.array([y[sb == k].mean() for k in range(NB)])
    Gt = (m - m[::-1]) / 2.0                                     # symmetrised 1 s forecast, ticks
    use = G if converged else Gt
    x = use[sb]
    kappa = float(np.dot(x, y) / np.dot(x, x))
    j = np.concatenate(jumps)

    lam = defaultdict(lambda: [0, 0.0])
    for row in csv.DictReader(open(probes_csv)):
        p = lam[(row['rule'], int(row['latency_ms']), int(row['depth']))]
        p[0] += int(row['fills']); p[1] += float(row['resting_s'])
    probes = {}
    for (rule, lat, dep) in sorted(lam):
        if dep: continue
        l = [lam[(rule, lat, d)][0] / lam[(rule, lat, d)][1] for d in (0, 1, 2)]
        probes.setdefault(rule, {})[f'{lat}ms'] = dict(A=round(l[0], 6), k01=round(math.log(l[0] / l[1]), 6),
                                                       k12=round(math.log(l[1] / l[2]), 6))
    return dict(
        days=days,
        sigma_ticks_per_sqrt_s=round(math.sqrt(sig[flat]), 6), sigma_horizon_s=flat,
        signature_ticks2_per_s={str(h): round(v, 6) for h, v in sig.items()},
        probes=probes,
        gstar_ticks={'bins': NB, 'imbalance': 'bid_lot/(bid_lot+ask_lot)', 'converged': converged,
                     'iterations': it, 'G1': [round(v, 6) for v in G1], 'after_6': [round(v, 6) for v in G6],
                     'final': [round(v, 6) for v in G]},
        g_tau_ticks=[round(v, 6) for v in Gt],
        alpha_source='gstar' if converged else 'g_tau',
        kappa=round(kappa, 6), tau_s=TAU / 1e6,
        mid_jumps={'changes': int(len(j)), 'share_ge_2_ticks': round(float((j >= 2).mean()), 4),
                   'median_ticks': float(np.median(j)), 'p90_ticks': float(np.percentile(j, 90))},
        gamma='set in Step 6 by the inventory rule (99th pct |q| <= Q_max), never P&L')


if __name__ == '__main__':
    for d in DEV:
        if not os.path.exists(f'data/{d}.evt'): sys.exit(f'missing data/{d}.evt')
    cal = calibrate(DEV, 'results/probes_dev.csv')
    os.makedirs('config', exist_ok=True)
    txt = json.dumps(cal, indent=2) + '\n'
    open('config/calib_dev.json', 'w').write(txt)
    print('\nsignature (ticks^2/s):', cal['signature_ticks2_per_s'])
    print(f"sigma = {cal['sigma_ticks_per_sqrt_s']} ticks/sqrt(s), flat from {cal['sigma_horizon_s']} s")
    g = cal['gstar_ticks']
    print(f"G* converged: {g['converged']} ({g['iterations']} iterations), top bin after 6 = {g['after_6'][-1]}, final = {g['final'][-1]}")
    print('G_tau (1 s) by imbalance bin (ticks):', cal['g_tau_ticks'])
    print('alpha source:', cal['alpha_source'], '  kappa =', cal['kappa'])
    print('mid jumps:', cal['mid_jumps'])
    print('prorata 10ms:', cal['probes']['prorata']['10ms'])
    print('sha256(config/calib_dev.json) =', hashlib.sha256(txt.encode()).hexdigest())

"""Sanity check of build/bbo/*.csv: does each dev day cover 24 h, evenly?
Prints rows, time span, gaps and rows per UTC hour. No prices, no P&L.
Usage (repo root): .venv/bin/python research/check_bbo.py"""
import datetime as dt
import numpy as np
import polars as pl

for day in ['20260401', '20260501', '20260601', '20260701']:
    t = pl.read_csv(f'build/bbo/{day}.csv')['ts_exch_us'].to_numpy()
    g = np.diff(t)
    utc = lambda us: dt.datetime.fromtimestamp(us / 1e6, dt.timezone.utc).strftime('%H:%M:%S')
    k = int(np.argmax(g))
    per_h = np.bincount(((t // 3_600_000_000) % 24).astype(int), minlength=24)
    print(f'{day}: {len(t)} rows, {utc(t[0])} -> {utc(t[-1])} UTC ({(t[-1] - t[0]) / 3.6e9:.2f} h), '
          f'median gap {np.median(g) / 1e3:.0f} ms, max gap {g[k] / 1e6:.1f} s at {utc(t[k])}')
    print('   rows per UTC hour:', ' '.join(str(x) for x in per_h))

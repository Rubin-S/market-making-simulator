"""Step 3b: run the C++ compat mode over the same 16 configs and compare fill by fill with hftbacktest.
Needs: out/ from run_oracle.py, and ../build/mm_compat (make sim).
Pass rule per config: identical fills (side, tick, exchange time), identical order ids, |d equity| < 1e-6 USDT.
Usage: python3 run_compat.py"""
import json, subprocess, sys
import numpy as np
from compare import compare

BIN = 'data/tardis_btcusdt_slice.bin'
# hftbacktest's own event array, dumped raw (64 bytes per event) so C++ reads exactly the same input
np.load('data/tardis_btcusdt_slice.npz')['data'].tofile(BIN)

passed = 0
for clock in ('single', 'real'):
    for q in ('riskadverse', 'power3_2', 'prorata', 'power2_2'):
        for lat in (0, 10):
            tag = f'T_{clock}_{q}_{lat}ms'
            subprocess.run(['../build/mm_compat', BIN, 'out', tag, clock, q, str(lat), '60'], check=True,
                           stdout=subprocess.DEVNULL)
            c = compare(tag, 'cppfills')
            ok = (c['match_rate'] == 1.0 and c['n_oracle'] == c['n_cand'] and c['same_ids']
                  and abs(c['d_equity_net']) < 1e-6)
            passed += ok
            print(f"{tag:28s} hbt {c['n_oracle']:4d}  cpp {c['n_cand']:4d}  matched {c['matched']:4d}  "
                  f"same_ids {str(c['same_ids']):5s}  d_equity {c['d_equity_net']:+.2e}  {'PASS' if ok else 'FAIL'}")
            if not ok:
                print('   first difference (index, hbt row, cpp row):', c['first_diff'])
print(f'{passed}/16 configs agree' + ('  ->  ORACLE AGREES' if passed == 16 else ''))
sys.exit(0 if passed == 16 else 1)

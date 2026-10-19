"""Step 3c: our OWN simulator (mm_sim: engine book + MakerSide) vs hftbacktest, same Oct 1 slice.
Tier A only (one clock), so it is compared with the T_single_* oracle runs.
Latency: hbt uses 10 ms each way; mm_sim takes the ROUND TRIP and halves it, so we pass 20.
Position cap off (qmax huge), like hbt.
Two numbers per config:
  recall    = share of hbt's fills that mm_sim also makes (same side, price, exchange time)
  precision = share of mm_sim's fills that hbt also makes
Pass rule: recall >= 97% in all 8 configs, and precision >= 95% at 10 ms.
At 0 ms mm_sim makes extra fills on purpose (see the printed note), so precision is reported, not gated.
Usage: python3 run_own.py   (needs ../engine/build/mdr_convert and ../build/mm_sim)"""
import csv, os, subprocess, sys
from collections import Counter

EVT = 'data/oct1_slice.evt'
if not os.path.exists(EVT):   # the same two cut CSVs hbt converted, through OUR converter
    subprocess.run(['../engine/build/mdr_convert', 'data/cut_incremental_book_L2.csv', 'data/cut_trades.csv', EVT],
                   check=True)

RULES = {'riskadverse': 'riskadverse', 'power3': 'power3_2', 'prorata': 'prorata', 'power2': 'power2_2'}
ok_all = True
for ours, hbt in RULES.items():
    for lat in (0, 10):
        out = f'out/own_{ours}_{lat}ms'
        subprocess.run(['../build/mm_sim', EVT, out, ours, str(2 * lat), '1000000'], check=True,
                       stdout=subprocess.DEVNULL)
        A = [(int(r['side']), int(r['price_tick']), int(r['exch_fill_ts']) // 1000)       # ns -> us
             for r in csv.DictReader(open(f'out/fills_T_single_{hbt}_{lat}ms.csv'))]
        rows = list(csv.DictReader(open(f'{out}/fills.csv')))
        B = [(1 if r['side'] == 'bid' else -1, int(r['px_tick']), int(r['ts_exch_us'])) for r in rows]
        left = Counter(A); extra = Counter(); matched = 0
        for r, k in zip(rows, B):
            if left[k] > 0: left[k] -= 1; matched += 1
            else: extra[r['src']] += 1
        recall, precision = matched / len(A), matched / len(B)
        ok = recall >= 0.97 and (lat == 0 or precision >= 0.95)
        ok_all &= ok
        print(f"{ours:12s} {lat:2d} ms  hbt {len(A):4d}  own {len(B):4d}  matched {matched:4d}  "
              f"recall {recall:6.1%}  precision {precision:6.1%}  own-only by source {dict(extra)}  "
              f"{'PASS' if ok else 'FAIL'}")
print('note: at 0 ms, after a fill hbt waits one extra timestamp before re-quoting (its wake rule),\n'
      '      mm_sim re-quotes at once and gets run over again by the same sweep: extra through/cross fills.')
print('OWN vs ORACLE: PASS' if ok_all else 'OWN vs ORACLE: FAIL')
sys.exit(0 if ok_all else 1)

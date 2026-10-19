"""Step 3a: run the hftbacktest oracle over the 16-config matrix on the Oct 1 slice.
4 queue rules x 2 latencies (0, 10 ms) x 2 clocks (single, real) = 16 configs. One process = one numba compile.
Outputs per config in out/: fills_<tag>.csv, events_<tag>.csv, summary_<tag>.json, plus out/matrix.json.
Usage: python3 run_oracle.py"""
import json, os
from naive_touch import run

FILES = ['data/tardis_btcusdt_slice.npz']
WARMUP_S = 60.0                      # same 60 s burn-in as mm_sim
os.makedirs('out', exist_ok=True)
rows = []
for clock in ('single', 'real'):
    for q in ('riskadverse', 'power3_2', 'prorata', 'power2_2'):
        for lat in (0, 10):
            tag = f'T_{clock}_{q}_{lat}ms'
            s = run(tag, clock, q, lat, WARMUP_S, FILES)
            rows.append(s)
            print(f"{tag:28s} fills {s['n_fills']:5d}  submits {s['n_submits']:5d}  cancels {s['n_cancels']:5d}  "
                  f"pos {s['final_position']:+.3f}  equity_net {s['equity_net']:+.6f}", flush=True)
json.dump(rows, open('out/matrix.json', 'w'), indent=1)

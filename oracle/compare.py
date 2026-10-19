"""Oracle comparison: fills_<tag>.csv (hftbacktest) vs reffills_<tag>.csv (candidate engine).
Matching key = (side, price_tick, exch_fill_ts) multiset; also checks order ids, counts, position, equity."""
import sys, csv, json
from collections import Counter
def load(p, cols):
    with open(p) as f:
        return [tuple(int(r[c]) for c in cols) for r in csv.DictReader(f)]
def compare(tag, cand_prefix='reffills', outdir='out'):
    A = load(f'{outdir}/fills_{tag}.csv', ['order_id', 'side', 'price_tick', 'exch_fill_ts'])
    B = load(f'{outdir}/{cand_prefix}_{tag}.csv', ['order_id', 'side', 'price_tick', 'exch_fill_ts'])
    ka = Counter(x[1:] for x in A); kb = Counter(x[1:] for x in B)
    common = sum((ka & kb).values())
    sa = json.load(open(f'{outdir}/summary_{tag}.json'))
    sb = json.load(open(f'{outdir}/{cand_prefix.replace("fills","summary")}_{tag}.json'))
    first_diff = None
    for i, (x, y) in enumerate(zip(sorted(A, key=lambda r: (r[3], r[0])), sorted(B, key=lambda r: (r[3], r[0])))):
        if x[1:] != y[1:]:
            first_diff = (i, x, y); break
    return dict(tag=tag, n_oracle=len(A), n_cand=len(B), matched=common,
                match_rate=common / max(len(A), 1), same_ids=(sorted(A) == sorted(B)),
                pos=(sa['final_position'], sb['final_position']),
                d_equity_net=sb['equity_net'] - sa['equity_net'],
                d_equity_gross=sb['equity_gross'] - sa['equity_gross'], first_diff=first_diff)
if __name__ == '__main__':
    pre = 'reffills'
    args = sys.argv[1:]
    if args and args[0].startswith('--prefix='): pre = args[0].split('=')[1]; args = args[1:]
    for t in args: print(json.dumps(compare(t, pre)))

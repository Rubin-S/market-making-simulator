"""check_facts.py - two data facts every P&L number rests on.

1. Is trades.side the TAKER (aggressor) side?
   A taker buy lifts the ask, so it should print at or above the best ask
   that stood just before it. A taker sell hits the bid: at or below the bid.
2. Are trades one row per maker fill, or aggregated?
   One taker order that eats several resting orders arrives as a burst of rows
   with the same exchange timestamp and side. If rows in a burst repeat the
   SAME price, each row is one maker's fill (not aggregated by price).

usage: .venv/bin/python research/check_facts.py 20261001
"""
import sys
import numpy as np
import pandas as pd

tag = sys.argv[1] if len(sys.argv) > 1 else "20261001"

# --- read the .evt file: 32-byte header, then 32-byte events (see engine/include/mdr/event.hpp)
HDR = np.dtype([("magic", "S8"), ("version", "<u2"), ("reserved", "<u2"),
                ("px_scale", "<u4"), ("qty_scale", "<u4"), ("count", "<u4"), ("min_book_px", "<i8")])
EVT = np.dtype([("ts_exch", "<i8"), ("ts_local", "<i8"), ("px", "<i8"), ("qty", "<i4"),
                ("side", "u1"), ("kind", "u1"), ("flags", "u1"), ("reserved", "u1")])
assert HDR.itemsize == 32 and EVT.itemsize == 32

raw = np.fromfile(f"data/{tag}.evt", dtype=np.uint8)
h = raw[:32].view(HDR)[0]
assert h["magic"].startswith(b"MDREVT"), "not an .evt file"
ev = raw[32:].view(EVT)
pxs = float(h["px_scale"])

tr = pd.DataFrame(ev[ev["kind"] == 1])                 # kind 1 = Trade
tr["price"] = tr["px"] / pxs
tr["buy"] = tr["side"] == 0                             # converter: "buy" -> Side::Bid (0)
print(f"day {tag}: {len(tr):,} trades, {tr['buy'].mean():.1%} marked buy")

# --- check 1: compare each trade with the book quote just BEFORE it
book = pd.read_csv(f"data/ofi_{tag}.csv", usecols=["ts_local_us", "mid", "spread"])
book["bid"] = book["mid"] - book["spread"] / 2
book["ask"] = book["mid"] + book["spread"] / 2
m = pd.merge_asof(tr.sort_values("ts_local"), book.sort_values("ts_local_us"),
                  left_on="ts_local", right_on="ts_local_us",
                  allow_exact_matches=False, direction="backward").dropna(subset=["bid"])
eps = 1e-6
buy_at_ask = (m.loc[m["buy"], "price"] >= m.loc[m["buy"], "ask"] - eps).mean()
sell_at_bid = (m.loc[~m["buy"], "price"] <= m.loc[~m["buy"], "bid"] + eps).mean()
print(f"check 1 (taker side): buys at/above ask {buy_at_ask:.2%}, sells at/below bid {sell_at_bid:.2%}")
print("   -> taker side CONFIRMED" if min(buy_at_ask, sell_at_bid) > 0.95 else "   -> NOT confirmed, tell Hark")

# --- check 2: bursts = same ts_exch and side
g = tr.groupby(["ts_exch", "side"])
burst = g["price"].transform("size") > 1
dup_px = tr.duplicated(["ts_exch", "side", "price"], keep=False)
print(f"check 2 (granularity): rows in bursts {burst.mean():.1%}, "
      f"rows sharing ts+side+price with another row {dup_px.mean():.1%}, "
      f"largest burst {g.size().max()} rows")
print("   -> looks PER MAKER FILL (same price repeats in a burst)" if dup_px.mean() > 0.01
      else "   -> looks AGGREGATED by price (aggTrade-like)")

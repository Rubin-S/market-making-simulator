"""Naive touch-quoting oracle run on hftbacktest 2.4.4.

Strategy (the exact spec the C++ compat mode must reproduce in Step 3b):
  * one GTX LIMIT order of QTY=0.001 per side, bid at local best bid, ask at local best ask
  * event driven: wake on every local feed batch, every order response, or a 1 s timeout
  * at each wake, in this order (steps 3-4 only on feed wakes or timeouts, never on order-response wakes):
      1. record fills: every order in the local dict with status FILLED not yet logged
      2. clear_inactive_orders (drops FILLED / CANCELED / EXPIRED)
      3. if t < start + warmup: do nothing else
      4. for each side: if we have an order with status NEW, req NONE and price_tick != target tick -> cancel
         if we have no order on that side -> submit a new one at target tick (order ids 1,2,3,...)
         if an order exists but has a request in flight -> do nothing
  * variant S2 ('sticky', stick=k>0): cancel only when |order tick - target tick| > k (k=0 is the naive S1 rule)
  * no position limit, no skew. Fees: maker 0.0002, taker 0.0005 of notional (Binance VIP 0).
Outputs: fills CSV + summary JSON per config.

Usage: python3 naive_touch.py <tag> <clock: real|single> <queue: riskadverse|power3_2|prorata|power2_2> <latency_ms> <warmup_s> files... [--snapshot f]
"""
import sys, json, time
import numpy as np
from numba import njit
from numba.typed import List
from numba.types import Tuple, int64, float64
from hftbacktest import (BacktestAsset, HashMapMarketDepthBacktest, GTX, LIMIT, BUY, SELL,
                         NEW, FILLED, NONE, EXPIRED, CANCELED, EXCH_EVENT, LOCAL_EVENT)

QTY = 0.001
MAKER, TAKER = 0.0002, 0.0005

# fill row: (order_id, side, price_tick, qty, last_req_local_ts (submit or later cancel time), exch_fill_ts, local_seen_ts, maker)
fill_ty = Tuple((int64, int64, int64, float64, int64, int64, int64, int64))
# event row: (kind 1=submit 2=cancel 3=expired-seen, side, price_tick, local_ts, order_id)
ev_ty = Tuple((int64, int64, int64, int64, int64))


@njit
def naive_touch(hbt, qty, warmup_ns, fills, evlog, logged, stick):
    next_id = 1
    start = -1
    n_wake = 0
    while True:
        r = hbt.wait_next_feed(True, 1_000_000_000)
        if r == 1 or (r != 0 and r != 2 and r != 3):
            break
        n_wake += 1
        now = hbt.current_timestamp
        if start < 0:
            start = now
        orders = hbt.orders(0)
        # 1. log fills / expiries before they are cleared
        vals = orders.values()
        while vals.has_next():
            o = vals.get()
            if int64(o.order_id) not in logged:
                if o.status == FILLED:
                    fills.append((int64(o.order_id), int64(o.side), int64(o.price_tick), float64(o.exec_qty),
                                  int64(o.local_timestamp), int64(o.exch_timestamp), int64(now),
                                  int64(1 if o.exec_price_tick == o.price_tick else 0)))
                    logged[int64(o.order_id)] = 1
                elif o.status == EXPIRED:
                    evlog.append((int64(3), int64(o.side), int64(o.price_tick), int64(now), int64(o.order_id)))
                    logged[int64(o.order_id)] = 1
        # 2. clear
        hbt.clear_inactive_orders(0)
        if now < start + warmup_ns:
            continue
        # act only on market-data wakes (2) or timeouts (0); on order responses (3) only log.
        # Acting on responses livelocks at latency 0 in the real-clock mode: a GTX order that the
        # (ahead-of-local) exchange book rejects is answered at the same timestamp, is resubmitted at
        # the same stale local price, rejected again, ... without time ever advancing.
        if r == 3:
            continue
        depth = hbt.depth(0)
        bb = depth.best_bid_tick
        ba = depth.best_ask_tick
        if bb <= -9223372036854775807 or ba >= 9223372036854775807 or bb >= ba:
            continue
        tick = depth.tick_size
        have_bid = False
        have_ask = False
        orders = hbt.orders(0)
        vals = orders.values()
        while vals.has_next():
            o = vals.get()
            if o.side == BUY:
                have_bid = True
                if o.status == NEW and o.req == NONE and abs(o.price_tick - bb) > stick:
                    hbt.cancel(0, o.order_id, False)
                    evlog.append((int64(2), int64(1), int64(o.price_tick), int64(now), int64(o.order_id)))
            else:
                have_ask = True
                if o.status == NEW and o.req == NONE and abs(o.price_tick - ba) > stick:
                    hbt.cancel(0, o.order_id, False)
                    evlog.append((int64(2), int64(-1), int64(o.price_tick), int64(now), int64(o.order_id)))
        if not have_bid:
            hbt.submit_buy_order(0, next_id, bb * tick, qty, GTX, LIMIT, False)
            evlog.append((int64(1), int64(1), int64(bb), int64(now), int64(next_id)))
            next_id += 1
        if not have_ask:
            hbt.submit_sell_order(0, next_id, ba * tick, qty, GTX, LIMIT, False)
            evlog.append((int64(1), int64(-1), int64(ba), int64(now), int64(next_id)))
            next_id += 1
    depth = hbt.depth(0)
    sv = hbt.state_values(0)
    mid = (depth.best_bid + depth.best_ask) / 2.0
    return n_wake, start, hbt.current_timestamp, sv.position, sv.balance, sv.fee, sv.num_trades, mid


def load(files, clock):
    arrs = [np.load(f)['data'] for f in files]
    data = np.concatenate(arrs)
    if clock == 'single':
        # one clock: keep each original event once (its EXCH copy) and stamp local_ts = exch_ts
        m = (data['ev'] & EXCH_EVENT) == EXCH_EVENT
        data = data[m].copy()
        data['local_ts'] = data['exch_ts']
        data['ev'] = data['ev'] | LOCAL_EVENT
        assert np.all(np.diff(data['exch_ts']) >= 0)
    return data


def run(tag, clock, queue, lat_ms, warmup_s, files, snapshot=None, outdir='out', stick=0):
    data = load(files, clock)
    asset = (BacktestAsset().data(data).linear_asset(1.0)
             .constant_order_latency(int(lat_ms * 1_000_000), int(lat_ms * 1_000_000))
             .no_partial_fill_exchange().trading_value_fee_model(MAKER, TAKER)
             .tick_size(0.1).lot_size(0.001).last_trades_capacity(0))
    snap_arr = None
    if snapshot:
        # keep a Python reference: BacktestAsset stores only a raw pointer to the ndarray
        # (passing np.load(...)['data'] inline segfaults once the temporary is freed)
        snap_arr = np.load(snapshot)['data']
        asset = asset.initial_snapshot(snap_arr)
    # Same 4 rules as the C++ QueueRule bracket (sim/include/mm/queue.hpp):
    if queue == 'riskadverse':
        asset = asset.risk_adverse_queue_model()
    elif queue == 'power3_2':
        asset = asset.power_prob_queue_model3(2.0)     # p_behind = 1 - (front/(front+back))^2
    elif queue == 'prorata':
        asset = asset.power_prob_queue_model(1.0)      # p_behind = back/(back+front)
    elif queue == 'power2_2':
        asset = asset.power_prob_queue_model2(2.0)     # p_behind = (back/(back+front))^2
    else:
        raise ValueError(queue)
    hbt = HashMapMarketDepthBacktest([asset])
    fills = List.empty_list(fill_ty)
    evlog = List.empty_list(ev_ty)
    from numba.typed import Dict
    logged = Dict.empty(key_type=int64, value_type=int64)
    t0 = time.perf_counter()
    n_wake, start, end, pos, bal, fee, ntr, mid = naive_touch(hbt, QTY, int(warmup_s * 1e9), fills, evlog, logged, int64(stick))
    wall = time.perf_counter() - t0
    hbt.close()
    fl = np.array(list(fills), dtype=[('order_id', 'i8'), ('side', 'i8'), ('price_tick', 'i8'), ('qty', 'f8'),
                                       ('last_req_local_ts', 'i8'), ('exch_fill_ts', 'i8'), ('local_seen_ts', 'i8'),
                                       ('maker', 'i8')])
    ev = np.array(list(evlog), dtype=[('kind', 'i8'), ('side', 'i8'), ('price_tick', 'i8'), ('local_ts', 'i8'),
                                      ('order_id', 'i8')])
    import csv
    with open(f'{outdir}/fills_{tag}.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(fl.dtype.names)
        for row in fl:
            w.writerow([int(x) if i != 3 else f'{x:.3f}' for i, x in enumerate(row)])
    with open(f'{outdir}/events_{tag}.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(ev.dtype.names)
        for row in ev:
            w.writerow([int(x) for x in row])
    equity = bal + pos * mid - fee
    gross = bal + pos * mid
    summ = dict(tag=tag, stick=stick, clock=clock, queue=queue, latency_ms=lat_ms, warmup_s=warmup_s, files=files,
                snapshot=snapshot, n_rows=int(len(data)), n_wake=int(n_wake),
                quote_start_ns=int(start + warmup_s * 1e9), end_ns=int(end),
                minutes_quoted=(end - start - warmup_s * 1e9) / 6e10,
                n_submits=int((ev['kind'] == 1).sum()) if len(ev) else 0,
                n_cancels=int((ev['kind'] == 2).sum()) if len(ev) else 0,
                n_expired=int((ev['kind'] == 3).sum()) if len(ev) else 0,
                n_fills=int(len(fl)), n_buy_fills=int((fl['side'] == 1).sum()),
                n_sell_fills=int((fl['side'] == -1).sum()), num_trades_state=int(ntr),
                final_position=round(pos, 6), balance=bal, fee=fee, final_mid=mid,
                equity_net=equity, equity_gross=gross, wall_s=round(wall, 3))
    with open(f'{outdir}/summary_{tag}.json', 'w') as f:
        json.dump(summ, f, indent=1)
    return summ


if __name__ == '__main__':
    args = sys.argv[1:]
    snap = None
    if '--snapshot' in args:
        i = args.index('--snapshot'); snap = args[i + 1]; args = args[:i] + args[i + 2:]
    tag, clock, queue, lat, warm = args[:5]
    s = run(tag, clock, queue, float(lat), float(warm), args[5:], snap)
    print(json.dumps({k: s[k] for k in ('tag', 'n_fills', 'n_buy_fills', 'n_sell_fills', 'final_position',
                                        'equity_gross', 'equity_net', 'n_submits', 'n_cancels', 'n_expired',
                                        'minutes_quoted', 'wall_s')}))

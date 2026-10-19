# Facts the simulator rests on
Checked 2026-10-19. Re-check before any re-run on new data.

## 1. Contract filters (Binance USD-M BTCUSDT perpetual)
Source: GET /fapi/v1/exchangeInfo?symbol=BTCUSDT on fapi.binance.com, 2026-10-19
| Filter | Value |
|---|---|
| Tick size | 0.10 USDT |
| Lot step / min qty / max qty | 0.001 / 0.001 / 1000 BTC |
| Min notional | 50 USDT |
Simulated order size: 1 lot = 0.001 BTC (smallest legal order; above 50,000 USDT
the lot, not the notional, is the binding limit).

## 2. Fees (Regular user / VIP 0, USDT-margined)
Source: Binance USD-M Futures fee rate page and FAQ 360033544231, 2026-10-19
| | Rate | bps |
|---|---|---|
| Maker | 0.0200% | 2.0 |
| Taker | 0.0500% | 5.0 |
No maker rebate at VIP 0. BNB fee discount is NOT applied in headline numbers.
The quoting rule ignores fees; the break-even maker fee is reported instead.

## 3. trades.side is the TAKER (aggressor) side
Check: research/check_facts.py 20261001 (trade vs quote just before it)
- buys at/above ask: 98.58%   - sells at/below bid: 98.59%   (164,109 trades)
Implication: a taker SELL depletes the bid queue and can fill our bid;
a taker BUY fills our ask.

## 4. One trade row per maker fill (not aggregated)
Check: research/check_facts.py 20261001
- rows in same-timestamp bursts: 88.5%
- rows sharing timestamp + side + price with another row: 74.4%
- largest burst: 407 rows
Implication: sum rows by (ts_exch, side, price) to get volume traded at a level
per message; a burst across several prices is a taker sweeping levels.

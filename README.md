# market-making-simulator

A C++20 maker backtester for Binance BTCUSDT perpetuals, built on the verified L2 book from
[l2-orderbook-replay](https://github.com/Rubin-S/l2-orderbook-replay) (pinned at `engine/`).
It has been checked fill for fill against hftbacktest 2.4.4. Results come after the
pre-registration below.

**Status:** engine + oracle done. Research has not started.
- `make test`: 4 C++ test programs pass.
- `oracle/run_compat.py`: compat mode matches hftbacktest in **16/16 configs**, with identical fills and equity within 1e-11 USDT.
- `oracle/run_own.py`: our own simulator reproduces **97.5-98.9%** of hftbacktest's fills in all 8 single-clock configs.

---

## Pre-registration (prereg-v1, 2026-10-19)

Written and tagged **before any strategy arm is run on any day**. Nothing below changes after this
tag. A change becomes a dated addendum, or a v2 pre-registration on new days.

### Disclosure: what was run before this tag
- **2026-07-01 (dev):** S0 naive touch smoke runs at 0 and 50 ms, to check the mechanics (fill counts, P&L identity).
- **2026-10-01 (holdout):** first 10 minutes only, used for the hftbacktest oracle and mechanics checks. Fill sets were compared. **No P&L from this day was used for any decision.** The one engine change it led to ("act once per timestamp") is a mechanics fix that was found by comparing fills, not P&L.
- No strategy arm other than S0 has been run on any day.

### Data
- Tardis free first-of-month files, Binance USDT-M BTCUSDT (`incremental_book_L2` + `trades`).
- **Dev:** 2026-04-01, 05-01, 06-01, 07-01. **Holdout:** 2026-08-01, 09-01, 10-01. The holdout is run **once**, after `freeze-v1`.
- Burn-in: the first 60 s of each day. Mask: fills within ±30 s of each funding time (00:00, 08:00, 16:00 UTC) are excluded from statistics.
- Depth cadence is 52 ms in Apr/May and 26 ms from June on, so results are also reported per regime.
- Order size: 1 lot = 0.001 BTC (the exchange minimum). Tick 0.1 USDT.

### Questions
- **Q1** How much does the fill rule move the answer? Compare the same quotes under 4 queue rules plus a naive strawman.
- **Q2** Does inventory-aware quoting (GLFT) control risk better than naive touch quoting?
- **Q3** How large is adverse selection on maker fills? Markouts by horizon, fill source, queue size and position.
- **Q4** Does skewing quotes on order-book imbalance help, either with the signal or against it?
- **Q5** What maker fee would each strategy break even at, compared with Binance's fee tiers?

### Simulator settings
- **Queue rules:** RiskAdverse ≤ Power3(2) ≤ Prorata = Power(1) ≤ Power2(2), plus the naive strawman. **Headline = Prorata.** RiskAdverse and Power2(2) are reported as the conservative and optimistic bounds.
- **Fill sources:** queue, through, cross. A level that vanishes gives no fill. Post-only (GTX) orders are rejected when they would cross.
- **Latency ladder (round trip, entry = response = half):** 0 / 10 / 50 ms. **Headline = 10 ms**, roughly a bot in the exchange's cloud region. 0 ms is an upper bound only and is physically impossible. 50 ms is a slower setup.
- **Clock:** Tier A (exchange timestamp, no feed latency) is the headline. The strategy acts once per timestamp, on the finished book.

### Arms
| Arm | Quote rule | α̂ (alpha, ticks) |
|---|---|---|
| S0 naive touch | 1 lot at the best bid and the best ask | none, no inventory term |
| S1 GLFT | Rule A, below | 0 |
| S2 with-imbalance | Rule A | +κ·G*(I) |
| S3 contrarian | Rule A | −κ·G*(I) |
| S3r random | Rule A | ±κ·G*(I), sign by coin flip per decision (seed 20261020), the control for H-Q4c |
| Strawman | S0 quotes under the naive trade-through fill rule | none |
| S4 reversal-gated | stretch only; if not built, it moves to v2 | |

**Rule A (discretised GLFT).** With half-spread h = c1 + ½σc2 and skew s = σc2 (GLFT infinite-horizon closed form), the distance behind best in ticks is
z_b = (h − ½) + s·q − α̂ for the bid and z_a = (h − ½) − s·q + α̂ for the ask.
- z ≤ 0.5 → quote at touch; z ≤ 1.5 → one tick back; otherwise don't quote that side.
- **Hysteresis:** cancel/re-post only when the target level changes.
- **Q_max = 10 lots** (0.01 BTC) hard cap on |position|, for every arm.
- **Fees stay out of the quote rule** and are charged in P&L only.

### Calibration (dev days only, frozen to `config/calib_dev.json`, hash recorded at `freeze-v1`)
- σ: signature plot from 100 ms to 30 s; take the shortest flat horizon.
- A, k: probe orders at 0/1/2 ticks behind best, for each queue rule and latency. λ = fills / exposure time, Poisson MLE; level 2 is the fit check.
- G*: Stoikov microprice estimator (symmetrised). One shrink κ is chosen on dev.
- **γ: the smallest γ whose 99th-percentile |q| ≤ Q_max on dev. Never chosen by P&L.**

### P&L and markouts
- Exact identity on every run (to 1e-9): **P&L = spread capture + markout(τ) + inventory − fees.**
- Markouts at 100 ms / 1 s / 5 s / 30 s from the mid just before the fill. Horizons past the data end are dropped, not clipped.
- **τ\* = 1 s** for the split and for the break-even fee f\* = Σ(SC + MO(τ\*)) / Σ m0 × 10⁴ bp.
- **Fees:** VIP-0 (0.02% maker / 0.05% taker) is the headline. Also reported at 0 fee and at the −0.5 bp rebate tier.

### Hypotheses (signs fixed now)
**Confirmed** means the 95% CI is on the predicted side **on the holdout**, under the headline settings (Prorata, 10 ms). Bounds are reported alongside.
- **H-Q1a** S0 probe fill rate (fills / (fills + cancels), 1-lot touch order, cancel only when price moves away) is in [0.45, 0.65], and RiskAdverse < Power2(2).
- **H-Q1b** the imbalance coefficient in a logistic model of fill probability is < 0.
- **H-Q3a** mean 1 s markout of S0 maker fills is < 0, before fees.
- **H-Q3b** for bid probes, P(fill | 5 s mid return < 0) − P(fill | 5 s mid return > 0) ≥ 0.4 (mirrored for asks).
- **H-Q3c** in large queues, front-of-queue fills have better 1 s markouts than back-of-queue. Labelled "model-dependent" unless all 4 rules agree.
- **H-Q3d** no queue-size × position cell has a positive mean 1 s markout.
- **H-Q3e** S0 needs a rebate to break even (f\* < 0).
- **H-Q4a** S2 (with-imbalance) is no better than S0 in net P&L per fill.
- **H-Q4c** S3 beats S3r by > 0.1 bp per fill but still loses at VIP-0.
- **H-L1** going from 0 to 50 ms cuts gross P&L per hour more for S1-S3 (cancel-using) than for S0.

### Statistics
- **Primary CI:** 5-minute moving-block bootstrap, resampled within each day, **2,000 resamples, seed 20261020**.
- **Also shown:** day-block bootstrap. With 3 holdout days it is too coarse to gate on alone.
- All 10 hypotheses are reported, confirmed or not; none is dropped. Holm-adjusted p-values are shown as a secondary column.
- All reportings include inventory variance, Sharpe with error bars, max drawdown.

### Order of work after this tag
calibrate (dev) → Albers replication (dev) → arms + economics (dev) → README addendum + `freeze-v1` → holdout **once** → `holdout-v1` → write-up → `v1.0`.

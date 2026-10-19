# Holdout post-mortem (2026-10-19, written after holdout-v1, before the README results)

Holdout: 2026-08-01, 09-01, 10-01, 00:00-02:00 UTC (6 h), run once on freeze-v1 (52e6887), results d97f5b2.

## Verdicts (95% CI, 5-min moving-block bootstrap, Holm over 11)
| Hypothesis | Holdout estimate [95% CI] | Verdict |
|---|---|---|
| H-Q1a fill rate in [0.45, 0.65]; riskadverse < power2 | 0.559 [0.545, 0.572]; -0.057 | confirmed |
| H-Q1b imbalance slope < 0 | -0.412 [-0.426, -0.395] | confirmed |
| H-Q3a 1 s markout < 0 | -0.594 bp [-0.627, -0.566] | confirmed |
| H-Q3b P(fill\|r<0) - P(fill\|r>0) >= 0.4 | 0.770 [0.733, 0.814] | confirmed |
| H-Q3c large queue front > back (all 4 rules) | +0.556 bp [0.467, 0.672] | confirmed |
| H-Q3d no cell positive | max -0.378 bp [-0.411, -0.343] | confirmed |
| H-Q3e S0 needs a rebate (f* < 0) | -0.596 bp [-0.630, -0.568] | confirmed |
| H-Q4a S2 worse than S0 | +0.070 bp [-0.429, 0.480] | not confirmed |
| H-Q4c S3 beats S3r by > 0.1 bp, and loses at VIP-0 | +0.002 bp [-0.152, 0.151]; -2.937 | not confirmed |
| H-L1 slower hurts S1t/S2/S3 more than S0 | S1t +0.168 [-0.172, 0.557] | not confirmed |
| H-C1 k12 < 0.5 k01 | -0.141 [-0.153, -0.128] | confirmed |

## Surprises
1. **The S3 edge did not survive.** Dev S3 - S3r was +0.181 bp; holdout +0.002. Against the post-hoc random-direction control S3m it was +0.097 on dev (CI touching 0) and -0.044 on the holdout. Fading top-of-book imbalance adds no measurable value per fill at this horizon.
2. **The holdout was quieter:** S0 traded 1,361 fills/h vs 3,787 on dev, and f* was -0.60 vs -0.80 bp. Every confirmed direction held anyway.
3. **No realistic arm breaks even at a -0.5 bp rebate** (-0.27 to -0.44 bp per fill). The naive-fill strawman shows +0.38 bp: the profit sign is set by the fill model.

## Known weaknesses (all written down before the holdout or now, none fixed in v1)
- H-L1 is in USDT/hour, so an arm that barely trades looks latency-robust (S1 with 0 fills scores +12.3).
- S2 has too few fills (178/h) for H-Q4a to have power.
- Data is 00:00-02:00 UTC only (Asian session): 8 dev hours, 6 holdout hours.
- S1 (Rule A) never quotes in a one-tick market; S1t is the touch-anchored stand-in.

## v2 pre-registration (to be tested only on days never used in v1)
- Full 24 h days, Nov 2026 - Jan 2027.
- H-L1 measured per fill (bp) instead of per hour.
- S4 reversal-gated contrarian (deferred from v1).
- Imbalance signal tested at longer horizons (5 s, 30 s) before any arm uses it.
- S2 run with enough hours for a CI narrower than 0.2 bp.

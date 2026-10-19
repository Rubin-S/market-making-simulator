# LEARNING.md: what this project teaches, in plain words

Read this before an interview. Every section ends with the one sentence to say out loud.

## 1. What a market maker earns, and what it loses

A maker posts a bid and an ask and hopes to buy low and sell high. Each fill earns **spread capture**:
half the spread, measured from the mid just before the fill. On BTCUSDT the spread is one tick (0.1 USDT),
so half a tick is well under 0.01 bp of the price. Spread capture is almost nothing.

Then the price moves. The **markout** at horizon τ is how far the mid moved for or against you τ after the
fill. Our P&L splits exactly, on every run, into

  **P&L = spread capture + markout(τ) + inventory − fees**

where *inventory* is what the position you kept did after τ. Every one of 462 runs (dev + holdout)
splits to 1e-9. That identity is how you know the books aren't lying.

> *Say:* "A maker earns half the spread and pays it back in markout. The question is which is bigger."

## 2. Adverse selection: you get filled when you're wrong

Your bid fills when someone sells into it. Who sells hard at the touch? Often someone who knows the price
is about to drop (Glosten & Milgrom's informed trader). So fills are a biased sample: on the holdout,
**P(bid fill | next 5 s down) − P(bid fill | next 5 s up) = 0.77**. The mean 1 s markout is −0.594 bp,
much bigger than the spread capture, and it keeps getting worse out to 30 s.

Order-book **imbalance** is the visible part of this. When your own side is heavy (big bid queue, thin ask),
you're at the back of a long line, the price tends to go *up* away from you, and you rarely fill. When
your side is thin, it fills, then the price falls through it. Logit slope of fill vs imbalance: −0.412.

> *Say:* "Fill probability and post-fill return move in opposite directions. That's the market maker's dilemma."

## 3. Queue position, and why the fill model decides everything

Binance doesn't tell you where your order sits in the queue. A simulator has to guess. When size ahead of
you is cancelled, did it come from in front of you or behind you?

- **RiskAdverse:** cancels never move you forward; only trades do. Most conservative.
- **Power(n) / Prorata:** a cancel comes from ahead with probability f(ahead) / (f(ahead) + f(behind)),
  f(x) = xⁿ. Prorata is n = 1, Power2 is n = 2 (more optimistic). This is hftbacktest's ProbQueueModel.
- **Naive:** you fill whenever a trade prints at your price. You skip the queue entirely.

The three queue models agree within about 0.1 bp. The naive one gives about 4.5× as many fills, and they're
the *easy* fills you'd never really get. It turns a losing strategy into one that looks profitable at the
rebate (+0.38 bp per fill).

> *Say:* "If your backtest skips the queue, it's measuring the fill model, not the strategy."

## 4. Avellaneda–Stoikov and GLFT

**A–S (2008):** a maker with risk aversion γ, volatility σ and a fill rate that decays as λ(δ) = A·e^(−kδ)
with distance δ from the mid quotes around a *reservation price* r = mid − q·γσ²(T − t): long inventory
pushes both quotes down, so you sell more easily.

**GLFT (Guéant, Lehalle & Fernandez-Tapia 2013)** is the infinite-horizon version with a closed form, in ticks:
- c1 = ln(1 + γ/k) / γ, c2 = √( γ/(2Ak) · (1 + γ/k)^(k/γ + 1) )
- half-spread h = c1 + ½σc2, skew per lot s = σc2
- bid sits h + s·q below the mid, ask h − s·q above it.

**Why the literal rule failed here:** it assumes exponential decay. Our probes showed λ falls 22% from 0
to 1 tick back but only 3% from 1 to 2 (k01 = 0.245, k12 = 0.033; H-C1 confirmed on the holdout). With
that k, GLFT's spread is many ticks wide, so S1 never quoted. Rule T (Addendum A1) anchors at the touch
and keeps only GLFT's skew. At the risk-chosen γ = 1e-6 that skew is 0.15 ticks per lot, so in practice the
hard |q| wall does the inventory control.

> *Say:* "GLFT's closed form needs exponential fill decay. Binance's isn't exponential, so I kept the skew and anchored at the touch."

## 5. The break-even fee f*

f* = (spread capture + 1 s markout) / notional, in bp: what the quotes earn per fill **before fees**.
Since net = f* − fee, a strategy profits only if its maker fee is below f*. One number answers every fee
tier. Holdout S0: f* = −0.60 bp, so even Binance's best −0.5 bp rebate isn't enough. Fees were kept out of
the quoting rule, so f* belongs to the strategy, not the account.

> *Say:* "f* is the most you could pay per fill and still break even, so you read any fee tier straight off it."

## 6. Microprice and the imbalance signal (Stoikov 2018)

The microprice adjusts the mid toward the thin side: if the ask queue is small, the next move is more
likely up. Stoikov estimates the adjustment G*(I) from a Markov chain on imbalance. On BTCUSDT the mid often
jumps several ticks at once, the sum didn't settle, and we used the 1 s conditional drift g_τ instead
(Addendum A2, fixed before running).

S2 leans with the signal, S3 fades it. Neither helped on the holdout. Knowing where the price is going
doesn't help a maker much, because the fills you get are exactly the ones where you're wrong.

> *Say:* "A signal that predicts the mid isn't the same as a signal that improves your fills."

## 7. Controls: change one thing

S3r was meant to be "S3 with a random direction". But it re-flipped its coin on every imbalance bin change,
so it cancelled about 2.9× as often as S3 and kept losing its queue place. S3 − S3r = +0.18 bp on dev mixed
"good direction" with "S3r is a clumsy order manager". S3m flips only when S3 itself would swap sides: same
churn, random direction. Against S3m the edge was +0.10 with a CI touching zero, and on the holdout it was gone.

A bootstrap CI only resamples the days you have. It can't see a regime change, or that you picked the
comparison after looking. That's what the holdout is for.

> *Say:* "A good control changes one thing only, and a CI is only as wide as the data it resamples."

## 8. Latency

Slower quotes stay stale for longer, which should hurt. But slower also means fewer fills. Per fill, dev P&L was −1.13 bp at
0 ms and −0.94 at 50 ms: slower looked *better*, because the fills you lose are the worst ones. H-L1 measured
the latency cut in USDT per hour, which rewards trading less (S1 with zero fills would "pass"). That flaw was
written down before the holdout, and v2 measures it per fill.

> *Say:* "Pick a metric that can't be won by doing nothing."

## 9. Pre-registration, freeze, holdout

1. **prereg-v1** tagged before any strategy ran: questions, arms, hypotheses with their signs, stats.
2. Every change after that is a dated **addendum** (A1–A7) saying what we had seen when we wrote it.
3. **freeze-v1** fixed code, calibration and γ. The holdout scripts refuse to run unless HEAD descends from
   it and the tree is clean.
4. The holdout ran **once**. Results: 8/11 confirmed, with Holm adjustment across all 11. The failures stay
   in the README; any fix is v2 on new days.

> *Say:* "I wrote down what would count as success before I could see the answer, and I reported the misses."

## 10. Interview Q&A

**Q1. Is market making on Binance profitable?**
Not for a retail-latency touch quoter in our sample. Holdout f* is −0.60 bp, below even the −0.5 bp rebate.
Albers et al. found the same live: naive touch quoting loses about 1.44 bp per round trip before fees.

**Q2. How do you know your simulator is right?**
Compat mode matches hftbacktest's fills exactly in 16/16 configs, own mode reproduces 97.5–98.9% of them,
the P&L identity holds on every run, and it reproduces Albers' live facts without tuning (fill rate 0.559 vs live 54.6–55.6%).

**Q3. What's the biggest assumption?**
Queue position. Binance hides it, so I bracket it with RiskAdverse and Power2 and report both. The answer
doesn't change sign across them; it does with a naive model.

**Q4. Why is your S3 result a failure, and why is that fine?**
The pre-registered S3 − S3r test failed on the holdout (+0.002 bp). The dev edge came from a bad control, which
the churn-matched S3m exposed. Reporting it is the point of pre-registering.

**Q5. How did you choose γ?**
Only by risk: the smallest γ keeping 99th-percentile |q| ≤ 10 lots on dev. Never by P&L, so it can't be overfit.

**Q6. Why is trade-through the worst fill source?**
A trade that sweeps through your price level is a big, aggressive order. That's the most informed flow, and
the price keeps going.

**Q7. What would change with a smaller-tick asset?**
Spread capture would be bigger relative to markout, queues shorter, and quoting a tick back would matter, so
the full GLFT rule might quote. It's worth testing in v2.

**Q8. One more week?**
Run v2 (full days, Nov–Jan), measure latency per fill, build S4 (reversal-gated), and add an execution
simulator that reuses this fill model.

## Further reading
- Albers, Cucuringu, Howison & Shestopaloff (2025), *The Market Maker's Dilemma: Navigating the Fill Probability vs. Post-Fill Returns Trade-Off*, arXiv 2502.18625.
- Avellaneda & Stoikov (2008), *High-frequency trading in a limit order book*, Quantitative Finance 8(3).
- Guéant, Lehalle & Fernandez-Tapia (2013), *Dealing with the inventory risk*, arXiv 1105.3115.
- Guéant (2017), *Optimal market making*, Applied Mathematical Finance 24(2), arXiv 1605.01862.
- Stoikov (2018), *The micro-price: a high-frequency estimator of future prices*, Quantitative Finance 18(12).
- Moallemi & Yuan (2017), *A model for queue position valuation in a limit order book*, SSRN 2996221.
- Huang, Lehalle & Rosenbaum (2015), *Simulating and analyzing order book data: the queue-reactive model*, JASA 110(509).
- DeLise (2024), *The Negative Drift of a Limit Order Fill*, arXiv 2407.16527.
- Cartea, Jaimungal & Penalva (2015), *Algorithmic and High-Frequency Trading*, Cambridge UP, ch. 10.
- hftbacktest documentation (queue models and the market-making tutorials): https://hftbacktest.readthedocs.io

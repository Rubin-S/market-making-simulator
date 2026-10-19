// queue.hpp - where are we in the line at our price level?
//
// We never see our own order in the data (it is simulated), so we estimate:
//   front  = size AHEAD of us (lots, may be fractional)
//   behind = size BEHIND us
// Two things move us forward:
//   1. a trade (print) at our price: it eats the line from the front.
//   2. a cancel: we can't see WHO cancelled, so a rule splits it between
//      ahead and behind. The rules are a bracket, from slowest to fastest:
//        RiskAdverse  <=  Power3(2)  <=  Prorata = Power(1)  <=  Power2(2)
//   Naive is a deliberately WRONG strawman (Crypto_MM / Hummingbot style):
//   no queue at all, so any print at our price fills us. It shows how much
//   ignoring the queue inflates results.
//
// Formulas copy hftbacktest's ProbQueueModel exactly (queue.rs 165-216),
// including its quirk that `back` is read before trades are taken off,
// so our "compat" mode can match it fill for fill.
#pragma once
#include <algorithm>
#include <cmath>
#include "mm/types.hpp"

namespace mm {

enum class QueueRule { RiskAdverse, Power3_2, Prorata, Power2_2, Naive };

// Probability that a cancel came from BEHIND us (so it does NOT help us).
inline double p_behind(QueueRule r, double front, double back) {
    double p = 1.0;
    switch (r) {
        case QueueRule::RiskAdverse:
        case QueueRule::Naive:       p = 1.0; break;                         // never from ahead
        case QueueRule::Power3_2: { double x = front / (front + back); p = 1.0 - x * x; break; }
        case QueueRule::Prorata:    p = back / (back + front); break;         // share of the line
        case QueueRule::Power2_2: { double x = back / (back + front); p = x * x; break; }
    }
    return std::isfinite(p) ? p : 1.0;                                        // 0/0 -> assume behind
}

struct QueuePos {
    QueueRule rule = QueueRule::Prorata;
    double front = 0;       // lots ahead of us
    double behind = 0;      // lots behind us
    double cum_trade = 0;   // prints at our price since the last depth update (for dedupe)

    // Our order reaches the exchange: we join BEHIND everything visible.
    void join(Lot level_qty) {
        front = rule == QueueRule::Naive ? 0.0 : static_cast<double>(level_qty);   // Naive: no line
        behind = 0; cum_trade = 0;
    }

    // A print of `qty` lots at exactly our price. Returns lots WE got filled (0 if none).
    Lot on_trade(Lot qty) {
        front -= static_cast<double>(qty);
        if (rule != QueueRule::RiskAdverse) cum_trade += static_cast<double>(qty);
        const Lot exec = round_half_away(-front);     // overshoot of >= 1/2 lot fills us
        if (exec > 0) { front = 0; return exec; }
        return 0;
    }

    // The level size at our price changed from `prev` to `now` (exchange book).
    void on_depth(Lot prev, Lot now) {
        const double nq = static_cast<double>(now);
        if (rule == QueueRule::Naive) {
            front = 0;                                  // always first in line
        } else if (rule == QueueRule::RiskAdverse) {
            front = std::min(front, nq);                // only the clamp moves us
        } else {
            double chg = static_cast<double>(prev - now) - cum_trade;   // dedupe: prints already counted
            cum_trade = 0;
            if (chg < 0) {
                front = std::min(front, nq);            // net growth = people joining behind us
            } else {
                const double back = static_cast<double>(prev) - front;  // hftbacktest's convention
                const double p = p_behind(rule, front, back);
                const double est = front - (1.0 - p) * chg + std::min(back - p * chg, 0.0);
                front = std::min(est, nq);
            }
        }
        behind = std::max(0.0, nq - std::max(front, 0.0));
    }

    // Queue position: 0 = at the front, 1 = at the back.
    double qp() const {
        const double f = std::max(front, 0.0), t = f + behind;
        return t > 0 ? f / t : 0.0;
    }
};

}  // namespace mm

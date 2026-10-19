// ledger.hpp - every fill, our position, cash, fees, and the P&L split.
//
// For one fill with sign s (+1 buy, -1 sell), size q, price p,
// mid at the fill m, mid tau later m_tau, and the final mid M:
//
//   spread capture  s*q*(m     - p)      what we earned vs the mid right away
//   markout         s*q*(m_tau - m)      did the price then move for or against us
//   inventory       s*q*(M - m_tau)      holding the position until the end
//   ---------------------------------
//   sum           = s*q*(M - p)          exactly the fill's trading P&L
//
// Prices are kept in HALF ticks so every term is an exact integer.
#pragma once
#include <cstdint>
#include <vector>
#include "mm/types.hpp"

namespace mm {

struct Fill {
    Ts ts;
    Side side;
    Tick px;
    Lot qty;
    bool maker;    // true = our resting order was hit (maker fee)
    Half mid;      // mid just before the fill, in half ticks
    Half mid_tau;  // mid tau later, in half ticks
};

struct PnlParts {             // all in USDT
    double spread_capture = 0, markout = 0, inventory = 0, fees = 0;
    double total() const { return spread_capture + markout + inventory - fees; }
};

class Ledger {
    std::vector<Fill> fills_;
    Lot pos_ = 0;              // + long, - short (lots)
    std::int64_t cash_ = 0;    // tick*lots; buying costs cash
    double fees_ = 0;          // USDT
    double maker_fee_, taker_fee_;   // as fractions: 0.0002 = 2 bps
public:
    Ledger(double maker_fee, double taker_fee) : maker_fee_(maker_fee), taker_fee_(taker_fee) {}

    void add(const Fill& f) {
        const std::int64_t s = sign(f.side);
        pos_  += s * f.qty;
        cash_ -= s * f.px * f.qty;
        fees_ += (f.maker ? maker_fee_ : taker_fee_) * f.px * f.qty * kUsdtPerTickLot;
        fills_.push_back(f);
    }

    Lot position() const { return pos_; }
    double fees() const { return fees_; }
    const std::vector<Fill>& fills() const { return fills_; }

    // Mark-to-market equity at final mid M (half ticks), after fees, in USDT.
    // 2*cash + pos*M is in half-tick*lots, so divide by 2 at the end.
    double equity(Half M) const {
        return static_cast<double>(2 * cash_ + pos_ * M) / 2.0 * kUsdtPerTickLot - fees_;
    }

    // The same P&L, split into its three parts plus fees.
    PnlParts split(Half M) const {
        std::int64_t sc = 0, mo = 0, inv = 0;      // half-tick*lots, exact
        for (const Fill& f : fills_) {
            const std::int64_t sq = sign(f.side) * f.qty;
            sc  += sq * (f.mid - 2 * f.px);
            mo  += sq * (f.mid_tau - f.mid);
            inv += sq * (M - f.mid_tau);
        }
        auto usdt = [](std::int64_t h) { return static_cast<double>(h) / 2.0 * kUsdtPerTickLot; };
        return {usdt(sc), usdt(mo), usdt(inv), fees_};
    }
};

}  // namespace mm

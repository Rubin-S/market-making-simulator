// types.hpp - the basic units of the simulator. Integers everywhere.
#pragma once
#include <cmath>
#include <cstdint>
#include "mdr/event.hpp"   // reuse the engine's Side { Bid = 0, Ask = 1 }

namespace mm {

using Side = mdr::Side;
using Ts   = std::int64_t;  // time in microseconds (same as the .evt files)
using Tick = std::int64_t;  // price in ticks: 1 tick = 0.1 USDT
using Lot  = std::int64_t;  // size in lots:  1 lot  = 0.001 BTC
using Half = std::int64_t;  // price in HALF ticks, so a mid (bid+ask)/2 is always exact

inline constexpr double kTickUsdt = 0.1;
inline constexpr double kLotBtc   = 0.001;
inline constexpr double kUsdtPerTickLot = kTickUsdt * kLotBtc;   // 1e-4 USDT

// +1 for a buy (bid), -1 for a sell (ask): turns "which side" into a sign.
inline constexpr std::int64_t sign(Side s) { return s == Side::Bid ? +1 : -1; }

// Round to the nearest integer, halves away from zero: 2.5 -> 3, -2.5 -> -3.
// (std::llround does exactly this; named here so the rule is explicit.)
inline std::int64_t round_half_away(double x) { return std::llround(x); }

}  // namespace mm

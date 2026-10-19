// markout.hpp - the mid price over time, so we can ask "what was the mid
// tau after this fill?" without peeking into the future.
//
// Rule (as-of): the mid at time t is the LAST mid recorded at or before t.
// Never the next one after t; that would be look-ahead.
#pragma once
#include <algorithm>
#include <optional>
#include <vector>
#include "mm/types.hpp"

namespace mm {

class MidSeries {
    std::vector<Ts> ts_;      // strictly increasing
    std::vector<Half> mid_;   // mid in half ticks (bid + ask)
public:
    // Record the mid at time t (call it after every book change, in time order).
    void push(Ts t, Half m) {
        if (!ts_.empty() && ts_.back() == t) { mid_.back() = m; return; }   // same instant: keep the last
        if (!mid_.empty() && mid_.back() == m) return;                       // unchanged: skip, saves memory
        ts_.push_back(t); mid_.push_back(m);
    }
    // Mid as of time t, or nothing if no mid existed yet.
    std::optional<Half> asof(Ts t) const {
        auto it = std::upper_bound(ts_.begin(), ts_.end(), t);   // first time > t
        if (it == ts_.begin()) return std::nullopt;
        return mid_[static_cast<std::size_t>(it - ts_.begin()) - 1];
    }
    std::size_t size() const { return ts_.size(); }
};

}  // namespace mm

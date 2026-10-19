// order.hpp - one order slot per side, and the rules for moving between states.
//
//   Idle --submit--> PendingNew --arrive--> Resting --cancel--> PendingCancel
//     ^                                       |                     |
//     +------------- filled / cancel_done ----+---------------------+
//
// After a fill or cancel, the strategy only learns about it one response
// latency later (t_free). Until then the slot stays busy, so we can never
// stack a second order on the same side.
#pragma once
#include <cstdint>
#include "mm/types.hpp"

namespace mm {

enum class State : std::uint8_t { Idle, PendingNew, Resting, PendingCancel };

struct OrderSlot {
    State st = State::Idle;
    std::uint64_t id = 0;     // order id, unique across the run
    Tick px = 0;
    Lot qty = 0;
    Ts t_arrive = 0;          // when the new order reaches the exchange
    Ts t_cancel_arrive = 0;   // when the cancel reaches the exchange
    Ts t_free = 0;            // when the strategy hears the order is gone

    // Busy = there is an order, or we are still waiting to hear it ended.
    bool busy(Ts now) const { return st != State::Idle || now < t_free; }

    // Send a new order. Refused (false) if the slot is busy.
    bool submit(std::uint64_t new_id, Tick p, Lot q, Ts now, Ts entry_lat) {
        if (busy(now)) return false;
        st = State::PendingNew; id = new_id; px = p; qty = q;
        t_arrive = now + entry_lat;
        return true;
    }
    // The exchange receives the order; it starts resting in the book.
    void arrive() { if (st == State::PendingNew) st = State::Resting; }

    // Ask to cancel. Only a resting order can be cancelled.
    bool cancel(Ts now, Ts entry_lat) {
        if (st != State::Resting) return false;
        st = State::PendingCancel;
        t_cancel_arrive = now + entry_lat;
        return true;
    }
    // The cancel reaches the exchange (and the order was not filled first).
    void cancel_done(Ts ts, Ts resp_lat) {
        if (st == State::PendingCancel) { st = State::Idle; t_free = ts + resp_lat; }
    }
    // The order filled (whole order; we trade 1 lot). Possible while a cancel is in flight.
    void filled(Ts ts, Ts resp_lat) {
        if (st == State::Resting || st == State::PendingCancel) { st = State::Idle; t_free = ts + resp_lat; }
    }
};

}  // namespace mm

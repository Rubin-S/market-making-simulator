// fill.hpp - the exchange side of ONE of our orders (one per side).
//
// Our order is a ghost: it never enters the replayed book. This class decides
// WHEN it reaches the exchange and WHEN it fills. Three fill sources:
//   Queue   - a print at our price eats everyone ahead of us and reaches us
//   Through - a print at a price WORSE than ours for the taker (a sell below
//             our bid): it must have gone through us, so we fill in full
//   Cross   - the opposite best price reaches ours (best ask <= our bid),
//             no print needed
// A level that simply vanishes with no print and no cross gives NO fill.
//
// Timing (same as hftbacktest): ties go to the market. The driver calls
// process_msg() for every order message that arrives STRICTLY before the next
// exchange event, so an order arriving at t sees the book after all events at t.
// Post-only (GTX): an order that would cross on arrival is rejected, never a taker.
#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include "mdr/books.hpp"   // mdr::kNoPx = "this side of the book is empty"
#include "mm/order.hpp"
#include "mm/queue.hpp"

namespace mm {

enum class FillSrc : std::uint8_t { Queue, Through, Cross };

struct FillEvent {
    Ts ts;          // exchange time of the fill
    Side side;      // our side (Bid = we bought)
    Tick px;        // always our own price (maker)
    Lot qty;
    FillSrc src;
};

inline constexpr Ts kNever = std::numeric_limits<Ts>::max();

class MakerSide {
    Side side_;
    Ts entry_lat_, resp_lat_;
    OrderSlot slot_;
    QueuePos q_;
    std::uint64_t gtx_rejects_ = 0;

    bool live() const { return slot_.st == State::Resting || slot_.st == State::PendingCancel; }

    FillEvent fill(Ts ts, FillSrc src) {
        slot_.filled(ts, resp_lat_);              // whole order (no partial fills)
        return {ts, side_, slot_.px, slot_.qty, src};
    }

public:
    MakerSide(Side side, QueueRule rule, Ts entry_lat, Ts resp_lat)
        : side_(side), entry_lat_(entry_lat), resp_lat_(resp_lat) { q_.rule = rule; }

    // ---- strategy side (local clock) ----
    bool quote(std::uint64_t id, Tick px, Lot qty, Ts now) { return slot_.submit(id, px, qty, now, entry_lat_); }
    bool cancel(Ts now) { return slot_.cancel(now, entry_lat_); }
    bool busy(Ts now) const { return slot_.busy(now); }
    Side side() const { return side_; }
    const OrderSlot& slot() const { return slot_; }
    const QueuePos& queue() const { return q_; }
    std::uint64_t gtx_rejects() const { return gtx_rejects_; }

    // ---- exchange side ----
    // When does our next message (new order or cancel) reach the exchange?
    Ts next_msg_time() const {
        if (slot_.st == State::PendingNew) return slot_.t_arrive;
        if (slot_.st == State::PendingCancel) return slot_.t_cancel_arrive;
        return kNever;
    }

    // Deliver that message. level_qty = size at our price on the exchange book now.
    void process_msg(Lot level_qty, Tick best_bid, Tick best_ask) {
        if (slot_.st == State::PendingNew) {
            const bool crosses = side_ == Side::Bid ? (best_ask != mdr::kNoPx && slot_.px >= best_ask)
                                                    : (best_bid != mdr::kNoPx && slot_.px <= best_bid);
            if (crosses) { slot_.rejected(slot_.t_arrive, resp_lat_); ++gtx_rejects_; return; }
            slot_.arrive();
            q_.join(level_qty);                       // behind everything visible
        } else if (slot_.st == State::PendingCancel) {
            slot_.cancel_done(slot_.t_cancel_arrive, resp_lat_);
        }
    }

    // A print. taker = the aggressor's side (Ask = a sell hitting bids).
    std::optional<FillEvent> on_trade(Ts ts, Side taker, Tick px, Lot qty) {
        if (!live() || taker == side_) return std::nullopt;   // only prints against our side
        const std::int64_t s = sign(side_);                   // +1 bid, -1 ask
        if (px == slot_.px) {
            if (q_.on_trade(qty) > 0) return fill(ts, FillSrc::Queue);
        } else if (s * (slot_.px - px) > 0) {                 // bid: print below us; ask: above us
            return fill(ts, FillSrc::Through);
        }
        return std::nullopt;
    }

    // A depth update at (side, px): size went prev -> now. Best prices are AFTER it.
    std::optional<FillEvent> on_depth(Ts ts, Side side, Tick px, Lot prev, Lot now,
                                      Tick best_bid, Tick best_ask) {
        if (!live()) return std::nullopt;
        if (side == side_ && px == slot_.px) q_.on_depth(prev, now);
        const bool crossed = side_ == Side::Bid ? (best_ask != mdr::kNoPx && best_ask <= slot_.px)
                                                : (best_bid != mdr::kNoPx && best_bid >= slot_.px);
        if (crossed) return fill(ts, FillSrc::Cross);
        return std::nullopt;
    }
};

}  // namespace mm

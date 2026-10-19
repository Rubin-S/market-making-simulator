// test_fill.cpp - Step 2c tests: fill sources, post-only, and latency races.
// A tiny fake exchange book drives MakerSide the same way the real driver will.
#include <cstdio>
#include <map>
#include <optional>
#include "mm/fill.hpp"

using namespace mm;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); ++failures; } } while (0)

// Minimal L2 exchange book + the "ties go to the market" event rule.
struct FakeExchange {
    std::map<Tick, Lot> bids, asks;
    Tick best_bid() const { return bids.empty() ? mdr::kNoPx : bids.rbegin()->first; }
    Tick best_ask() const { return asks.empty() ? mdr::kNoPx : asks.begin()->first; }
    Lot qty(Side s, Tick px) const {
        const auto& m = s == Side::Bid ? bids : asks;
        auto it = m.find(px);
        return it == m.end() ? 0 : it->second;
    }
    // Deliver our order messages that arrive STRICTLY before an event at `ts`.
    void flush(MakerSide& o, Ts ts) const {
        while (o.next_msg_time() < ts)
            o.process_msg(qty(o.side(), o.slot().px), best_bid(), best_ask());
    }
    std::optional<FillEvent> depth(MakerSide& o, Ts ts, Side s, Tick px, Lot q) {
        flush(o, ts);
        auto& m = s == Side::Bid ? bids : asks;
        const Lot prev = qty(s, px);
        if (q == 0) m.erase(px); else m[px] = q;
        return o.on_depth(ts, s, px, prev, q, best_bid(), best_ask());
    }
    std::optional<FillEvent> trade(MakerSide& o, Ts ts, Side taker, Tick px, Lot q) {
        flush(o, ts);
        return o.on_trade(ts, taker, px, q);
    }
};

// Book: bid 1000 x 10, ask 1001 x 10 (ticks of 0.1 USDT, so 100.0 / 100.1).
static FakeExchange book() { FakeExchange x; x.bids[1000] = 10; x.asks[1001] = 10; return x; }

// fill_through: bid 100.0, a sell print at 99.9 -> whole order fills at OUR price.
static void fill_through() {
    FakeExchange x = book();
    MakerSide o(Side::Bid, QueueRule::Prorata, 0, 0);
    o.quote(1, 1000, 1, 0); x.flush(o, 1);
    CHECK(o.slot().st == State::Resting && o.queue().front == 10);   // joined behind 10
    auto f = x.trade(o, 10, Side::Ask, 999, 1);
    CHECK(f && f->src == FillSrc::Through && f->px == 1000 && f->qty == 1);
    CHECK(o.slot().st == State::Idle);
}

// fill_cross + vanish: our level disappears (no fill), then the best ask
// drops to 100.0 with no print -> fill at 100.0, source Cross.
static void fill_cross() {
    FakeExchange x = book();
    MakerSide o(Side::Bid, QueueRule::Prorata, 0, 0);
    o.quote(1, 1000, 1, 0); x.flush(o, 1);
    CHECK(!x.depth(o, 10, Side::Bid, 1000, 0));      // level vanished: NOT a fill
    CHECK(o.slot().st == State::Resting);
    auto f = x.depth(o, 20, Side::Ask, 1000, 3);     // best ask now 1000 <= our bid
    CHECK(f && f->src == FillSrc::Cross && f->px == 1000);
}

// fill_queue: 10 ahead; a sell print of 10 at our price -> not yet; 1 more -> Queue fill.
static void fill_queue() {
    FakeExchange x = book();
    MakerSide o(Side::Bid, QueueRule::RiskAdverse, 0, 0);
    o.quote(1, 1000, 1, 0); x.flush(o, 1);
    CHECK(!x.trade(o, 10, Side::Ask, 1000, 10));
    CHECK(!x.trade(o, 11, Side::Bid, 1001, 50));     // a BUY print never fills our bid
    auto f = x.trade(o, 12, Side::Ask, 1000, 1);
    CHECK(f && f->src == FillSrc::Queue);
}

// gtx_reject: a bid at 100.1 when the best ask is 100.1 -> rejected, never a taker.
static void gtx_reject() {
    FakeExchange x = book();
    MakerSide o(Side::Bid, QueueRule::Prorata, 50, 50);
    CHECK(o.quote(1, 1001, 1, 0));
    x.flush(o, 51);                                  // arrives at 50
    CHECK(o.slot().st == State::Idle && o.gtx_rejects() == 1);
    CHECK(o.busy(99) && !o.busy(100));               // we hear the reject at 100
}

// lat_tie: our bid arrives at t=100 and a big sell print is ALSO at t=100.
// Ties go to the market: the print happens first, our order is not there yet.
static void lat_tie() {
    FakeExchange x = book();
    MakerSide o(Side::Bid, QueueRule::Prorata, 100, 0);
    o.quote(1, 1000, 1, 0);
    CHECK(!x.trade(o, 100, Side::Ask, 1000, 50));    // not seen
    CHECK(o.slot().st == State::PendingNew);
    x.flush(o, 101);
    CHECK(o.slot().st == State::Resting);
}

// lat_cancel_race: cancel sent at 70 with 50 entry latency lands at 120.
// A print that reaches us at 119 or 120 fills; at 121 the order is already gone.
static void lat_cancel_race() {
    const Ts print_ts[] = {119, 120, 121};
    const bool expect_fill[] = {true, true, false};
    for (int i = 0; i < 3; ++i) {
        FakeExchange x = book();
        MakerSide o(Side::Bid, QueueRule::Prorata, 50, 50);
        o.quote(1, 1000, 1, 0); x.flush(o, 51);      // resting at 50, 10 ahead
        CHECK(o.cancel(70));
        auto f = x.trade(o, print_ts[i], Side::Ask, 1000, 11);
        CHECK(f.has_value() == expect_fill[i]);
    }
}

int main() {
    fill_through(); fill_cross(); fill_queue(); gtx_reject(); lat_tie(); lat_cancel_race();
    if (failures) { std::fprintf(stderr, "%d check(s) FAILED\n", failures); return 1; }
    std::printf("all fill tests passed\n");
}

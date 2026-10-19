// test_core.cpp - Step 2a tests: order state machine + ledger.
// Dependency-free. CHECK stays active in Release (no assert()).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include "mm/ledger.hpp"
#include "mm/order.hpp"

using namespace mm;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); ++failures; } } while (0)

// The basic life of one order, step by step.
static void order_lifecycle() {
    OrderSlot o;
    CHECK(!o.busy(0));
    CHECK(o.submit(1, 1000, 1, /*now*/ 0, /*entry*/ 50));
    CHECK(o.st == State::PendingNew && o.t_arrive == 50);
    CHECK(!o.submit(2, 1000, 1, 10, 50));            // busy: no second order
    CHECK(!o.cancel(10, 50));                        // can't cancel before it arrives
    o.arrive();
    CHECK(o.st == State::Resting);
    CHECK(o.cancel(60, 50) && o.st == State::PendingCancel && o.t_cancel_arrive == 110);
    o.cancel_done(110, 50);
    CHECK(o.st == State::Idle && o.busy(150) && !o.busy(160));   // free only after the response
    CHECK(o.submit(2, 1001, 1, 160, 50));

    // A fill that races the cancel wins: the order is gone, the cancel does nothing.
    o.arrive(); o.cancel(200, 50); o.filled(220, 50);
    CHECK(o.st == State::Idle);
    o.cancel_done(250, 50);
    CHECK(o.t_free == 270);                          // set by the fill, not the late cancel
}

// lat_no_dupes: 100 ms round trip, an event every 10 ms, a strategy that
// tries to quote on every event. There must never be 2 orders on one side.
static void lat_no_dupes() {
    const Ts entry = 50'000, resp = 50'000;          // 50 ms + 50 ms = 100 ms round trip
    OrderSlot bid;
    std::uint64_t next_id = 1;
    int accepted = 0, live_max = 0;
    for (Ts now = 0; now <= 10'000'000; now += 10'000) {   // 10 s of events
        if (bid.st == State::PendingNew && bid.t_arrive <= now) bid.arrive();
        if (bid.st == State::PendingCancel && bid.t_cancel_arrive <= now)
            bid.cancel_done(bid.t_cancel_arrive, resp);
        // greedy strategy: cancel anything resting, otherwise try to post a new bid
        if (bid.st == State::Resting) bid.cancel(now, entry);
        else if (bid.submit(next_id, 1000, 1, now, entry)) { ++next_id; ++accepted; }
        int live = bid.st != State::Idle ? 1 : 0;
        live_max = live > live_max ? live : live_max;
    }
    CHECK(live_max <= 1);
    CHECK(accepted <= 10'000'000 / 100'000);         // never faster than one per round trip
    CHECK(accepted > 0);
    std::printf("lat_no_dupes: %d orders in 10 s, max live per side %d\n", accepted, live_max);
}

// pnl_identity: random fills; spread capture + markout + inventory - fees must
// equal equity. The three parts are exact integers, so this holds to 1e-9.
static void pnl_identity() {
    std::mt19937_64 rng(20261019);
    std::uniform_int_distribution<int> side(0, 1), qty(1, 5), move(-20, 20), coin(0, 1);
    for (int trial = 0; trial < 100; ++trial) {
        Ledger L(0.0002, 0.0005);
        Half mid = 2 * 1'100'000 + 1;                // ~110,000 USDT, a half-tick mid
        for (int i = 0; i < 1000; ++i) {
            mid += move(rng);
            Side s = side(rng) ? Side::Bid : Side::Ask;
            Tick px = (mid / 2) + (s == Side::Bid ? -move(rng) % 3 : move(rng) % 3);
            Half mid_tau = mid + move(rng);
            L.add({i * 1000, s, px, qty(rng), coin(rng) == 1, mid, mid_tau});
        }
        Half M = mid + move(rng);
        PnlParts p = L.split(M);
        CHECK(std::fabs(p.total() - L.equity(M)) < 1e-9);
    }
    // a hand case: buy 1 lot at 1000 ticks, mid 1000.5 then 1001, end mid 1002
    Ledger H(0.0002, 0.0005);
    H.add({0, Side::Bid, 1000, 1, true, 2001, 2002});
    PnlParts p = H.split(2004);
    CHECK(std::fabs(p.spread_capture - 0.5e-4) < 1e-12);   // half a tick-lot
    CHECK(std::fabs(p.markout - 0.5e-4) < 1e-12);
    CHECK(std::fabs(p.inventory - 1.0e-4) < 1e-12);
    CHECK(H.position() == 1);
    CHECK(std::fabs(H.fees() - 2e-5) < 1e-15);              // 2 bps of 1000 ticks * 1 lot
}

int main() {
    order_lifecycle();
    lat_no_dupes();
    pnl_identity();
    CHECK(round_half_away(2.5) == 3 && round_half_away(-2.5) == -3 && round_half_away(2.4) == 2);
    if (failures) { std::fprintf(stderr, "%d check(s) FAILED\n", failures); return 1; }
    std::printf("all core tests passed\n");
}

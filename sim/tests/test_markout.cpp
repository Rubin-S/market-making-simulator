// test_markout.cpp - Step 2c-2 test: markout_asof (no look-ahead).
#include <cmath>
#include <cstdio>
#include "mm/ledger.hpp"
#include "mm/markout.hpp"

using namespace mm;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); ++failures; } } while (0)

// markout_asof: hand-made mid path (half ticks).
//   t=0 -> 2001, t=10 -> 2003, t=20 -> 2003 (unchanged), t=30 -> 1999
static void markout_asof() {
    MidSeries m;
    m.push(0, 2001); m.push(10, 2003); m.push(20, 2003); m.push(30, 1999);
    CHECK(m.size() == 3);                       // the unchanged 2003 was skipped
    CHECK(!m.asof(-1));                         // before any mid: nothing
    CHECK(*m.asof(0) == 2001);
    CHECK(*m.asof(9) == 2001);                  // NOT 2003: that's still in the future
    CHECK(*m.asof(10) == 2003);                 // at exactly t: included
    CHECK(*m.asof(29) == 2003);
    CHECK(*m.asof(1000) == 1999);

    m.push(30, 1997);                           // a 2nd update at the same instant wins
    CHECK(*m.asof(30) == 1997 && m.size() == 3);

    // Buy 1 lot at 1000 ticks at t=5; tau = 25 -> mid at t=30 = 1997 half ticks.
    // markout = +1 * 1 * (1997 - 2001) = -4 half ticks = -2 ticks = -2e-4 USDT.
    Ledger L(0.0002, 0.0005);
    L.add({5, Side::Bid, 1000, 1, true, *m.asof(5), *m.asof(5 + 25)});
    CHECK(std::fabs(L.split(1997).markout - (-2e-4)) < 1e-12);
    std::printf("markout_asof: markout = %.1e USDT (expect -2.0e-04)\n", L.split(1997).markout);
}

int main() {
    markout_asof();
    if (failures) { std::fprintf(stderr, "%d check(s) FAILED\n", failures); return 1; }
    std::printf("all markout tests passed\n");
}

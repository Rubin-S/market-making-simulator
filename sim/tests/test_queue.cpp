// test_queue.cpp - Step 2b tests: queue position under each cancel rule.
#include <cmath>
#include <cstdio>
#include "mm/queue.hpp"

using namespace mm;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); ++failures; } } while (0)
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) < tol; }

static const QueueRule kAll[] = {QueueRule::RiskAdverse, QueueRule::Power3_2,
                                 QueueRule::Prorata, QueueRule::Power2_2};

// q_fifo_basic: 5 ahead, a print of 3 at our price -> 2 ahead, no fill.
static void q_fifo_basic() {
    for (QueueRule r : kAll) {
        QueuePos q{r}; q.join(5);
        CHECK(q.on_trade(3) == 0);
        CHECK(near(q.front, 2));
        CHECK(q.on_trade(2) == 0);          // exactly reaches us: still not filled
        CHECK(q.on_trade(1) == 1);          // now the print reaches our 1 lot
    }
}

// q_dead_order: the old order's queue must not haunt the new one.
// (lob-execution-lab bug: queue state keyed by price kept a cancelled order.)
static void q_dead_order() {
    QueuePos old{QueueRule::Prorata}; old.join(5);
    // old order cancelled: we simply throw `old` away. A new order joins later.
    QueuePos now{QueueRule::Prorata}; now.join(7);
    CHECK(now.on_trade(3) == 0 && near(now.front, 4));   // the live order advances
}

// q_dedupe_trade_first: 5 ahead (level 10), print 3, then depth 10 -> 7.
// The depth drop IS the print, so it must not count twice. Front 2 under every rule.
static void q_dedupe_trade_first() {
    for (QueueRule r : kAll) {
        QueuePos q{r}; q.join(10); q.front = 5;          // 5 ahead, 5 behind
        q.on_trade(3);
        q.on_depth(10, 7);
        CHECK(near(q.front, 2));
    }
}

// q_rules_bracket: 1 ahead, 9 behind, level 10 -> 5 with no print.
// RiskAdverse 1.0 >= Power3(2) 0.95 >= Prorata 0.5 >= Power2(2) 0.05 lots ahead.
static void q_rules_bracket() {
    const double expect[] = {1.0, 0.95, 0.5, 0.05};
    for (int i = 0; i < 4; ++i) {
        QueuePos q{kAll[i]}; q.join(10); q.front = 1;
        q.on_depth(10, 5);
        CHECK(near(q.front, expect[i]));
    }
}

// q_adds_behind: level 2 -> 5. New size joins behind us; front unchanged.
static void q_adds_behind() {
    for (QueueRule r : kAll) {
        QueuePos q{r}; q.join(2);
        q.on_depth(2, 5);
        CHECK(near(q.front, 2) && near(q.behind, 3));
        CHECK(near(q.qp(), 0.4));
    }
}

// q_back_quirk: hftbacktest reads `back` before prints are taken off.
// Level 15 = 10 ahead + us + 5 behind; print 3; depth 15 -> 10; Prorata -> 6.0667
// (6.067 in the engine; the "true" back would give 5.833).
static void q_back_quirk() {
    QueuePos q{QueueRule::Prorata}; q.join(15); q.front = 10;
    q.on_trade(3);
    q.on_depth(15, 10);
    CHECK(near(q.front, 7.0 - (7.0 / 15.0) * 2.0));
    std::printf("q_back_quirk: front = %.4f (hftbacktest 6.067)\n", q.front);
}

int main() {
    q_fifo_basic(); q_dead_order(); q_dedupe_trade_first();
    q_rules_bracket(); q_adds_behind(); q_back_quirk();
    if (failures) { std::fprintf(stderr, "%d check(s) FAILED\n", failures); return 1; }
    std::printf("all queue tests passed\n");
}

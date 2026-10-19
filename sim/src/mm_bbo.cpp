// mm_bbo.cpp - dump the top of book once per finished exchange timestamp,
// on the same clock as mm_sim (Tier A: exchange timestamp, non-decreasing).
// One row each time the best bid/ask price or size changes. Prices in ticks,
// sizes in lots. Read by research/calibrate.py (sigma, microprice G*).
// usage: mm_bbo <file.evt> <out.csv>
#include <algorithm>
#include <cstdio>
#include <exception>
#include "mdr/books.hpp"
#include "mdr/replay.hpp"

using mdr::ArrayBook; using mdr::Event; using mdr::Level;

struct Bbo {
    std::FILE* fp;
    long long t = 0, rows = 0;
    bool pending = false;
    Level lb{}, la{};                                   // last row written

    void emit(const ArrayBook& b) {
        const Level bb = b.best_bid(), ba = b.best_ask();
        if (bb.px == mdr::kNoPx || ba.px == mdr::kNoPx || bb.px >= ba.px) return;   // one-sided or crossed
        if (bb.px == lb.px && bb.qty == lb.qty && ba.px == la.px && ba.qty == la.qty) return;  // no change
        std::fprintf(fp, "%lld,%lld,%lld,%lld,%lld\n", t, (long long)bb.px, (long long)ba.px,
                     (long long)bb.qty, (long long)ba.qty);
        lb = bb; la = ba; ++rows;
    }
    void before_event(const ArrayBook& b, const Event& e) {
        if (pending && e.ts_exch > t) { pending = false; emit(b); }   // time t is over: its final book
        t = std::max<long long>(t, e.ts_exch);
    }
    void on_book(const ArrayBook&, const Event& e) { if (!(e.flags & mdr::kSnapshot)) pending = true; }
    void on_trade(const ArrayBook&, const Event&) {}
};

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: mm_bbo <file.evt> <out.csv>\n"); return 2; }
    try {
        mdr::EventFile f(argv[1]);
        std::FILE* fp = std::fopen(argv[2], "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        std::fprintf(fp, "ts_exch_us,bid_tick,ask_tick,bid_lot,ask_lot\n");
        Bbo s{fp};
        ArrayBook book;
        mdr::replay(f.events(), book, s);
        if (s.pending) s.emit(book);                     // the last timestamp
        std::fclose(fp);
        std::printf("%lld rows -> %s\n", s.rows, argv[2]);
        return 0;
    } catch (const std::exception& ex) { std::fprintf(stderr, "error: %s\n", ex.what()); return 1; }
}

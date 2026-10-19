// mm_sim.cpp - replay one .evt day through the exchange book and simulate
// our maker orders on top of it. Strategy here: S0 naive touch.
//
// Tier A clock: ONE clock = the exchange timestamp (made non-decreasing),
// zero feed latency. Order entry/response latency is real (half the round
// trip each way). Feed latency arrives in Tier B.
//
// Per event, in this order:
//   before_event: deliver our order messages that arrive STRICTLY before
//                 this event (ties go to the market); remember the old size
//                 at our price; remember the mid "just before".
//   apply event:  the engine's replay() updates the book.
//   on_book / on_trade: queue update + fill checks for both sides,
//                 record the mid, and mark "the strategy wants to act".
// The strategy acts ONCE per timestamp, on the finished book: when the next
// event has a later timestamp (one exchange message can carry many levels,
// and we must not quote off a half-applied message). Same as hftbacktest.
//
// S0 naive touch: keep 1 lot on the best bid and 1 lot on the best ask.
// Probe mode (depth > 0): the same, but `depth` ticks behind best (bid at
// best_bid - depth, ask at best_ask + depth). Used to measure the fill
// intensity lambda(depth) = fills / seconds resting, for calibrating A and k.
// Cancel only when the touch moves (hysteresis keeps queue position).
// Hard cap: stop quoting the side that would push |position| past qmax.
// The strategy only acts on market events, never on a response wake.
//
// Outputs in <out_dir>: fills.csv, events.csv, summary.json.
// usage: mm_sim <file.evt> <out_dir> [rule=prorata] [latency_ms=0] [qmax=10] [depth=0]
//        rule: riskadverse | power3 | prorata | power2 | naive
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "mdr/books.hpp"
#include "mdr/replay.hpp"
#include "mm/fill.hpp"
#include "mm/ledger.hpp"
#include "mm/markout.hpp"

using namespace mm;
using mdr::ArrayBook; using mdr::Event; using mdr::Kind; using mdr::Level;

static constexpr Ts kBurnIn = 60'000'000;                       // 60 s, also covers the funding mask
static constexpr Ts kHorizon[4] = {100'000, 1'000'000, 5'000'000, 30'000'000};   // markout taus
static constexpr int kTauLedger = 1;                            // P&L split uses tau = 1 s
static constexpr Ts kFundingEvery = 8LL * 3600 * 1'000'000;     // Binance funding: 00:00, 08:00, 16:00 UTC
static constexpr Ts kFundingMask = 30'000'000;                  // +-30 s around each one (pre-registered)

// True inside +-30 s of a funding time. Fills there are flagged, and the time
// does not count as resting, so lambda = fills / resting time skips them both.
static bool in_funding(Ts t) {
    const Ts m = ((t % kFundingEvery) + kFundingEvery) % kFundingEvery;
    return m < kFundingMask || m > kFundingEvery - kFundingMask;
}

static const char* src_name(FillSrc s) {
    return s == FillSrc::Queue ? "queue" : s == FillSrc::Through ? "through" : "cross";
}

// Size resting at `px` on side `s` (0 if no such level). Our quotes sit at the
// touch, so this almost always stops at the first level.
static Lot qty_at(const ArrayBook& b, Side s, Tick px) {
    Level lv[64];
    const int n = b.top(s, lv, 64);
    for (int i = 0; i < n; ++i) if (lv[i].px == px) return lv[i].qty;
    return 0;
}

struct RawFill { FillEvent f; Half mid_before; double qp; Lot pos_after; };

struct Sim {
    MakerSide side[2];
    Ts entry, resp, qmax, depth;
    Ts rest[2] = {0, 0};               // microseconds each side spent resting, after burn-in
    Ts t = 0, t_first = -1, t_start = 0;
    Half mid_now = 0, mid_before = 0;
    bool have_mid = false;
    Lot prev_qty = 0;                  // size at our price before the current book event
    Lot pos = 0;                       // true position (exchange side)
    Lot pos_heard = 0;                 // what the strategy knows (fills reach it after resp)
    std::deque<FillEvent> unheard;
    std::uint64_t next_id = 1, n_new = 0, n_cancel = 0;
    std::vector<RawFill> fills;
    MidSeries mids;
    std::FILE* ev_fp;

    Sim(QueueRule r, Ts e, Ts rs, Lot q, Ts d, std::FILE* fp)
        : side{MakerSide(Side::Bid, r, e, rs), MakerSide(Side::Ask, r, e, rs)},
          entry(e), resp(rs), qmax(q), depth(d), ev_fp(fp) {}

    void log(const char* what, Side s, Tick px, std::uint64_t id) {
        std::fprintf(ev_fp, "%lld,%s,%s,%lld,%llu\n", (long long)t, s == Side::Bid ? "bid" : "ask",
                     what, (long long)px, (unsigned long long)id);
    }

    void record(const std::optional<FillEvent>& f, double qp) {
        if (!f) return;
        pos += sign(f->side) * f->qty;
        fills.push_back({*f, mid_before, qp, pos});
        unheard.push_back(*f);
        log("fill", f->side, f->px, side[f->side == Side::Bid ? 0 : 1].slot().id);
    }

    // ---- replay() callbacks ----
    bool pending = false;              // events at time t happened; act once t is finished

    void before_event(const ArrayBook& b, const Event& e) {
        if (pending && e.ts_exch > t) { pending = false; act(b); }   // time t is over: act on its final book
        if (t_first >= 0 && e.ts_exch > t && t >= t_start && !in_funding(t))   // resting time (to one event gap)
            for (int i = 0; i < 2; ++i)
                if (side[i].slot().st == State::Resting) rest[i] += e.ts_exch - t;
        t = std::max(t, e.ts_exch);                              // non-decreasing exchange clock
        if (t_first < 0) { t_first = t; t_start = t + kBurnIn; }
        const Tick bb = b.best_bid().px, ba = b.best_ask().px;
        for (MakerSide& o : side) {
            while (o.next_msg_time() < t) {                      // strictly before: ties go to market
                const State st = o.slot().st;
                const std::uint64_t rej = o.gtx_rejects();
                o.process_msg(qty_at(b, o.side(), o.slot().px), bb, ba);
                if (o.gtx_rejects() != rej) log("reject", o.side(), o.slot().px, o.slot().id);
                else if (st == State::PendingCancel) log("cancel_done", o.side(), o.slot().px, o.slot().id);
            }
        }
        mid_before = mid_now;
        prev_qty = 0;
        if (e.kind == Kind::Book)
            for (MakerSide& o : side)
                if (o.side() == e.side && o.slot().px == e.px) prev_qty = qty_at(b, e.side, e.px);
    }

    void on_book(const ArrayBook& b, const Event& e) {
        const Level bb = b.best_bid(), ba = b.best_ask();
        if (e.flags & mdr::kSnapshot) return;                    // book is being rebuilt: no fills
        for (MakerSide& o : side) {
            const double qp = o.queue().qp();
            record(o.on_depth(t, e.side, e.px, prev_qty, e.qty, bb.px, ba.px), qp);
        }
        if (bb.px != mdr::kNoPx && ba.px != mdr::kNoPx && bb.px < ba.px) {
            mid_now = bb.px + ba.px; have_mid = true;            // half ticks
            mids.push(t, mid_now);
        }
        pending = true;
    }

    void on_trade(const ArrayBook& /*b*/, const Event& e) {
        for (MakerSide& o : side) {
            const double qp = o.queue().qp();
            record(o.on_trade(t, e.side, e.px, e.qty), qp);
        }
        pending = true;
    }

    // ---- S0 naive touch ----
    void act(const ArrayBook& b) {
        while (!unheard.empty() && unheard.front().ts + resp <= t) {   // fills we have heard about
            pos_heard += sign(unheard.front().side) * unheard.front().qty;
            unheard.pop_front();
        }
        if (t < t_start) return;
        const Level bb = b.best_bid(), ba = b.best_ask();
        if (bb.px == mdr::kNoPx || ba.px == mdr::kNoPx || bb.px >= ba.px) return;
        for (MakerSide& o : side) {
            const bool is_bid = o.side() == Side::Bid;
            const Tick target = is_bid ? bb.px - depth : ba.px + depth;
            const bool allowed = is_bid ? pos_heard < qmax : pos_heard > -qmax;
            if (o.slot().st == State::Resting && (o.slot().px != target || !allowed)) {
                if (o.cancel(t)) { ++n_cancel; log("cancel", o.side(), o.slot().px, o.slot().id); }
            } else if (allowed && !o.busy(t)) {
                if (o.quote(next_id, target, 1, t)) { log("new", o.side(), target, next_id); ++next_id; ++n_new; }
            }
        }
    }
};

static bool parse_rule(const std::string& s, QueueRule& r) {
    if (s == "riskadverse") r = QueueRule::RiskAdverse; else if (s == "power3") r = QueueRule::Power3_2;
    else if (s == "prorata") r = QueueRule::Prorata;     else if (s == "power2") r = QueueRule::Power2_2;
    else if (s == "naive") r = QueueRule::Naive;         else return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 7) {
        std::fprintf(stderr, "usage: mm_sim <file.evt> <out_dir> [rule=prorata] [latency_ms=0] [qmax=10] [depth=0]\n");
        return 2;
    }
    try {
        const std::string out = argv[2], rule_s = argc > 3 ? argv[3] : "prorata";
        QueueRule rule;
        if (!parse_rule(rule_s, rule)) { std::fprintf(stderr, "unknown rule %s\n", rule_s.c_str()); return 2; }
        const double lat_ms = argc > 4 ? std::stod(argv[4]) : 0.0;
        const Lot qmax = argc > 5 ? std::stoll(argv[5]) : 10;
        const Tick depth = argc > 6 ? std::stoll(argv[6]) : 0;   // ticks behind best (probe mode)
        const Ts half_rt = static_cast<Ts>(lat_ms * 1000.0 / 2.0);   // entry = response = half round trip

        mdr::EventFile f(argv[1]);
        if (f.header().px_scale != 10 || f.header().qty_scale != 1000) {
            std::fprintf(stderr, "expected px_scale 10 and qty_scale 1000 (tick 0.1, lot 0.001)\n"); return 1;
        }
        std::filesystem::create_directories(out);
        std::FILE* ev = std::fopen((out + "/events.csv").c_str(), "w");
        std::fprintf(ev, "ts_exch_us,side,action,px_tick,order_id\n");

        Sim sim(rule, half_rt, half_rt, qmax, depth, ev);
        ArrayBook book;
        const auto w0 = std::chrono::steady_clock::now();
        mdr::replay(f.events(), book, sim);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        std::fclose(ev);

        // ---- markouts (as-of mid, horizons past the data end are dropped) + ledger ----
        const Ts t_end = sim.t;
        const Half M = sim.mid_now;
        Ledger L(0.0002, 0.0005);                                // VIP 0: 2 bp maker, 5 bp taker
        std::FILE* fp = std::fopen((out + "/fills.csv").c_str(), "w");
        std::fprintf(fp, "ts_exch_us,side,px_tick,qty_lot,src,qp_before,mid_before_half,"
                         "mid_100ms_half,mid_1s_half,mid_5s_half,mid_30s_half,pos_after,masked\n");
        std::uint64_t by_src[3] = {0, 0, 0}, n_kept = 0;   // n_kept: fills outside the funding mask
        for (const RawFill& r : sim.fills) {
            Half mt[4]; bool ok[4];
            for (int h = 0; h < 4; ++h) {
                ok[h] = r.f.ts + kHorizon[h] <= t_end;
                mt[h] = ok[h] ? *sim.mids.asof(r.f.ts + kHorizon[h]) : 0;
            }
            std::fprintf(fp, "%lld,%s,%lld,%lld,%s,%.4f,%lld", (long long)r.f.ts,
                         r.f.side == Side::Bid ? "bid" : "ask", (long long)r.f.px, (long long)r.f.qty,
                         src_name(r.f.src), r.qp, (long long)r.mid_before);
            for (int h = 0; h < 4; ++h) {
                if (ok[h]) std::fprintf(fp, ",%lld", (long long)mt[h]); else std::fprintf(fp, ",");
            }
            std::fprintf(fp, ",%lld,%d\n", (long long)r.pos_after, in_funding(r.f.ts) ? 1 : 0);
            if (!in_funding(r.f.ts)) ++n_kept;
            ++by_src[static_cast<int>(r.f.src)];
            // ledger: tau = 1 s; past the data end, use the final mid (that fill's markout+inventory = M - mid)
            L.add({r.f.ts, r.f.side, r.f.px, r.f.qty, true, r.mid_before, ok[kTauLedger] ? mt[kTauLedger] : M});
        }
        std::fclose(fp);

        const PnlParts p = L.split(M);
        const double n_ev = static_cast<double>(f.events().size());
        const double per_fill = sim.fills.empty() ? 0.0 : 1.0 / static_cast<double>(sim.fills.size());
        const double rest_s = static_cast<double>(sim.rest[0] + sim.rest[1]) / 1e6;   // both sides
        const double n_fill = static_cast<double>(sim.fills.size());
        const double lam = rest_s > 0 ? static_cast<double>(n_kept) / rest_s : 0.0;  // unmasked fills per resting s
        const double fill_rate = n_fill + sim.n_cancel > 0 ? n_fill / (n_fill + sim.n_cancel) : 0.0;
        std::FILE* js = std::fopen((out + "/summary.json").c_str(), "w");
        std::fprintf(js,
            "{\n  \"file\": \"%s\",\n  \"rule\": \"%s\",\n  \"latency_ms\": %g,\n  \"qmax_lots\": %lld,\n"
            "  \"depth_ticks\": %lld,\n  \"resting_s_bid\": %.3f,\n  \"resting_s_ask\": %.3f,\n"
            "  \"lambda_per_s\": %.6f,\n  \"fill_rate\": %.4f,\n"
            "  \"events\": %.0f,\n  \"seconds\": %.3f,\n  \"events_per_s\": %.0f,\n"
            "  \"orders_new\": %llu,\n  \"cancels\": %llu,\n  \"gtx_rejects\": %llu,\n"
            "  \"fills\": %zu,\n  \"fills_queue\": %llu,\n  \"fills_through\": %llu,\n  \"fills_cross\": %llu,\n"
            "  \"final_position_lots\": %lld,\n"
            "  \"pnl_usdt\": {\"spread_capture\": %.6f, \"markout_1s\": %.6f, \"inventory\": %.6f, "
            "\"fees\": %.6f, \"total\": %.6f, \"equity_check\": %.6f},\n"
            "  \"per_fill_usdt\": {\"spread_capture\": %.6f, \"markout_1s\": %.6f, \"total\": %.6f}\n}\n",
            argv[1], rule_s.c_str(), lat_ms, (long long)qmax, (long long)depth,
            static_cast<double>(sim.rest[0]) / 1e6, static_cast<double>(sim.rest[1]) / 1e6, lam, fill_rate,
            n_ev, sec, n_ev / sec,
            (unsigned long long)sim.n_new, (unsigned long long)sim.n_cancel,
            (unsigned long long)(sim.side[0].gtx_rejects() + sim.side[1].gtx_rejects()),
            sim.fills.size(), (unsigned long long)by_src[0], (unsigned long long)by_src[1],
            (unsigned long long)by_src[2], (long long)L.position(),
            p.spread_capture, p.markout, p.inventory, p.fees, p.total(), L.equity(M),
            p.spread_capture * per_fill, p.markout * per_fill, p.total() * per_fill);
        std::fclose(js);

        std::printf("[%s, %g ms] %.0f events in %.2f s = %.2fM events/s\n", rule_s.c_str(), lat_ms, n_ev, sec,
                    n_ev / sec / 1e6);
        std::printf("orders %llu, cancels %llu, fills %zu (queue %llu, through %llu, cross %llu), final pos %lld\n",
                    (unsigned long long)sim.n_new, (unsigned long long)sim.n_cancel, sim.fills.size(),
                    (unsigned long long)by_src[0], (unsigned long long)by_src[1], (unsigned long long)by_src[2],
                    (long long)L.position());
        std::printf("depth %lld: resting %.1f s, lambda %.4f fills/s, fill rate %.3f\n", (long long)depth, rest_s,
                    lam, fill_rate);
        std::printf("P&L check: parts %.6f vs equity %.6f USDT -> %s\n", p.total(), L.equity(M),
                    std::fabs(p.total() - L.equity(M)) < 1e-9 ? "OK" : "MISMATCH");
        return 0;
    } catch (const std::exception& ex) { std::fprintf(stderr, "error: %s\n", ex.what()); return 1; }
}

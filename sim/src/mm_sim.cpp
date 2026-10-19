// mm_sim.cpp - replay one .evt day through the exchange book and simulate
// our maker orders on top of it. Strategies: S0 naive touch, and (Step 7a) the
// GLFT arms S1 (Rule A) and S1t / S2 / S3 / S3r / S3m (Rule T), see mm/glft.hpp.

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
// GLFT arms (Step 7a): pass a params file as the 7th argument, one "key value"
// per line: arm (s0|s1|s1t|s2|s3|s3r|s3m), gamma, sigma, A, k, kappa, seed and
// g_tau (10 values, by best-level imbalance bin I = bid/(bid+ask)).
//   alpha = sign * kappa * g_tau[bin(I)]: sign +1 for S2, -1 for S3, 0 for S1/S1t.
//   S3r: a fresh coin flip (std::mt19937_64 seeded with `seed`) each time the
//   imbalance bin changes, the only event that changes S3's alpha (Addendum A4).
//   S3m (Addendum A5): a fresh coin flip only when the SIGN of g_tau[bin] changes, the only
//   event that makes S3 swap which side leans back. So S3m swaps sides as often as S3.
//   q = the position the strategy has HEARD about (lots). Q_max still applies.
// inv_hist.csv: seconds spent at each |true position| (after burn-in, outside
// funding). Used to choose gamma by the inventory rule, never by P&L.
//
// Outputs in <out_dir>: fills.csv, orders.csv, events.csv, inv_hist.csv, pnl_min.csv, summary.json.
// pnl_min.csv (Step 8): mark-to-market equity after fees (USDT) at t_start, at every whole UTC minute
// after it, and at the data end (final mid). Row k minus row k-1 = the P&L of fills in [t_{k-1}, t_k)
// plus the price move on the position held. The rows telescope exactly to the total P&L, which is
// what the 5-minute moving-block bootstrap resamples (Addendum A6).
// orders.csv (Step 6): one row per order we sent, with the book when we sent it
// (own-side imbalance, size at our price, mid) and how it ended (fill/cancel/
// reject/open). fills.csv carries the order_id so the two can be joined.
// usage: mm_sim <file.evt> <out_dir> [rule=prorata] [latency_ms=0] [qmax=10] [depth=0] [params_file]
//        rule: riskadverse | power3 | prorata | power2 | naive
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "mdr/books.hpp"
#include "mdr/replay.hpp"
#include "mm/fill.hpp"
#include "mm/glft.hpp"
#include "mm/ledger.hpp"
#include "mm/markout.hpp"

using namespace mm;
using mdr::ArrayBook; using mdr::Event; using mdr::Kind; using mdr::Level;

static constexpr Ts kBurnIn = 60'000'000;                       // 60 s, also covers the funding mask
static constexpr Ts kHorizon[4] = {100'000, 1'000'000, 5'000'000, 30'000'000};   // markout taus
static constexpr int kTauLedger = 1;                            // P&L split uses tau = 1 s
static constexpr Ts kFundingEvery = 8LL * 3600 * 1'000'000;     // Binance funding: 00:00, 08:00, 16:00 UTC
static constexpr Ts kFundingMask = 30'000'000;                  // +-30 s around each one (pre-registered)
static constexpr Ts kMinute = 60'000'000;                       // pnl_min.csv row spacing
static constexpr double kMakerFee = 0.0002, kTakerFee = 0.0005; // VIP 0: 2 bp maker, 5 bp taker

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

enum class Arm { S0, S1, S1t, S2, S3, S3r, S3m };

struct Params {                        // the GLFT arms' inputs (all from config/calib_dev.json + gamma)
    Arm arm = Arm::S0;
    std::string arm_name = "s0";
    double gamma = 0, sigma = 0, A = 0, k = 0, kappa = 0;
    std::uint64_t seed = 20261020;
    double g_tau[10] = {};
};

static Params read_params(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open params file " + path);
    Params P;
    std::string key;
    while (in >> key) {
        if (key == "arm") {
            in >> P.arm_name;
            const std::string& a = P.arm_name;
            P.arm = a == "s0" ? Arm::S0 : a == "s1" ? Arm::S1 : a == "s1t" ? Arm::S1t : a == "s2" ? Arm::S2
                  : a == "s3" ? Arm::S3 : a == "s3r" ? Arm::S3r : a == "s3m" ? Arm::S3m : throw std::runtime_error("unknown arm " + a);
        } else if (key == "gamma") in >> P.gamma;
        else if (key == "sigma") in >> P.sigma;
        else if (key == "A") in >> P.A;
        else if (key == "k") in >> P.k;
        else if (key == "kappa") in >> P.kappa;
        else if (key == "seed") in >> P.seed;
        else if (key == "g_tau") { for (double& g : P.g_tau) in >> g; }
        else throw std::runtime_error("unknown params key " + key);
    }
    if (P.arm != Arm::S0 && (P.gamma <= 0 || P.sigma <= 0 || P.A <= 0 || P.k <= 0))
        throw std::runtime_error("GLFT arm needs gamma, sigma, A, k > 0");
    return P;
}

struct RawFill { FillEvent f; Half mid_before; double qp; Lot pos_after; std::uint64_t id; };
struct MinRow { Ts t; double equity; Lot pos; Half mid; };

// One order we sent. Ids run 1, 2, 3, ... so order `id` lives at orders[id - 1].
struct OrderRec {
    std::uint64_t id; Side side; Tick px; Ts t_new;
    Ts t_end = -1;            // exchange time it ended (-1 = still open at the data end)
    char outcome = 'o';       // f = filled, c = cancelled, r = rejected (post-only), o = open
    double imb_own;           // (own-side top size - other side) / (sum), at send. Bid: (B-A)/(B+A)
    Lot q_size;               // size resting at our price when we sent (the line we join, roughly)
    Half mid_new;             // mid when we sent, half ticks
};

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
    std::vector<OrderRec> orders;
    std::map<Lot, Ts> inv_time;        // |true position| -> microseconds spent there
    std::int64_t cash = 0;             // tick*lots, same as the Ledger (buying costs cash)
    double fees = 0;                   // USDT, same sum in the same order as the Ledger
    Ts next_min = 0;                   // next pnl_min.csv row time
    std::vector<MinRow> minutes;
    MidSeries mids;
    std::FILE* ev_fp;
    Params P;
    Glft g{0, 0};
    std::mt19937_64 rng;
    int last_bin = -1;                 // S3r: imbalance bin at the last decision
    double coin = 1;                   // S3r / S3m: the current random sign
    int last_sgn = 0;                  // S3m: sign of g_tau at the last decision

    Sim(QueueRule r, Ts e, Ts rs, Lot q, Ts d, std::FILE* fp, const Params& prm)
        : side{MakerSide(Side::Bid, r, e, rs), MakerSide(Side::Ask, r, e, rs)},
          entry(e), resp(rs), qmax(q), depth(d), ev_fp(fp), P(prm), rng(prm.seed) {
        if (P.arm != Arm::S0) g = glft_coeffs(P.gamma, P.A, P.k, P.sigma);
    }

    void log(const char* what, Side s, Tick px, std::uint64_t id) {
        std::fprintf(ev_fp, "%lld,%s,%s,%lld,%llu\n", (long long)t, s == Side::Bid ? "bid" : "ask",
                     what, (long long)px, (unsigned long long)id);
    }

    void record(const std::optional<FillEvent>& f, double qp) {
        if (!f) return;
        pos += sign(f->side) * f->qty;
        cash -= sign(f->side) * f->px * f->qty;
        fees += kMakerFee * f->px * f->qty * kUsdtPerTickLot;
        const std::uint64_t id = side[f->side == Side::Bid ? 0 : 1].slot().id;   // filled() keeps the id
        fills.push_back({*f, mid_before, qp, pos, id});
        unheard.push_back(*f);
        end_order(id, 'f', f->ts);
        log("fill", f->side, f->px, id);
    }

    double equity(Half mid) const {                              // = Ledger::equity, during the replay
        return static_cast<double>(2 * cash + pos * mid) / 2.0 * kUsdtPerTickLot - fees;
    }

    void end_order(std::uint64_t id, char how, Ts ts) {
        OrderRec& o = orders[id - 1];
        o.outcome = how; o.t_end = ts;
    }

    // ---- replay() callbacks ----
    bool pending = false;              // events at time t happened; act once t is finished

    void before_event(const ArrayBook& b, const Event& e) {
        if (pending && e.ts_exch > t) { pending = false; act(b); }   // time t is over: act on its final book
        while (t_first >= 0 && e.ts_exch >= next_min) {          // whole minutes passed: book their equity
            minutes.push_back({next_min, equity(mid_now), pos, mid_now});
            next_min += kMinute;
        }
        if (t_first >= 0 && e.ts_exch > t && t >= t_start && !in_funding(t)) {   // time until this event
            for (int i = 0; i < 2; ++i)
                if (side[i].slot().st == State::Resting) rest[i] += e.ts_exch - t;
            inv_time[pos < 0 ? -pos : pos] += e.ts_exch - t;
        }
        t = std::max(t, e.ts_exch);                              // non-decreasing exchange clock
        if (t_first < 0) {
            t_first = t; t_start = t + kBurnIn;
            next_min = (t_start / kMinute + 1) * kMinute;          // first whole UTC minute after the burn-in
            minutes.push_back({t_start, 0.0, 0, 0});               // nothing can fill before t_start
        }
        const Tick bb = b.best_bid().px, ba = b.best_ask().px;
        for (MakerSide& o : side) {
            while (o.next_msg_time() < t) {                      // strictly before: ties go to market
                const State st = o.slot().st;
                const std::uint64_t rej = o.gtx_rejects();
                o.process_msg(qty_at(b, o.side(), o.slot().px), bb, ba);
                if (o.gtx_rejects() != rej) {
                    log("reject", o.side(), o.slot().px, o.slot().id); end_order(o.slot().id, 'r', t);
                } else if (st == State::PendingCancel) {
                    log("cancel_done", o.side(), o.slot().px, o.slot().id); end_order(o.slot().id, 'c', t);
                }
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

    // Ticks behind best for each side (0 touch, 1 back, -1 don't quote).
    // S0 / probe: `depth` on both sides. GLFT arms: Rule A or Rule T.
    void levels(const Level& bb, const Level& ba, int lv[2]) {
        if (P.arm == Arm::S0) { lv[0] = lv[1] = static_cast<int>(depth); return; }
        const double I = static_cast<double>(bb.qty) / static_cast<double>(bb.qty + ba.qty);
        const int bin = std::min(static_cast<int>(I * 10), 9);
        double sgn = 0;                                          // S1, S1t: no forecast
        if (P.arm == Arm::S2) sgn = 1;
        else if (P.arm == Arm::S3) sgn = -1;
        else if (P.arm == Arm::S3r) {                            // new flip only when S3 would re-decide
            if (bin != last_bin) { coin = (rng() & 1) ? 1 : -1; last_bin = bin; }
            sgn = coin;
        } else if (P.arm == Arm::S3m) {                     // new flip only when S3 would swap sides
            const int sg = P.g_tau[bin] > 0 ? 1 : -1;
            if (sg != last_sgn) { coin = (rng() & 1) ? 1 : -1; last_sgn = sg; }
            sgn = coin;
        }
        const double alpha = sgn * P.kappa * P.g_tau[bin];       // ticks; + = expect the mid to rise
        const double q = static_cast<double>(pos_heard);
        const double base = P.arm == Arm::S1 ? g.half_spread - 0.5 : 0.0;   // Rule A vs Rule T
        lv[0] = level_from_z(base + g.skew * q - alpha);         // bid: long -> lean back
        lv[1] = level_from_z(base - g.skew * q + alpha);         // ask: long -> lean in
    }

    // ---- the strategy: one decision per timestamp ----
    void act(const ArrayBook& b) {
        while (!unheard.empty() && unheard.front().ts + resp <= t) {   // fills we have heard about
            pos_heard += sign(unheard.front().side) * unheard.front().qty;
            unheard.pop_front();
        }
        if (t < t_start) return;
        const Level bb = b.best_bid(), ba = b.best_ask();
        if (bb.px == mdr::kNoPx || ba.px == mdr::kNoPx || bb.px >= ba.px) return;
        int lv[2];
        levels(bb, ba, lv);
        for (MakerSide& o : side) {
            const bool is_bid = o.side() == Side::Bid;
            const int l = lv[is_bid ? 0 : 1];
            const Tick target = is_bid ? bb.px - l : ba.px + l;
            const bool allowed = l >= 0 && (is_bid ? pos_heard < qmax : pos_heard > -qmax);
            if (o.slot().st == State::Resting && (o.slot().px != target || !allowed)) {
                if (o.cancel(t)) { ++n_cancel; log("cancel", o.side(), o.slot().px, o.slot().id); }
            } else if (allowed && !o.busy(t)) {
                if (o.quote(next_id, target, 1, t)) {
                    const double own = static_cast<double>(is_bid ? bb.qty : ba.qty);
                    const double oth = static_cast<double>(is_bid ? ba.qty : bb.qty);
                    orders.push_back({next_id, o.side(), target, t, -1, 'o', (own - oth) / (own + oth),
                                      qty_at(b, o.side(), target), static_cast<Half>(bb.px + ba.px)});
                    log("new", o.side(), target, next_id); ++next_id; ++n_new;
                }
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
    if (argc < 3 || argc > 8) {
        std::fprintf(stderr, "usage: mm_sim <file.evt> <out_dir> [rule=prorata] [latency_ms=0] [qmax=10] [depth=0]"
                             " [params_file]\n");
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
        const Params prm = argc > 7 ? read_params(argv[7]) : Params{};
        if (prm.arm != Arm::S0 && depth != 0) { std::fprintf(stderr, "GLFT arms need depth 0\n"); return 2; }

        mdr::EventFile f(argv[1]);
        if (f.header().px_scale != 10 || f.header().qty_scale != 1000) {
            std::fprintf(stderr, "expected px_scale 10 and qty_scale 1000 (tick 0.1, lot 0.001)\n"); return 1;
        }
        std::filesystem::create_directories(out);
        std::FILE* ev = std::fopen((out + "/events.csv").c_str(), "w");
        std::fprintf(ev, "ts_exch_us,side,action,px_tick,order_id\n");

        Sim sim(rule, half_rt, half_rt, qmax, depth, ev, prm);
        ArrayBook book;
        const auto w0 = std::chrono::steady_clock::now();
        mdr::replay(f.events(), book, sim);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        std::fclose(ev);

        // ---- markouts (as-of mid, horizons past the data end are dropped) + ledger ----
        const Ts t_end = sim.t;
        const Half M = sim.mid_now;
        Ledger L(kMakerFee, kTakerFee);
        std::FILE* fp = std::fopen((out + "/fills.csv").c_str(), "w");
        std::fprintf(fp, "ts_exch_us,side,px_tick,qty_lot,src,qp_before,mid_before_half,"
                         "mid_100ms_half,mid_1s_half,mid_5s_half,mid_30s_half,pos_after,masked,order_id\n");
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
            std::fprintf(fp, ",%lld,%d,%llu\n", (long long)r.pos_after, in_funding(r.f.ts) ? 1 : 0,
                         (unsigned long long)r.id);
            if (!in_funding(r.f.ts)) ++n_kept;
            ++by_src[static_cast<int>(r.f.src)];
            // ledger: tau = 1 s; past the data end, use the final mid (that fill's markout+inventory = M - mid)
            L.add({r.f.ts, r.f.side, r.f.px, r.f.qty, true, r.mid_before, ok[kTauLedger] ? mt[kTauLedger] : M});
        }
        std::fclose(fp);

        // orders.csv: mid 5 s after we sent (blank if past the data end), for P(fill | 5 s return)
        std::FILE* op = std::fopen((out + "/orders.csv").c_str(), "w");
        std::fprintf(op, "order_id,side,px_tick,t_new_us,t_end_us,outcome,imb_own,q_size_lot,"
                         "mid_new_half,mid_5s_half,masked\n");
        for (const OrderRec& o : sim.orders) {
            std::fprintf(op, "%llu,%s,%lld,%lld,%lld,%c,%.6f,%lld,%lld,", (unsigned long long)o.id,
                         o.side == Side::Bid ? "bid" : "ask", (long long)o.px, (long long)o.t_new,
                         (long long)o.t_end, o.outcome, o.imb_own, (long long)o.q_size, (long long)o.mid_new);
            if (o.t_new + kHorizon[2] <= t_end) std::fprintf(op, "%lld", (long long)*sim.mids.asof(o.t_new + kHorizon[2]));
            std::fprintf(op, ",%d\n", in_funding(o.t_new) ? 1 : 0);
        }
        // pnl_min.csv: the minute rows + a final row at the data end, marked at the final mid
        sim.minutes.push_back({t_end, sim.equity(M), sim.pos, M});
        std::FILE* mp = std::fopen((out + "/pnl_min.csv").c_str(), "w");
        std::fprintf(mp, "t_us,equity_usdt,pos_lots,mid_half\n");
        for (const MinRow& r : sim.minutes)
            std::fprintf(mp, "%lld,%.12f,%lld,%lld\n", (long long)r.t, r.equity, (long long)r.pos, (long long)r.mid);
        std::fclose(mp);

        // inv_hist.csv + the time-weighted 99th percentile of |position|
        std::FILE* ip = std::fopen((out + "/inv_hist.csv").c_str(), "w");
        std::fprintf(ip, "abs_pos_lots,seconds\n");
        Ts inv_total = 0;
        for (const auto& [q, us] : sim.inv_time) {
            std::fprintf(ip, "%lld,%.6f\n", (long long)q, static_cast<double>(us) / 1e6);
            inv_total += us;
        }
        std::fclose(ip);
        long long q99 = 0;
        for (Ts acc = 0; const auto& [q, us] : sim.inv_time) {
            acc += us; q99 = q;
            if (static_cast<double>(acc) >= 0.99 * static_cast<double>(inv_total)) break;
        }

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
            "  \"arm\": \"%s\",\n  \"gamma\": %.6g,\n  \"half_spread_ticks\": %.6f,\n"
            "  \"skew_ticks_per_lot\": %.6f,\n  \"q_abs_p99_lots\": %lld,\n"
            "  \"depth_ticks\": %lld,\n  \"resting_s_bid\": %.3f,\n  \"resting_s_ask\": %.3f,\n"
            "  \"lambda_per_s\": %.6f,\n  \"fill_rate\": %.4f,\n"
            "  \"events\": %.0f,\n  \"seconds\": %.3f,\n  \"events_per_s\": %.0f,\n"
            "  \"orders_new\": %llu,\n  \"cancels\": %llu,\n  \"gtx_rejects\": %llu,\n"
            "  \"fills\": %zu,\n  \"fills_queue\": %llu,\n  \"fills_through\": %llu,\n  \"fills_cross\": %llu,\n"
            "  \"final_position_lots\": %lld,\n"
            "  \"pnl_usdt\": {\"spread_capture\": %.6f, \"markout_1s\": %.6f, \"inventory\": %.6f, "
            "\"fees\": %.6f, \"total\": %.6f, \"equity_check\": %.6f},\n"
            "  \"per_fill_usdt\": {\"spread_capture\": %.6f, \"markout_1s\": %.6f, \"total\": %.6f}\n}\n",
            argv[1], rule_s.c_str(), lat_ms, (long long)qmax, prm.arm_name.c_str(), prm.gamma,
            sim.g.half_spread, sim.g.skew, q99, (long long)depth,
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

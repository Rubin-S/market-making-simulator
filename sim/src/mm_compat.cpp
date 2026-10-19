// mm_compat.cpp - "compat mode": the same naive touch strategy, run with
// hftbacktest 2.4.4's exact machinery, so its fills can be checked one by one
// against the real hftbacktest (oracle/run_oracle.py).
//
// What is OURS here (and what the oracle tests):
//   - the queue model: mm::QueuePos from queue.hpp, all 4 rules
//   - the P&L: mm::Ledger from ledger.hpp, in integer ticks and lots
//   - the three fill sources (queue / through / cross) and the GTX reject
// What is COPIED from hftbacktest (so the comparison is apples to apples):
//   - its event file (the .npz it converted, dumped to raw binary)
//   - its book rules (hashmapmarketdepth.rs: crossed levels stay as ghosts)
//   - its 4-cursor event loop (backtest/mod.rs) and order buses (order.rs)
//   - its local order bookkeeping (proc/local.rs)
// Every block names the Rust file it mirrors.
//
// Tie order at equal timestamps: local feed < local responses < exchange feed
// < exchange orders. So market events at t come before our orders arriving at t.
//
// usage: mm_compat <slice.bin> <out_dir> <tag> <single|real> <rule> <latency_ms> <warmup_s>
//        rule: riskadverse | power3_2 | prorata | power2_2
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "mm/ledger.hpp"
#include "mm/queue.hpp"

using namespace mm;

// ---------------- hftbacktest's event row (64 bytes) ----------------
struct HEv {
    std::uint64_t ev;
    std::int64_t exch_ts, local_ts;   // nanoseconds
    double px, qty;                   // USDT, BTC
    std::uint64_t order_id;
    std::int64_t ival;
    double fval;
};
static_assert(sizeof(HEv) == 64);

constexpr std::uint64_t EXCH = 1ull << 31, LOCAL = 1ull << 30, BUY = 1ull << 29, SELL = 1ull << 28;
constexpr std::uint64_t DEPTH = 1, TRADE = 2, CLEAR = 3, SNAP = 4;
constexpr std::int64_t IMIN = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t IMAX = std::numeric_limits<std::int64_t>::max();

static Tick to_tick(double px) { return round_half_away(px / kTickUsdt); }
static Lot to_lot(double q) { return round_half_away(q / kLotBtc); }

// ---------------- book (depth/hashmapmarketdepth.rs) ----------------
struct HbtBook {
    std::unordered_map<Tick, Lot> bid, ask;
    Tick bb = IMIN, ba = IMAX, low_bid = IMAX, high_ask = IMIN;

    static Lot get(const std::unordered_map<Tick, Lot>& m, Tick t) {
        auto it = m.find(t); return it == m.end() ? 0 : it->second;
    }
    Tick below(Tick start, Tick end) const {          // best bid in [end, start)
        if (end == IMAX || start == IMIN) return IMIN;
        Tick t = start - 1;
        for (int n = 0; t >= end && n < 4000; --t, ++n) if (get(bid, t) > 0) return t;
        if (t < end) return IMIN;
        Tick best = IMIN;                              // far away: scan the keys instead (same answer)
        for (auto& [k, q] : bid) if (k >= end && k < start && q > 0 && k > best) best = k;
        return best;
    }
    Tick above(Tick start, Tick end) const {          // best ask in (start, end]
        if (end == IMIN || start == IMAX) return IMAX;
        Tick t = start + 1;
        for (int n = 0; t <= end && n < 4000; ++t, ++n) if (get(ask, t) > 0) return t;
        if (t > end) return IMAX;
        Tick best = IMAX;
        for (auto& [k, q] : ask) if (k > start && k <= end && q > 0 && k < best) best = k;
        return best;
    }
    struct Upd { Tick t, prev_best, best; Lot prev, now; };
    Upd update_bid(double px, double qty) {           // L86-139
        const Tick t = to_tick(px); const Lot q = to_lot(qty); const Tick pbb = bb;
        const Lot prev = get(bid, t);
        if (q > 0) bid[t] = q; else bid.erase(t);
        if (q == 0) {
            if (t == bb) { bb = below(bb, low_bid); if (bb == IMIN) low_bid = IMAX; }
        } else {
            if (t > bb) { bb = t; if (bb >= ba) ba = above(bb, high_ask); }   // crossed asks stay (ghosts)
            low_bid = std::min(low_bid, t);
        }
        return {t, pbb, bb, prev, q};
    }
    Upd update_ask(double px, double qty) {           // L141-194
        const Tick t = to_tick(px); const Lot q = to_lot(qty); const Tick pba = ba;
        const Lot prev = get(ask, t);
        if (q > 0) ask[t] = q; else ask.erase(t);
        if (q == 0) {
            if (t == ba) { ba = above(ba, high_ask); if (ba == IMAX) high_ask = IMIN; }
        } else {
            if (t < ba) { ba = t; if (bb >= ba) bb = below(ba, low_bid); }
            high_ask = std::max(high_ask, t);
        }
        return {t, pba, ba, prev, q};
    }
    void clear(int side, double px) {                 // L196-240
        if (side == 1) {
            if (std::isfinite(px)) {
                const Tick cu = to_tick(px);
                if (bb != IMIN) std::erase_if(bid, [&](auto& kv) { return kv.first >= cu && kv.first <= bb; });
                bb = below(cu - 1, low_bid);
            } else { bid.clear(); bb = IMIN; }
            if (bb == IMIN) low_bid = IMAX;
        } else if (side == -1) {
            if (std::isfinite(px)) {
                const Tick cu = to_tick(px);
                if (ba != IMAX) std::erase_if(ask, [&](auto& kv) { return kv.first >= ba && kv.first <= cu; });
                ba = above(cu + 1, high_ask);
            } else { ask.clear(); ba = IMAX; }
            if (ba == IMAX) high_ask = IMIN;
        } else {
            clear(1, INFINITY); clear(-1, INFINITY);
        }
    }
};

// ---------------- orders (backtest/order.rs) ----------------
enum class St { None, New, Filled, Canceled, Expired };
enum class Rq { None, New, Cancel, Rejected };

struct HOrder {
    std::uint64_t id = 0;
    int side = 0;                     // +1 buy, -1 sell
    Tick tick = 0;
    St status = St::None;
    Rq req = Rq::None;
    std::int64_t local_ts = 0, exch_ts = 0;
    QueuePos q;
    const char* reason = "";
};

struct Bus {                          // FIFO; a timestamp never goes below the last one queued
    std::deque<std::pair<HOrder, std::int64_t>> q;
    void push(const HOrder& o, std::int64_t ts) {
        const std::int64_t last = q.empty() ? 0 : q.back().second;
        q.push_back({o, std::max(ts, last)});
    }
    std::int64_t head() const { return q.empty() ? IMAX : q.front().second; }
};

// ---------------- the simulator ----------------
struct Sim {
    std::vector<HEv> d;
    std::vector<std::size_t> loc_rows, exc_rows;
    std::size_t li = 0, ei = 0;
    HbtBook lbook, xbook;
    QueueRule rule;
    std::int64_t lat;
    Bus to_exch, to_local;
    std::vector<HOrder> xorders;                       // resting at the exchange, in arrival order
    std::map<std::uint64_t, HOrder> lorders;           // what the strategy knows, by id
    Ledger ledger{0.0002, 0.0005};
    std::int64_t cur = IMAX;

    Sim(std::vector<HEv> data, QueueRule r, std::int64_t lat_ns) : d(std::move(data)), rule(r), lat(lat_ns) {
        for (std::size_t i = 0; i < d.size(); ++i) {
            if (d[i].ev & LOCAL) loc_rows.push_back(i);
            if (d[i].ev & EXCH) exc_rows.push_back(i);
        }
    }

    // ----- exchange side (proc/nopartialfillexchange.rs) -----
    void respond(const HOrder& o, std::int64_t ts) { to_local.push(o, ts + lat); }

    void fill(std::size_t i, std::int64_t ts, const char* why) {   // whole order, at our own price
        HOrder& o = xorders[i];
        o.status = St::Filled; o.exch_ts = ts; o.reason = why;
        respond(o, ts);
        xorders.erase(xorders.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void exch_row(const HEv& r) {
        const std::uint64_t kind = r.ev & 0xff;
        const int side = (r.ev & BUY) ? 1 : (r.ev & SELL) ? -1 : 0;
        const std::int64_t ts = r.exch_ts;
        HbtBook& b = xbook;
        if (kind == CLEAR) { b.clear(side, r.px); return; }
        if ((kind == DEPTH || kind == SNAP) && side == 1) {
            const auto u = b.update_bid(r.px, r.qty);
            for (HOrder& o : xorders) if (o.side == 1 && o.tick == u.t) o.q.on_depth(u.prev, u.now);
            if (u.best > u.prev_best)                  // best bid moved up: our asks it reached are filled
                for (std::size_t i = 0; i < xorders.size(); ++i) {
                    const HOrder& o = xorders[i];
                    if (o.side == -1 && ((u.prev_best == IMIN && o.tick <= u.best) ||
                                         (u.prev_best < o.tick && o.tick <= u.best))) { fill(i, ts, "cross"); --i; }
                }
        } else if ((kind == DEPTH || kind == SNAP) && side == -1) {
            const auto u = b.update_ask(r.px, r.qty);
            for (HOrder& o : xorders) if (o.side == -1 && o.tick == u.t) o.q.on_depth(u.prev, u.now);
            if (u.best < u.prev_best)
                for (std::size_t i = 0; i < xorders.size(); ++i) {
                    const HOrder& o = xorders[i];
                    if (o.side == 1 && ((u.prev_best == IMAX && o.tick >= u.best) ||
                                        (u.best <= o.tick && o.tick < u.prev_best))) { fill(i, ts, "cross"); --i; }
                }
        } else if (kind == TRADE && side != 0) {       // a buy print hits asks, a sell print hits bids
            const Tick tt = to_tick(r.px);
            const Lot q = to_lot(r.qty);
            for (std::size_t i = 0; i < xorders.size(); ++i) {
                HOrder& o = xorders[i];
                if (o.side != -side) continue;
                const bool reach = side == 1 ? (b.bb == IMIN || (b.bb < o.tick && o.tick <= tt))
                                             : (b.ba == IMAX || (tt <= o.tick && o.tick < b.ba));
                if (!reach) continue;
                const bool through = side == 1 ? o.tick < tt : o.tick > tt;   // the print went past our price
                if (through) { fill(i, ts, "through"); --i; }
                else if (o.tick == tt && o.q.on_trade(q) > 0) { fill(i, ts, "queue"); --i; }
            }
        }
    }

    void exch_orders(std::int64_t ts) {                // process_recv_order L609-636
        while (!to_exch.q.empty() && to_exch.q.front().second == ts) {
            HOrder o = to_exch.q.front().first; to_exch.q.pop_front();
            if (o.req == Rq::New) {
                o.req = Rq::None; o.exch_ts = ts;
                const bool cross = o.side == 1 ? o.tick >= xbook.ba : o.tick <= xbook.bb;
                if (cross) o.status = St::Expired;     // GTX: would take liquidity, so rejected
                else {
                    o.q.rule = rule;
                    o.q.join(HbtBook::get(o.side == 1 ? xbook.bid : xbook.ask, o.tick));
                    o.status = St::New;
                    xorders.push_back(o);
                }
                respond(o, ts);
            } else if (o.req == Rq::Cancel) {
                o.req = Rq::None;
                std::size_t i = 0;
                while (i < xorders.size() && xorders[i].id != o.id) ++i;
                if (i == xorders.size()) { o.req = Rq::Rejected; o.exch_ts = ts; respond(o, ts); }   // too late
                else {
                    HOrder x = xorders[i];
                    xorders.erase(xorders.begin() + static_cast<std::ptrdiff_t>(i));
                    x.status = St::Canceled; x.exch_ts = ts; x.req = Rq::None;
                    respond(x, ts);
                }
            }
        }
    }

    // ----- local side (proc/local.rs) -----
    void local_row(const HEv& r) {
        const std::uint64_t kind = r.ev & 0xff;
        const int side = (r.ev & BUY) ? 1 : (r.ev & SELL) ? -1 : 0;
        if (kind == CLEAR) lbook.clear(side, r.px);
        else if ((kind == DEPTH || kind == SNAP) && side == 1) lbook.update_bid(r.px, r.qty);
        else if ((kind == DEPTH || kind == SNAP) && side == -1) lbook.update_ask(r.px, r.qty);
    }

    void local_orders(std::int64_t ts) {
        while (!to_local.q.empty() && to_local.q.front().second == ts) {
            HOrder o = to_local.q.front().first; to_local.q.pop_front();
            if (o.status == St::Filled)
                ledger.add({o.exch_ts, o.side == 1 ? Side::Bid : Side::Ask, o.tick, 1, true, 0, 0});
            auto it = lorders.find(o.id);
            if (it == lorders.end()) continue;
            HOrder& lo = it->second;
            if (o.req == Rq::Rejected) {
                if (o.local_ts == lo.local_ts) {
                    if (lo.req == Rq::New) { lo.req = Rq::None; lo.status = St::Expired; }
                    else lo.req = Rq::None;
                }
            } else {
                const std::int64_t keep = lo.local_ts;
                lo = o; lo.local_ts = keep;
            }
        }
    }

    // ----- wait_next_feed(include_order_resp=true) (backtest/mod.rs) -----
    std::int64_t next_ts(int& k) const {
        const std::int64_t t[4] = {
            li < loc_rows.size() ? d[loc_rows[li]].local_ts : IMAX, to_local.head(),
            ei < exc_rows.size() ? d[exc_rows[ei]].exch_ts : IMAX, to_exch.head()};
        k = 0;
        for (int j = 1; j < 4; ++j) if (t[j] < t[k]) k = j;   // first minimum = tie order
        return t[k];
    }
    // returns 1 = end of data, 0 = timeout, 2 = market data, 3 = order response
    int wait_next_feed(std::int64_t timeout) {
        int k;
        if (cur == IMAX) { const std::int64_t ts = next_ts(k); if (ts == IMAX) return 1; cur = ts; }
        std::int64_t target = cur + timeout;
        int result = 0;
        while (true) {
            const std::int64_t ts = next_ts(k);
            if (ts == IMAX) return 1;
            if (ts > target) { cur = target; return result; }
            if (k == 0) { local_row(d[loc_rows[li]]); ++li; target = ts; result = 2; }
            else if (k == 1) { local_orders(ts); target = ts; result = 3; }
            else if (k == 2) { exch_row(d[exc_rows[ei]]); ++ei; }
            else exch_orders(ts);
        }
    }

    void submit(std::uint64_t id, int side, Tick tick) {
        HOrder o; o.id = id; o.side = side; o.tick = tick; o.req = Rq::New; o.local_ts = cur;
        lorders[id] = o;
        to_exch.push(o, cur + lat);
    }
    void cancel(std::uint64_t id) {
        HOrder& lo = lorders[id];
        lo.req = Rq::Cancel; lo.local_ts = cur;
        to_exch.push(lo, cur + lat);
    }
};

static std::vector<HEv> load(const std::string& path, bool single) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open " + path);
    const auto bytes = static_cast<std::size_t>(in.tellg());
    std::vector<HEv> d(bytes / sizeof(HEv));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size() * sizeof(HEv)));
    if (!single) return d;
    std::vector<HEv> s;                                // one clock: keep each event once, local = exchange
    for (HEv e : d) if (e.ev & EXCH) { e.local_ts = e.exch_ts; e.ev |= LOCAL; s.push_back(e); }
    return s;
}

static bool parse_rule(const std::string& s, QueueRule& r) {
    if (s == "riskadverse") r = QueueRule::RiskAdverse; else if (s == "power3_2") r = QueueRule::Power3_2;
    else if (s == "prorata") r = QueueRule::Prorata;     else if (s == "power2_2") r = QueueRule::Power2_2;
    else return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc != 8) {
        std::fprintf(stderr, "usage: mm_compat <slice.bin> <out_dir> <tag> <single|real> <rule> <latency_ms> <warmup_s>\n");
        return 2;
    }
    try {
        const std::string out = argv[2], tag = argv[3], clock = argv[4];
        QueueRule rule;
        if (!parse_rule(argv[5], rule)) { std::fprintf(stderr, "unknown rule %s\n", argv[5]); return 2; }
        const std::int64_t lat = static_cast<std::int64_t>(std::stod(argv[6]) * 1e6);    // ms -> ns
        const std::int64_t warm = static_cast<std::int64_t>(std::stod(argv[7]) * 1e9);   // s -> ns

        Sim sim(load(argv[1], clock == "single"), rule, lat);
        std::filesystem::create_directories(out);
        std::FILE* fp = std::fopen((out + "/cppfills_" + tag + ".csv").c_str(), "w");
        std::fprintf(fp, "order_id,side,price_tick,qty,exch_fill_ts,local_seen_ts,reason\n");

        // ----- the naive touch strategy: same rules as oracle/naive_touch.py -----
        std::uint64_t next_id = 1, n_sub = 0, n_can = 0, n_exp = 0, n_fill = 0;
        std::int64_t start = -1;
        while (true) {
            const int r = sim.wait_next_feed(1'000'000'000);       // wake at least every 1 s
            if (r == 1) break;
            const std::int64_t now = sim.cur;
            if (start < 0) start = now;
            for (auto it = sim.lorders.begin(); it != sim.lorders.end();) {   // log, then clear inactive
                const HOrder& o = it->second;
                if (o.status == St::Filled) {
                    std::fprintf(fp, "%llu,%d,%lld,0.001,%lld,%lld,%s\n", (unsigned long long)o.id, o.side,
                                 (long long)o.tick, (long long)o.exch_ts, (long long)now, o.reason);
                    ++n_fill;
                } else if (o.status == St::Expired) ++n_exp;
                if (o.status == St::Filled || o.status == St::Canceled || o.status == St::Expired)
                    it = sim.lorders.erase(it);
                else ++it;
            }
            if (now < start + warm || r == 3) continue;            // never act on an order-response wake
            const Tick bb = sim.lbook.bb, ba = sim.lbook.ba;
            if (bb == IMIN || ba == IMAX || bb >= ba) continue;
            bool have_bid = false, have_ask = false;
            std::vector<std::uint64_t> to_cancel;
            for (auto& [id, o] : sim.lorders) {
                (o.side == 1 ? have_bid : have_ask) = true;
                const Tick target = o.side == 1 ? bb : ba;
                if (o.status == St::New && o.req == Rq::None && o.tick != target) to_cancel.push_back(id);
            }
            for (std::uint64_t id : to_cancel) { sim.cancel(id); ++n_can; }
            if (!have_bid) { sim.submit(next_id++, 1, bb); ++n_sub; }
            if (!have_ask) { sim.submit(next_id++, -1, ba); ++n_sub; }
        }
        std::fclose(fp);

        const Half M = sim.lbook.bb + sim.lbook.ba;               // final local mid, half ticks
        const double eq_net = sim.ledger.equity(M);
        const double eq_gross = eq_net + sim.ledger.fees();
        const double pos = static_cast<double>(sim.ledger.position()) * kLotBtc;
        std::FILE* js = std::fopen((out + "/cppsummary_" + tag + ".json").c_str(), "w");
        std::fprintf(js, "{\"tag\": \"%s\", \"n_fills\": %llu, \"n_submits\": %llu, \"n_cancels\": %llu, "
                         "\"n_expired\": %llu, \"final_position\": %.6f, \"fee\": %.10f, "
                         "\"equity_gross\": %.10f, \"equity_net\": %.10f}\n",
                     tag.c_str(), (unsigned long long)n_fill, (unsigned long long)n_sub, (unsigned long long)n_can,
                     (unsigned long long)n_exp, pos, sim.ledger.fees(), eq_gross, eq_net);
        std::fclose(js);
        std::printf("%-28s fills %5llu  submits %5llu  cancels %5llu  pos %+.3f  equity_net %+.6f\n", tag.c_str(),
                    (unsigned long long)n_fill, (unsigned long long)n_sub, (unsigned long long)n_can, pos, eq_net);
        return 0;
    } catch (const std::exception& ex) { std::fprintf(stderr, "error: %s\n", ex.what()); return 1; }
}

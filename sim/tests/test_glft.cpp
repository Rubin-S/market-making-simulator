// test_glft.cpp - Step 7a test: GLFT coefficients and the discrete level rule.
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "mm/glft.hpp"

using namespace mm;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); ++failures; } } while (0)

// Hand values (computed separately in Python with the same formulas):
//   gamma 0.1, A 1, k 0.5, sigma 2:  c1 = ln(1.2)/0.1 = 1.823216
//   c2 = sqrt(0.1/(2*1*0.5) * 1.2^6) = 0.546442  ->  h = 1.823216 + 0.546442 = 2.369657,  s = 1.092883
static void coeffs_by_hand() {
    const Glft g = glft_coeffs(0.1, 1.0, 0.5, 2.0);
    CHECK(std::fabs(g.half_spread - 2.369657) < 1e-6);
    CHECK(std::fabs(g.skew - 1.092883) < 1e-6);
    std::printf("coeffs_by_hand: h = %.6f (expect 2.369657), s = %.6f (expect 1.092883)\n", g.half_spread, g.skew);
}

// As gamma -> 0: c1 -> 1/k and the skew -> 0 (no risk aversion = no inventory skew).
static void small_gamma_limit() {
    const Glft g = glft_coeffs(1e-12, 0.78, 0.245, 58.0);
    CHECK(std::fabs(g.half_spread - 1.0 / 0.245) < 1e-3);
    CHECK(g.skew < 1e-3);
    std::printf("small_gamma_limit: h = %.4f (expect 1/k = %.4f), s = %.2e\n", g.half_spread, 1.0 / 0.245, g.skew);
}

// The skew grows with gamma (more risk averse = leans harder against inventory).
static void skew_grows_with_gamma() {
    double prev = 0;
    for (double gm : {1e-8, 1e-6, 1e-4, 1e-2}) {
        const double s = glft_coeffs(gm, 0.78, 0.245, 58.0).skew;
        CHECK(s > prev);
        prev = s;
    }
}

static void levels() {
    CHECK(level_from_z(-3.0) == 0);    // wants to be inside the spread: best we can do is the touch
    CHECK(level_from_z(0.5) == 0);
    CHECK(level_from_z(0.51) == 1);
    CHECK(level_from_z(1.5) == 1);
    CHECK(level_from_z(1.51) == -1);
}

int main() {
    coeffs_by_hand();
    small_gamma_limit();
    skew_grows_with_gamma();
    levels();
    if (failures) { std::fprintf(stderr, "%d check(s) FAILED\n", failures); return 1; }
    std::printf("all glft tests passed\n");
}

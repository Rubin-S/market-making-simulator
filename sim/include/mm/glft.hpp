// glft.hpp - the GLFT quoting maths (Gueant, Lehalle, Fernandez-Tapia; infinite
// horizon closed form) and the two discrete quote rules from the README.
//
// Units: prices in ticks, size in lots (one quote = 1 lot, so Delta = 1),
// time in seconds. sigma = ticks / sqrt(s), A = fills per second at the touch,
// k = per tick (lambda(d) = A * exp(-k d)), gamma = risk aversion (xi = gamma).
//   c1 = ln(1 + gamma/k) / gamma            (tends to 1/k as gamma -> 0)
//   c2 = sqrt( gamma / (2 A k) * (1 + gamma/k)^(k/gamma + 1) )
//   half spread h = c1 + sigma*c2/2,  skew s = sigma*c2  (ticks per lot held)
//
// Distance behind best, z, in ticks (q = position in lots, alpha = forecast tilt):
//   Rule A (S1, literal GLFT):  z_bid = (h - 1/2) + s*q - alpha,  z_ask = (h - 1/2) - s*q + alpha
//   Rule T (Addendum A1):       z_bid =            s*q - alpha,  z_ask =           - s*q + alpha
// Then: z <= 0.5 -> at the touch (0 back), z <= 1.5 -> 1 tick back, else don't quote (-1).
#pragma once
#include <cmath>

namespace mm {

struct Glft { double half_spread, skew; };

inline Glft glft_coeffs(double gamma, double A, double k, double sigma) {
    const double c1 = std::log1p(gamma / k) / gamma;
    const double c2 = std::sqrt(gamma / (2.0 * A * k) * std::exp((k / gamma + 1.0) * std::log1p(gamma / k)));
    return {c1 + 0.5 * sigma * c2, sigma * c2};
}

// Ticks behind best for a distance z: 0, 1, or -1 = don't quote this side.
inline int level_from_z(double z) { return z <= 0.5 ? 0 : z <= 1.5 ? 1 : -1; }

}  // namespace mm

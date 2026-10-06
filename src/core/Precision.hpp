// Frame Window Mod - NaNDL "Precision" (L*) model (no Geode dependencies).
//
// Model (from the public NaNDL formula page, https://nandl.pages.dev/):
//   w_i  = N_i / f                         time window of input i (s), N_i = frame window
//   s_i  = 1/2 * w_i * L * prod(lambda)    sigma-value of input i
//   p_i  = P(|X| <= s_i), X ~ N(0, 1)      = erf(s_i / sqrt(2))
//   P(C) = prod p_i
//   E[T_A] = t_n P(C) + sum t_i r_i q_i    r_i = prod_{j<i} p_j, q_i = 1 - p_i
//   E[T_C] = E[T_A] / P(C)
//   L*   : E[T_C(L*)] = target (24 h by default)
// Modifiers: nerve  exp(-kT * t_i),  fatigue exp(-kU * i),
//            CPS    (4 / max(1, 2 c_i)) ^ kC,  c_i = (i - i') / (t_i - t_i')
#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace fwm {

struct PrecisionInput {
    double tick = 0;    // time position in ticks (frames), measured from the start of the attempt
    double window = 1;  // frame window in ticks
};

struct PrecisionParams {
    double tps = 240.0;
    double respawnSeconds = 0.0;
    double targetSeconds = 86400.0;
    double kNerve = 0.0016520833717346;
    double kFatigue = 0.0002727763242154;
    double kCps = 0.2784421686721826;
};

enum class PVariant { Base = 0, N, F, C, NF, NC, FC, NFC };
constexpr int kVariantCount = 8;
const char* variantName(PVariant v);

struct PrecisionResult {
    std::array<double, kVariantCount> value{};
    std::array<bool, kVariantCount> ok{};
};

// Expected time (seconds) to complete the level for precision L (exposed for tests).
double expectedCompletionTime(std::vector<PrecisionInput> const& inputs, PrecisionParams const& p, bool nerve,
                              bool fatigue, bool cps, double L);

// Solve E[T_C](L) = target for the first `count` inputs. Returns NaN when no solution exists.
double solvePrecision(PrecisionInput const* inputs, std::size_t count, PrecisionParams const& p, bool nerve,
                      bool fatigue, bool cps);

PrecisionResult solveAll(std::vector<PrecisionInput> const& inputs, PrecisionParams const& p);

}  // namespace fwm

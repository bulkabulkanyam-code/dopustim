#include "Precision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fwm {

const char* variantName(PVariant v) {
    switch (v) {
        case PVariant::Base: return "Base";
        case PVariant::N: return "Nerve";
        case PVariant::F: return "Fatigue";
        case PVariant::C: return "CPS";
        case PVariant::NF: return "Nerve + Fatigue";
        case PVariant::NC: return "Nerve + CPS";
        case PVariant::FC: return "Fatigue + CPS";
        case PVariant::NFC: return "Nerve + Fatigue + CPS";
    }
    return "?";
}

namespace {

constexpr double kInvSqrt2 = 0.70710678118654752440;

// Coefficient c_m such that x_m = c_m * L is the erf argument of input m
// (p = erf(x), q = erfc(x)); also fills the attempt time t_m in seconds.
struct Prepared {
    std::vector<double> coef;  // c_m
    std::vector<double> time;  // t_m (seconds, includes respawn)
};

Prepared prepare(PrecisionInput const* in, std::size_t n, PrecisionParams const& p, bool nerve, bool fatigue,
                 bool cps) {
    Prepared out;
    out.coef.resize(n);
    out.time.resize(n);
    double fps = p.tps > 0 ? p.tps : 240.0;
    double prevTime = 0.0;
    long long prevNumber = 0;
    for (std::size_t m = 0; m < n; ++m) {
        double t = p.respawnSeconds + in[m].tick / fps;
        long long number = static_cast<long long>(m) + 1;
        double w = (in[m].window > 0 ? in[m].window : 1.0) / fps;

        double mult = 1.0;
        if (nerve) mult *= std::exp(-p.kNerve * t);
        if (fatigue) {
            double f = std::exp(-p.kFatigue * static_cast<double>(number));
            mult *= (std::isfinite(f) ? f : 0.0);
        }
        if (cps) {
            double dt = (t - prevTime) / static_cast<double>(number - prevNumber);
            if (!(dt > 0.0) || !std::isfinite(dt)) dt = 1.0;
            double maxVal = std::max(1.0, 2.0 / dt);
            double c = std::pow(4.0 / maxVal, p.kCps);
            if (!std::isfinite(c) || c < 0.0) c = 1.0;
            mult *= c;
        }
        double c = w * 0.5 * kInvSqrt2 * mult;
        if (!std::isfinite(c) || c < 0.0) c = 0.0;
        out.coef[m] = c;
        out.time[m] = t;
        prevTime = t;
        prevNumber = number;
    }
    return out;
}

// E[T_C] = t_n + sum_j t_j q_j / S_j,  S_j = prod_{k>=j} p_k   (numerically stable form)
double expectedTime(Prepared const& pr, double L) {
    std::size_t n = pr.coef.size();
    if (n == 0) return 0.0;
    if (!(L > 0.0)) return std::numeric_limits<double>::infinity();
    double logS = 0.0;  // log of suffix product
    double sum = 0.0;
    for (std::size_t idx = n; idx-- > 0;) {
        double x = pr.coef[idx] * L;
        double q = std::erfc(x);
        double p = std::erf(x);
        if (!(p > 0.0)) return std::numeric_limits<double>::infinity();
        logS += std::log(p);
        double term = pr.time[idx] * q * std::exp(-logS);
        if (!std::isfinite(term)) return std::numeric_limits<double>::infinity();
        sum += term;
    }
    return pr.time[n - 1] + sum;
}

}  // namespace

double expectedCompletionTime(std::vector<PrecisionInput> const& inputs, PrecisionParams const& p, bool nerve,
                              bool fatigue, bool cps, double L) {
    auto pr = prepare(inputs.data(), inputs.size(), p, nerve, fatigue, cps);
    return expectedTime(pr, L);
}

double solvePrecision(PrecisionInput const* inputs, std::size_t count, PrecisionParams const& p, bool nerve,
                      bool fatigue, bool cps) {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    if (count == 0 || !(p.targetSeconds > 0.0)) return nan;
    auto pr = prepare(inputs, count, p, nerve, fatigue, cps);
    // E[T_C] > t_n always, so a target not above the last input time has no solution.
    if (!(p.targetSeconds > pr.time[count - 1])) return nan;

    auto f = [&](double L) { return expectedTime(pr, L) - p.targetSeconds; };

    double lo = 1e-9;
    double hi = 1.0;
    int guard = 0;
    while (f(hi) > 0.0 && guard++ < 200) hi *= 2.0;
    if (f(hi) > 0.0) return nan;
    if (f(lo) < 0.0) return lo;

    // Bisection in log space; E[T_C] is strictly decreasing in L.
    double a = std::log(lo), b = std::log(hi);
    for (int i = 0; i < 200; ++i) {
        double mid = 0.5 * (a + b);
        if (f(std::exp(mid)) > 0.0) a = mid; else b = mid;
        if (b - a < 1e-13) break;
    }
    return std::exp(0.5 * (a + b));
}

PrecisionResult solveAll(std::vector<PrecisionInput> const& inputs, PrecisionParams const& p) {
    PrecisionResult r;
    for (int v = 0; v < kVariantCount; ++v) {
        bool n = v == 1 || v == 4 || v == 5 || v == 7;
        bool fa = v == 2 || v == 4 || v == 6 || v == 7;
        bool c = v == 3 || v == 5 || v == 6 || v == 7;
        double val = solvePrecision(inputs.data(), inputs.size(), p, n, fa, c);
        r.value[v] = val;
        r.ok[v] = std::isfinite(val);
    }
    return r;
}

}  // namespace fwm

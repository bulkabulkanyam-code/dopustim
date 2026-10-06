// Frame Window Mod - result export helpers (no Geode dependencies).
#pragma once
#include <string>
#include <vector>

#include "Precision.hpp"
#include "Types.hpp"

namespace fwm {

// Inputs usable for the precision model: not unstable; presses only unless includeReleases.
std::vector<PrecisionInput> toPrecisionInputs(std::vector<InputResult> const& results, bool includeReleases);

// Full results (readable, re-loadable by the mod).
std::string toResultsJson(Replay const& replay, Settings const& settings, std::vector<InputResult> const& results,
                          Tick endTick, bool partial);

// NaNDL calculator import format ("nandl-calculator" v1, time in frames).
std::string toNandlJson(std::vector<InputResult> const& results, double tps, double respawnSeconds,
                        bool includeReleases);

std::string toCsv(std::vector<InputResult> const& results);

// Binary ".fwc" (FWC2) understood by the Frame Window Counter mod.
std::vector<unsigned char> toFwc(std::vector<InputResult> const& results, double tps, bool includeReleases);

// Summary statistics over usable results (exact + capped lower bounds).
struct Stats {
    int count = 0;
    int capped = 0;
    int unstable = 0;
    double average = 0;
    double median = 0;
    int tightest = 0;               // smallest window
    int tightestNumber = 0;         // 1-based input number of the tightest input
    std::vector<int> histogram;     // histogram[w-1] = count of windows == w, last bin = ">= size"
};
Stats computeStats(std::vector<InputResult> const& results, int histogramBins = 10);

}  // namespace fwm

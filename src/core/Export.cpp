#include "Export.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fwm {

namespace {

std::string esc(std::string const& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

std::string num(double v) {
    char b[48];
    if (!std::isfinite(v)) return "null";
    std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}

std::string integer(long long v) { return std::to_string(v); }
const char* boolean(bool b) { return b ? "true" : "false"; }

void putU32(std::vector<unsigned char>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}
void putI32(std::vector<unsigned char>& o, std::int32_t v) { putU32(o, static_cast<std::uint32_t>(v)); }
void putF64(std::vector<unsigned char>& o, double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 8; ++i) o.push_back(static_cast<unsigned char>((bits >> (8 * i)) & 0xFF));
}

bool wanted(InputResult const& r, bool includeReleases) {
    return !r.unstable && (r.down || includeReleases);
}

}  // namespace

std::vector<PrecisionInput> toPrecisionInputs(std::vector<InputResult> const& results, bool includeReleases) {
    std::vector<PrecisionInput> out;
    out.reserve(results.size());
    for (auto const& r : results) {
        if (!wanted(r, includeReleases)) continue;
        out.push_back({static_cast<double>(r.tick), static_cast<double>(r.window())});
    }
    std::stable_sort(out.begin(), out.end(), [](PrecisionInput const& a, PrecisionInput const& b) { return a.tick < b.tick; });
    return out;
}

std::string toResultsJson(Replay const& replay, Settings const& s, std::vector<InputResult> const& results, Tick endTick,
                          bool partial) {
    std::string o;
    o += "{\n  \"format\": \"frame-window-mod\",\n  \"version\": 1,\n";
    o += "  \"replay\": {\"name\": \"" + esc(replay.name) + "\", \"tps\": " + num(replay.tps) +
         ", \"inputs\": " + integer(static_cast<long long>(replay.inputs.size())) +
         ", \"levelId\": " + integer(replay.levelId) + ", \"levelName\": \"" + esc(replay.levelName) +
         "\", \"bot\": \"" + esc(replay.bot) + "\"},\n";
    o += "  \"endTick\": " + integer(endTick) + ",\n";
    o += std::string("  \"partial\": ") + boolean(partial) + ",\n";
    o += "  \"settings\": {\"presses\": " + std::string(boolean(s.presses)) + ", \"releases\": " + boolean(s.releases) +
         ", \"keepHoldLength\": " + boolean(s.keepHoldLength) + ", \"fullCompletion\": " + boolean(s.fullCompletion) +
         ", \"horizonSeconds\": " + integer(s.horizonSeconds) + ", \"maxWindow\": " + integer(s.maxWindow) +
         ", \"repeats\": " + integer(s.repeats) + "},\n";
    o += "  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        auto const& r = results[i];
        o += "    {\"id\": " + integer(r.id) + ", \"number\": " + integer(r.id + 1) + ", \"tick\": " + integer(r.tick) +
             ", \"button\": " + integer(r.button) + ", \"player2\": " + boolean(r.player2) +
             ", \"down\": " + boolean(r.down) + ", \"early\": " + integer(r.early) + ", \"late\": " + integer(r.late) +
             ", \"window\": " + integer(r.window()) + ", \"earlyBound\": " + boolean(r.earlyBound) +
             ", \"lateBound\": " + boolean(r.lateBound) + ", \"capped\": " + boolean(r.capped) +
             ", \"unstable\": " + boolean(r.unstable) + ", \"trials\": " + integer(r.trials) + "}";
        o += (i + 1 < results.size()) ? ",\n" : "\n";
    }
    o += "  ]\n}\n";
    return o;
}

std::string toNandlJson(std::vector<InputResult> const& results, double tps, double respawnSeconds,
                        bool includeReleases) {
    std::string o;
    o += "{\n  \"format\": \"nandl-calculator\",\n  \"version\": 1,\n";
    o += "  \"settings\": {\"gameFps\": " + num(tps) + ", \"windowFps\": " + num(tps) +
         ", \"respawnSeconds\": " + num(respawnSeconds) + ", \"timeUnit\": \"frames\"},\n";
    o += "  \"frameWindows\": [\n";
    std::vector<InputResult const*> list;
    for (auto const& r : results)
        if (wanted(r, includeReleases)) list.push_back(&r);
    std::stable_sort(list.begin(), list.end(), [](InputResult const* a, InputResult const* b) { return a->tick < b->tick; });
    for (std::size_t i = 0; i < list.size(); ++i) {
        auto const& r = *list[i];
        o += "    {\"input\": " + integer(static_cast<long long>(i) + 1) + ", \"timePosition\": " + integer(r.tick) +
             ", \"frameWindow\": " + integer(r.window()) + ", \"isPlayer2\": " + boolean(r.player2) + "}";
        o += (i + 1 < list.size()) ? ",\n" : "\n";
    }
    o += "  ]\n}\n";
    return o;
}

std::string toCsv(std::vector<InputResult> const& results) {
    std::string o = "number,tick,type,player,early,late,window,capped,earlyBound,lateBound,unstable,trials\n";
    for (auto const& r : results) {
        o += integer(r.id + 1) + "," + integer(r.tick) + "," + (r.down ? "press" : "release") + "," +
             (r.player2 ? "2" : "1") + "," + integer(r.early) + "," + integer(r.late) + "," + integer(r.window()) + "," +
             boolean(r.capped) + "," + boolean(r.earlyBound) + "," + boolean(r.lateBound) + "," + boolean(r.unstable) +
             "," + integer(r.trials) + "\n";
    }
    return o;
}

std::vector<unsigned char> toFwc(std::vector<InputResult> const& results, double tps, bool includeReleases) {
    std::vector<InputResult const*> list;
    for (auto const& r : results)
        if (wanted(r, includeReleases)) list.push_back(&r);
    std::stable_sort(list.begin(), list.end(), [](InputResult const* a, InputResult const* b) { return a->tick < b->tick; });

    std::vector<unsigned char> o;
    o.push_back('F'); o.push_back('W'); o.push_back('C'); o.push_back('2');
    putF64(o, tps);
    putU32(o, static_cast<std::uint32_t>(list.size()));
    for (auto const* r : list) {
        putI32(o, static_cast<std::int32_t>(r->tick));
        putF64(o, static_cast<double>(r->window()));
        o.push_back(static_cast<unsigned char>(1 | (r->player2 ? 2 : 0)));  // bit0: draw, bit1: player 2
        putI32(o, 1);                                                      // inputs per frame
    }
    return o;
}

Stats computeStats(std::vector<InputResult> const& results, int bins) {
    Stats s;
    s.histogram.assign(std::max(1, bins), 0);
    std::vector<int> windows;
    int tightest = 1 << 30;
    for (auto const& r : results) {
        if (r.unstable) {
            ++s.unstable;
            continue;
        }
        int w = r.window();
        windows.push_back(w);
        if (r.capped) ++s.capped;
        int bin = std::min(w, static_cast<int>(s.histogram.size())) - 1;
        ++s.histogram[bin];
        if (w < tightest) {
            tightest = w;
            s.tightestNumber = r.id + 1;
        }
    }
    s.count = static_cast<int>(windows.size());
    if (!windows.empty()) {
        double sum = 0;
        for (int w : windows) sum += w;
        s.average = sum / windows.size();
        std::sort(windows.begin(), windows.end());
        std::size_t n = windows.size();
        s.median = (n % 2) ? windows[n / 2] : 0.5 * (windows[n / 2 - 1] + windows[n / 2]);
        s.tightest = tightest;
    }
    return s;
}

}  // namespace fwm

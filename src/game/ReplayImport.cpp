#include "ReplayImport.hpp"

#include <Geode/Geode.hpp>
#include <gdr/gdr.hpp>
#include <gdr_convert.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <stdexcept>

namespace fwm {

namespace {

struct FwmReplay : gdr::Replay<FwmReplay, gdr::Input<>> {
    FwmReplay() : gdr::Replay<FwmReplay, gdr::Input<>>("FrameWindowMod", 1) {}
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::uint8_t> readAll(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open the file");
    f.seekg(0, std::ios::end);
    auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size == 0) throw std::runtime_error("the file is empty");
    if (size > 256u * 1024u * 1024u) throw std::runtime_error("the file is too large");
    std::vector<std::uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!f) throw std::runtime_error("failed to read the file");
    return data;
}

}  // namespace

namespace {

template <class R>
std::shared_ptr<Replay const> convertReplay(R const& parsed, std::filesystem::path const& path) {
    auto out = std::make_shared<Replay>();
    out->name = path.filename().string();
    out->bot = parsed.botInfo.name;
    out->author = parsed.author;
    out->levelId = parsed.levelInfo.id;
    out->levelName = parsed.levelInfo.name;
    out->ldm = parsed.ldm;
    out->platformer = parsed.platformer;
    out->tps = parsed.framerate > 0 ? parsed.framerate : 240.0;

    if (out->platformer) throw std::runtime_error("platformer replays are not supported (classic levels only)");

    out->inputs.reserve(parsed.inputs.size());
    for (auto const& in : parsed.inputs) {
        Input i;
        i.tick = static_cast<Tick>(in.frame);
        i.button = in.button == 0 ? 1 : in.button;
        i.player2 = in.player2;
        i.down = in.down;
        out->inputs.push_back(i);
    }
    if (out->inputs.empty()) throw std::runtime_error("the replay contains no inputs");

    // Stable sort keeps the recorded order of same-tick inputs (e.g. release + press on one tick).
    std::stable_sort(out->inputs.begin(), out->inputs.end(),
                     [](Input const& a, Input const& b) { return a.tick < b.tick; });
    for (std::size_t k = 0; k < out->inputs.size(); ++k) out->inputs[k].id = static_cast<int>(k);
    return out;
}

}  // namespace

std::shared_ptr<Replay const> importReplay(std::filesystem::path const& path) {
    auto ext = lower(path.extension().string());
    auto data = readAll(path);

    if (ext == ".gdr2") {
        auto res = FwmReplay::importData(std::span<std::uint8_t>(data));
        if (res.isErr()) throw std::runtime_error("cannot parse .gdr2: " + res.unwrapErr());
        return convertReplay(res.unwrap(), path);
    }
    if (ext == ".gdr") {
        auto res = gdr::convert<FwmReplay, gdr::Input<>>(std::span<std::uint8_t const>(data));
        if (res.isErr()) throw std::runtime_error("cannot parse .gdr: " + res.unwrapErr());
        return convertReplay(res.unwrap(), path);
    }
    throw std::runtime_error("unsupported extension '" + ext + "' (expected .gdr or .gdr2)");
}

}  // namespace fwm

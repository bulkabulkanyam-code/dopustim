#include "ReplayImport.hpp"

#include <Geode/Geode.hpp>
#include <gdr/gdr.hpp>
#include <gdr_convert.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <span>
#include <stdexcept>

namespace fwm {

namespace fs = std::filesystem;

namespace {

struct FwmReplay : gdr::Replay<FwmReplay, gdr::Input<>> {
    FwmReplay() : gdr::Replay<FwmReplay, gdr::Input<>>("FrameWindowMod", 1) {}
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool endsWith(std::string const& s, std::string const& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::uint8_t> readAll(fs::path const& path) {
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

bool isReplayName(fs::path const& p) {
    auto name = lower(p.filename().string());
    return endsWith(name, ".gdr") || endsWith(name, ".gdr2") || endsWith(name, ".gdr.json");
}

template <class R>
std::shared_ptr<Replay const> convertReplay(R const& parsed, fs::path const& path) {
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

std::shared_ptr<Replay const> parseGdr2(std::vector<std::uint8_t>& data, fs::path const& path, std::string& err) {
    auto res = FwmReplay::importData(std::span<std::uint8_t>(data));
    if (res.isErr()) {
        err = res.unwrapErr();
        return nullptr;
    }
    return convertReplay(res.unwrap(), path);
}

std::shared_ptr<Replay const> parseGdr1(std::vector<std::uint8_t> const& data, fs::path const& path, std::string& err) {
    auto res = gdr::convert<FwmReplay, gdr::Input<>>(std::span<std::uint8_t const>(data));
    if (res.isErr()) {
        err = res.unwrapErr();
        return nullptr;
    }
    return convertReplay(res.unwrap(), path);
}

}  // namespace

std::shared_ptr<Replay const> importReplay(fs::path const& path) {
    if (!isReplayName(path)) {
        throw std::runtime_error(
            "this is not a GDR replay (.gdr / .gdr2). In the bot's save / export dialog pick the GDR format; "
            "native formats such as xdBot's .xd or Mega Hack's .mhr cannot be read.");
    }
    auto data = readAll(path);

    // GDR2 files start with the magic "GDR"; GDR1 is JSON or MessagePack. The extension is not trusted.
    bool looksGdr2 = data.size() >= 3 && data[0] == 'G' && data[1] == 'D' && data[2] == 'R';
    std::string first, second;
    if (looksGdr2) {
        if (auto r = parseGdr2(data, path, first)) return r;
        if (auto r = parseGdr1(data, path, second)) return r;
    } else {
        if (auto r = parseGdr1(data, path, first)) return r;
        if (auto r = parseGdr2(data, path, second)) return r;
    }
    throw std::runtime_error("cannot parse the replay: " + first);
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
namespace {

// Name of the bot a mod folder / mod id belongs to ("" if it is not a known bot).
std::string knownBot(std::string const& name) {
    auto l = lower(name);
    if (l.find("eclipse") != std::string::npos) return "Eclipse";
    if (l.find("xdbot") != std::string::npos) return "xdBot";
    if (l.find("megahack") != std::string::npos || l.find("mega hack") != std::string::npos ||
        l.find("mega-hack") != std::string::npos || l.find("hackpro") != std::string::npos)
        return "Mega Hack";
    return {};
}

bool isMacroFolderName(std::string const& name) {
    auto l = lower(name);
    return l == "macros" || l == "macro" || l == "replays" || l == "replay";
}

struct Scanner {
    std::vector<FoundReplay> found;
    std::set<std::string> seen;

    void scan(fs::path const& dir, std::string const& source, int depth) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (found.size() > 5000) return;
            std::error_code e2;
            auto const& p = it->path();
            if (it->is_directory(e2)) {
                if (depth > 0) scan(p, source, depth - 1);
                continue;
            }
            if (!it->is_regular_file(e2) || !isReplayName(p)) continue;
            auto key = p.lexically_normal().string();
            if (!seen.insert(key).second) continue;
            FoundReplay f;
            f.path = p;
            f.source = source;
            f.time = fs::last_write_time(p, e2);
            f.size = static_cast<std::uintmax_t>(fs::file_size(p, e2));
            found.push_back(std::move(f));
        }
    }

    // <root>/<each subfolder>: known bots are scanned completely, other mods only in their macro folders.
    void scanModFolders(fs::path const& root) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            auto id = it->path().filename().string();
            auto bot = knownBot(id);
            if (!bot.empty()) {
                scan(it->path(), bot, 3);
            } else {
                for (auto const& sub : fs::directory_iterator(it->path(), fs::directory_options::skip_permission_denied, e2)) {
                    std::error_code e3;
                    if (sub.is_directory(e3) && isMacroFolderName(sub.path().filename().string())) scan(sub.path(), id, 2);
                }
            }
        }
    }
};

}  // namespace

std::vector<FoundReplay> findBotReplays(std::size_t limit) {
    using namespace geode::prelude;
    Scanner sc;

    // Own folder
    std::error_code ec;
    auto imports = Mod::get()->getSaveDir() / "imports";
    fs::create_directories(imports, ec);
    sc.scan(imports, "imports", 1);

    // Geode config and save folders of the bots (Eclipse: eclipse.eclipse-menu, xdBot: zilko.xdBot, Mega Hack: ...)
    sc.scanModFolders(dirs::getModConfigDir());
    sc.scanModFolders(dirs::getModsSaveDir());

    // Mega Hack keeps its own folder inside the game directory.
    auto game = dirs::getGameDir();
    if (fs::is_directory(game, ec)) {
        for (fs::directory_iterator it(game, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            auto name = it->path().filename().string();
            if (knownBot(name) == "Mega Hack") sc.scan(it->path(), "Mega Hack", 3);
            else if (isMacroFolderName(name)) sc.scan(it->path(), "Game folder", 2);
        }
    }

    std::stable_sort(sc.found.begin(), sc.found.end(), [](FoundReplay const& a, FoundReplay const& b) { return a.time > b.time; });
    if (sc.found.size() > limit) sc.found.resize(limit);
    return std::move(sc.found);
}

std::string ageText(fs::file_time_type t) {
    using namespace std::chrono;
    auto age = fs::file_time_type::clock::now() - t;
    auto minutes = duration_cast<std::chrono::minutes>(age).count();
    if (minutes < 1) return "just now";
    if (minutes < 60) return std::to_string(minutes) + " min ago";
    auto hours = minutes / 60;
    if (hours < 48) return std::to_string(hours) + " h ago";
    return std::to_string(hours / 24) + " d ago";
}

ReplayInfo peekReplay(fs::path const& path) {
    ReplayInfo info;
    std::error_code ec;
    if (fs::file_size(path, ec) > 16u * 1024u * 1024u) return info;
    try {
        auto r = importReplay(path);
        info.ok = true;
        info.levelId = r->levelId;
        info.levelName = r->levelName;
        info.inputs = r->inputs.size();
        info.tps = r->tps;
    } catch (...) {
    }
    return info;
}

}  // namespace fwm

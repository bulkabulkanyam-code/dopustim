// Frame Window Mod - replay loading and discovery.
//   * .gdr2 / .gdr (GDR format 1 and 2). The format is detected from the CONTENT, not from the
//     extension: Eclipse writes GDR2 data, xdBot / Mega Hack may write either, and people rename files.
//   * discovery of macros saved by Eclipse, Mega Hack and xdBot (and other bots' macro folders).
#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../core/Types.hpp"

namespace fwm {

// Throws std::runtime_error with a readable message on failure.
std::shared_ptr<Replay const> importReplay(std::filesystem::path const& path);

struct FoundReplay {
    std::filesystem::path path;
    std::string source;  // "Eclipse", "xdBot", "Mega Hack", "imports", mod id of another bot, ...
    std::filesystem::file_time_type time;
    std::uintmax_t size = 0;
};

// All replay files found in the known macro folders, newest first (at most `limit`).
std::vector<FoundReplay> findBotReplays(std::size_t limit = 300);

// "5 min ago", "3 h ago", "2 d ago"
std::string ageText(std::filesystem::file_time_type t);

// Short facts about a replay file without keeping it (level id, input count). Empty on failure.
struct ReplayInfo {
    bool ok = false;
    std::uint32_t levelId = 0;
    std::string levelName;
    std::size_t inputs = 0;
    double tps = 240.0;
};
ReplayInfo peekReplay(std::filesystem::path const& path);

}  // namespace fwm

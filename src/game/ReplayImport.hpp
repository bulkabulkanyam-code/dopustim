// Frame Window Mod - loads .gdr2 (GDR format 2) and .gdr (GDR 1: JSON/MessagePack) replays.
#pragma once
#include <filesystem>
#include <memory>

#include "../core/Types.hpp"

namespace fwm {

// Throws std::runtime_error with a readable message on failure.
std::shared_ptr<Replay const> importReplay(std::filesystem::path const& path);

}  // namespace fwm

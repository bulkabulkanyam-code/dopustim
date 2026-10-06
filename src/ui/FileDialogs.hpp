// Frame Window Mod - replay file selection.
//   Desktop: native file dialog.
//   Android / iOS: in-game list of replays found in the mod's "imports" folder and in the
//   macro folders of other bots (geode/config/<mod-id>/{macros,replays}).
#pragma once
#include <filesystem>
#include <functional>

namespace fwm::ui {

void pickReplay(std::function<void(std::filesystem::path const&)> onPicked);

// Folder that holds replays on mobile (created on demand).
std::filesystem::path importsDir();

}  // namespace fwm::ui

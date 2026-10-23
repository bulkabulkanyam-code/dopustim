// Frame Window Mod - replay file selection.
//   pickReplay():   in-game list of the macros saved by Eclipse, Mega Hack and xdBot (newest first,
//                   replays of the open level on top). On desktop it also has a "Browse..." button.
//   browseReplay(): native file dialog (desktop only; does nothing on mobile).
#pragma once
#include <filesystem>
#include <functional>

namespace fwm::ui {

void pickReplay(std::function<void(std::filesystem::path const&)> onPicked);
void browseReplay(std::function<void(std::filesystem::path const&)> onPicked);

// Folder where replays can be dropped manually (created on demand).
std::filesystem::path importsDir();

}  // namespace fwm::ui

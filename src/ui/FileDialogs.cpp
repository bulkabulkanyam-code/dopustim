#include "FileDialogs.hpp"

#include <Geode/Geode.hpp>
#if !defined(GEODE_IS_MOBILE)
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#else
#include "ImportPopup.hpp"
#endif

using namespace geode::prelude;

namespace fwm::ui {

std::filesystem::path importsDir() {
    auto dir = Mod::get()->getSaveDir() / "imports";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void pickReplay(std::function<void(std::filesystem::path const&)> onPicked) {
#if defined(GEODE_IS_MOBILE)
    if (auto popup = ImportPopup::create(std::move(onPicked))) popup->show();
#else
    file::FilePickOptions options;
    options.filters.push_back({"GD replay (*.gdr, *.gdr2)", {"*.gdr", "*.gdr2"}});
    async::spawn(
        file::pick(file::PickMode::OpenFile, options),
        [cb = std::move(onPicked)](Result<std::optional<std::filesystem::path>> result) {
            if (!result.isOk()) return;
            auto picked = result.unwrap();
            if (!picked.has_value()) return;
            cb(*picked);
        });
#endif
}

}  // namespace fwm::ui

# Frame Window Mod (by Seby)

Geode mod for Geometry Dash (GD 2.2081, Geode 5.10.1; Windows + Android).
Measures the frame window of every input in a `.gdr` / `.gdr2` replay with the real game physics and
computes NaNDL precision (L\*). English first, short Russian summary at the end.

## Build
Needs the Geode SDK (`GEODE_SDK` env var) and the Geode CLI.

    geode build

Or push the project to GitHub: `.github/workflows/build.yml` builds Windows + Android32/64 and uploads the `.geode` files.

Core unit tests (no Geode needed):

    g++ -std=c++20 -O2 -Isrc tests/test_core.cpp src/core/Analyzer.cpp src/core/Precision.cpp src/core/Export.cpp -o test_core && ./test_core

## How it works
- `src/core/` - Geode-independent logic: `Analyzer` (search for the window of every input), `Precision` (NaNDL L\*), `Export`.
- `src/game/` - `GameRunner` runs one trial on the game's own physics (fixed 1/240 s steps, inputs injected in `processCommands`), `Session` ties everything together, `Hooks.cpp` has the Geode hooks.
- `src/ui/` - main window, precision window, file picker (native dialog on desktop, in-game list on Android).

Window of an input = number of consecutive ticks where, with only that input moved, the run still works
(survives the validation horizon, or completes the level in "Validate to completion" mode).

## Known limits
Classic levels, 240 TPS replays, normal mode from 0%, no noclip. Not tested in-game yet: compile it, run **Verify** on a replay you know completes the level; if Verify fails, try the other **Input phase** in the settings.

## По-русски
1. Откройте уровень в обычном режиме с 0%. 2. Пауза -> кнопка с часами. 3. Import -> выберите .gdr/.gdr2 (на Android положите файл в папку из кнопки "i" в списке). 4. Verify (проверка бота) -> Start. 5. Результаты в таблице, кнопка L\* Precision показывает 8 вариантов NaNDL, Export сохраняет .json/.fwc/.nandl.json/.csv.

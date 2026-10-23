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
- `src/game/` - `GameRunner` runs one trial on the game's own physics (fixed 1/240 s steps, inputs injected in `processCommands`; saved game states, per-tick trace and early pass), `Session` ties everything together (analysis, replay view with circles, auto import), `ReplayImport` (format detection, Eclipse / Mega Hack / xdBot folder discovery), `Hooks.cpp` has the Geode hooks.
- `src/ui/` - main window, precision window, in-game replay list (+ native dialog on desktop).
- `resources/fw_icon.png` - pause-menu button icon.

Window of an input = number of consecutive ticks where, with only that input moved, the run still works
(survives the validation horizon, or completes the level in "Validate to completion" mode).

## S&Q (Speed and Quality)
Top setting of the mod. **Fast & Medium** = v0.2 (saved states at quiet moments). **Slow & Accurate** = no saved states, long validation, gap scan ("holes"). **Ultra Fast & Inaccurate** = saved states at any tick incl. held buttons, no level restart per trial, short validation; a self-test picks how held buttons are restored (checkpoint / Practice Fix, or press again) and falls back to exact states if neither matches. Ultra is a fast estimate: no control runs.

## Known limits
Classic levels, 240 TPS replays, normal mode from 0%, no noclip. Run **Verify** on a replay you know completes the level; if Verify fails, try the other **Input phase** in the settings. v0.2 and v0.3 (S&Q, Ultra mode, sprite circles) were written without a Geode build environment: if the compile fails, send the error log.

Saved states: the first thing Start does is a self-test of the checkpoints. The status line / yellow-blue note in the main window tells whether they are used ("Saved states verified ...") or were switched off and why ("... using full replays"). Setting **Saved game states** off gives the exact v0.1 behaviour.

## По-русски
**S&Q** (самая верхняя настройка): Fast & Medium - как в 0.2; Slow & Accurate - без сейв стейтов, длинная проверка и поиск "дырок" за провалившимся сдвигом; Ultra Fast & Inaccurate - сейв стейты на любом тике (в том числе при зажатой кнопке), без перезапуска уровня между попытками, короткая проверка. Самотест выбирает, как возвращать зажатые кнопки (чекпоинт / Practice Fix или нажать заново), а если не совпадает - Ультра сама переходит на точные сейв стейты.

1. Откройте уровень в обычном режиме с 0%. 2. Пауза -> кнопка FW (цветные кружки над линейкой). 3. Import -> список макросов Eclipse / Mega Hack / xdBot (новые сверху) или кнопка Auto (самый свежий макрос этого уровня). 4. Verify (проверка бота) -> Start. 5. Результаты в таблице, кнопка L\* Precision показывает 8 вариантов NaNDL, Export сохраняет .json/.fwc/.nandl.json/.csv. Кнопка Watch проигрывает реплей с цветными кружками на кликах и пресидном по ходу реплея.

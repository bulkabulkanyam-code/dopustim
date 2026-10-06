# Frame Window Mod

Measures the **frame window** of every input of a GDR replay using the **real game physics**, and turns the windows into the **NaNDL precision (L\*)**.

## How to use
1. Open the level in **normal mode from 0%** (no noclip, no practice mode, no start pos, no other bot / TPS mods).
2. Pause the game and tap the clock button in the pause menu.
3. **Import** a successful replay (`.gdr` / `.gdr2`). On Android put it in the folder shown by the info button of the import list.
4. **Verify** (optional) checks that the replay completes the level, **Start** measures every input.
5. Results appear in the table. **L\* Precision** shows the 8 NaNDL variants (Base, Nerve, Fatigue, CPS and their combinations). **Export** writes `.json`, `.fwc`, `.nandl.json` and `.csv`.

## Definition
For each input only that input is moved 1, 2, 3... ticks earlier and later (all other inputs stay at their recorded ticks) and the level is re-simulated at 240 TPS. The window is the number of consecutive ticks that still work.

## Limits (v0.1)
Classic levels, 240 TPS replays, normal mode from 0%. Platformer and other TPS values are not supported yet.

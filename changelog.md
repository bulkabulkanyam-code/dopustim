# v0.3.0
**S&Q (Speed and Quality) - new main setting, first in the list**
- **Fast & Medium** (default): exactly the v0.2 behaviour - saved game states at quiet moments, early pass, control runs.
- **Slow & Accurate**: no saved states (every trial replays from 0%), no early pass, validation of at least 15 s after the moved input, and a gap scan: after the first failing shift on a side, 3 more shifts are tested to find isolated passing ticks (shown as "holes +N" in the table, `holes` in the exports; they do not change the window).
- **Ultra Fast & Inaccurate**: saved states are taken at ANY tick, also while a button is held (v0.2 only used moments with no button down, so spam / hold sections replayed everything from far back). The runner remembers which buttons are held in the state and brings them back after loading; the level is not restarted between trials; validation is 2 s at most and the early pass needs 40 ticks.
  - How the held buttons come back is chosen by a self-test on points inside holds: first "the game's checkpoint does it" (Practice Fix in Eclipse / Mega Hack), then "press the buttons again" (vanilla). If neither reproduces the original run, Ultra uses the exact saved states of Fast & Medium and says so in the status line. Practice Fix is used automatically when it is on but is not required.
  - Not exact by design: there are no control runs, and the short validation can miss deaths that happen later.

**Mobile**
- Replay circles are now sprites (`resources/fw_circle.png`) instead of `CCDrawNode` dots, which were drawn as squares on Android. Round markers with the window number, early/late and optionally the tick on every platform.

# v0.2.0
**Faster analysis**
- Saved game states: every trial starts from a game checkpoint taken shortly before the input instead of replaying the level from 0%. Checkpoints are taken only at quiet moments (no button held, no input nearby). A self-test and periodic control runs verify them; if the game does not restore states exactly, the mod switches back to full replays on its own and says why.
- Early pass: a trial ends as soon as the run is identical to the original again for half a second (cube sections finish in a fraction of the time).
- The search is explicitly "stop at the first failing shift per side": if the shifts -1 and +1 both fail, the window is 1 and the analysis moves to the next input.
- New settings: time per frame (default 30 ms, was fixed 12 ms), saved states, early pass, control run interval. ETA in the status line.

**Replay with frame windows ("Watch")**
- Plays the replay at normal speed with a coloured circle on every click (red = 1 frame, orange = 2, yellow = 3, light green = 4-5, green = 6+, blue = capped, grey = unstable) and a smaller outlined circle on releases. Each circle shows the window, early/late and optionally the tick.
- The precision (L* and N+F+C) is recomputed live while the replay plays.

**Fewer fake 1-frame windows**
- New "Hold handling" setting (default Auto): taps and spam (hold up to 3 ticks) now move together with their release; long holds still move only the press. Previously every tap was boxed in by its own release and the neighbouring taps and got window 1 without any physics being tested.
- A press may now share the tick of the release before it (order is preserved), so spam windows are no longer 1 tick too small.
- New "Dies after" column / export fields: how many ticks after the moved input the failing shift died. A window that is narrow only because of a death seconds later is easy to spot.

**Import**
- Auto button: loads the newest replay of the open level from the Eclipse, Mega Hack and xdBot folders.
- The import list (all platforms) shows the macros of those bots, newest first, replays of the open level on top; Browse... on desktop.
- The format is detected from the file content, not the extension (Eclipse writes GDR2 data, other bots GDR1/GDR2).

**Other**
- New pause-menu icon instead of the clock.
- Rewritten GitHub workflow (official build action, Windows + Android, tests no longer block the build, CMake 4 compatibility).
- Replaced setting `keep-hold` by `hold-mode`.

# v0.1.0
- First version: replay import (.gdr / .gdr2), frame window measurement on real physics, NaNDL L* (8 variants), export, Windows + Android.

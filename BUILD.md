# Build instructions

## Prerequisites

- Windows: Visual Studio 2026 with the C++ workload. A plain PowerShell is enough; no developer
  prompt is needed. WSL and containers are not needed, except for container builds.
- Linux: a C++23 compiler (GCC 12 or newer) and the libraries listed in
  [Containerfile](container/Containerfile), or only a container runtime (Podman or Docker) to
  build in that container.
- Both: [mise](https://mise.jdx.dev), which installs the pinned CMake, Ninja, nushell, Python (for
  the Switch runner only), clang-format and ast-grep from [mise.toml](mise.toml). Without mise,
  CMake 3.25 or newer is enough to build.

## Building and testing

The build tool is [x.nu](x.nu), a nushell script, and the same commands work on both systems:

```shell
nu x.nu build                       # configure and build the release preset
nu x.nu build --test                # and run the tests
nu x.nu build asan --test           # the same with AddressSanitizer
nu x.nu build release --container --test   # build and test in the Debian 13 container
nu x.nu run ambrosia                # run the game; arguments are passed on, e.g. 02_NYC_BatteryPark
nu x.nu run dented                  # run the editor
nu x.nu build --help                # every option
```

In a shell where mise is not activated, `mise run x build` and so on does the same.

`nu x.nu build <preset>` takes one of the presets in [CMakePresets.json](CMakePresets.json):
`release`, `debug`, `asan`, `tsan` (Linux), `mingw` (a Windows build cross-compiled on Linux, with
the toolchain in [Containerfile](container/Containerfile)) and `switch` (below). The build goes in
`build/<preset>`, with a suffix for each option that changes the build (`--lto`, `--no-lto`,
`--tracy`, `--mesa`), so `nu x.nu build --tracy` builds in `build/release-tracy` and leaves
`build/release` as it is. Arguments that start with `-D` go to the configure step, as
`nu x.nu build debug -DSKIP_UT=ON`; they stay in that folder's cache. `--clean` removes the folder
first. Windows uses the Visual Studio generator and MSVC; Linux uses the default compiler and
Ninja. Without the tool, the presets work directly:

```shell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

When neither `DEUSEX_GAME_DATA` nor `game-data` in the checkout is there, the tool configures with
`-DAMBROSIA_DATA_TESTS=OFF`, so the tests that need the data are left out.

This builds the game `ambrosia`, the editor `dented`, the compiler `ucc` and `unit-tests`.

`build/` holds only what a build makes, so deleting it gives a clean build. Data of your own that
should outlive that, such as the Switch runner's variants and results, goes in `scratch/`, which
git also ignores.

| Option | What |
| -- | -- |
| `-DAMBROSIA_DATA_TESTS=OFF` | Leave out the tests that need the game data, where there is none (`x.nu` sets it when it finds none). When on (the default), they fail saying the data is missing if they cannot find it. |
| `-DSKIP_UT=ON` | Don't build the unit tests |
| `-DAMBROSIA_STEAM_AUDIO=OFF` | Build without Steam Audio, which the optional HRTF sound mode needs. When on (the default on 64-bit Windows and Linux), configuring downloads its 180 MB release once into `.cache/` and copies `phonon.dll` or `libphonon.so` next to the programs, with its licences in `LICENSES.txt`. |
| `-DSTEAMAUDIO_ARCHIVE=<zip>` | Use a local copy of `steamaudio_4.8.1.zip` instead of downloading it |
| `-DAMBROSIA_TRACY=ON` | Build with the [Tracy](https://github.com/wolfpld/tracy) profiler's zones, for profiling (see below). Off by default; when on, configuring downloads the Tracy 0.15.0 client source (21 MB) once into `.cache/`. |
| `-DTRACY_ARCHIVE=<tar.gz>` | Use a local copy of GitHub's `v0.15.0.tar.gz` source archive of Tracy instead of downloading it |
| `-DAMBROSIA_LTO=ON` | Link-time optimisation (`--lto`). On by default for the Switch only: on the desktop it made frames about 1% shorter but each rebuild after an edit about 7 s longer. Not with a sanitizer. |
| `-DAMBROSIA_CCACHE=OFF` | Build without ccache, which is used when it is installed. A launcher given with `-DCMAKE_CXX_COMPILER_LAUNCHER=` is used instead of it. |
| `-DSANITIZER=asan` | AddressSanitizer; with GCC and Clang also UBSan. The `asan` preset sets it. |
| `-DSANITIZER=tsan` | ThreadSanitizer and UBSan; GCC and Clang only. The `tsan` preset sets it. |

## Running

```shell
build/release/bin/ambrosia
build/release/bin/Release/ambrosia.exe
```
The second is for Visual Studio, which puts each configuration in its own directory.
You can pass a path to a map to load like `02_NYC_BatteryPark`:

```shell
./ambrosia 02_NYC_BatteryPark
```

### Game data
No path is compiled in. The game, the editor, `ucc` and the tests all look at run time, in this
order, and take the first that holds `System/DeusEx.u`:
- the `DEUSEX_GAME_DATA` env var
- a `game-data` directory next to the executable
- a `game-data` directory in each folder above it, up to the root, so a build in
  `build/<preset>/bin` finds `game-data` in the repo root

That keeps a build directory working when the checkout is mounted at another path, as in a
container.

The simplest setup is to set the variable in `mise.local.toml` at the root of the checkout, which
git ignores. mise then sets it for every task, the tests find the data, and worktrees under
`.claude/worktrees` pick it up from the main checkout's file:

```toml
[env]
DEUSEX_GAME_DATA = "D:/games/DeusEx"
```

The variable is read when a program starts, so changing it needs no reconfigure.

### User data
Settings, saves and `ucc` packages go to `$XDG_DATA_HOME/ambrosia` when that is set (on every
platform, so tests can isolate their user data), and otherwise to `%LOCALAPPDATA%\Ambrosia` on
Windows and `~/.local/share/ambrosia` on Linux.

### Log
The game logs to `Ambrosia.log` and the editor to `DentEd.log` in the user data directory.

The lines also go to standard error when it is redirected to a file or pipe. On Windows the game
and the editor start like any GUI program and write to no console; `--console` has them write to
the terminal they were started from (which does not wait for them, so the prompt comes back
first), or to a console window of their own when started from Explorer. On Linux standard error
is always written.

cmd and PowerShell do not wait for a GUI program to end. To wait, redirect its output, which also
keeps the log for a script:

```shell
& .\build\release\bin\Release\ambrosia.exe 00_Training --hidden --frames 1500 2>&1 | Out-Host
```

A fatal error is shown in a message box too, unless standard error is redirected to a file or pipe,
so a script or test run never stops on one.

### Baked acoustics
The raytraced sound mode (`AmbrosiaRaytracedAudio`) bakes each level's reverb and sound paths
from the game's maps, so the data is made on this machine and kept in `AcousticCache` in the
user data folder, one `.dxbake` file per map. Entering a level with no valid file bakes it in the
background on two threads while the mode runs on its live simulation (about 10 s for the
largest levels), then switches over. A file made from other geometry, materials, settings or
Steam Audio version, or a damaged one, is reported (the log and the overlay's Acoustic scene
section) and baked again, never used. To bake maps ahead of play:

```shell
build/release/bin/ucc bakeacoustics 11_Paris_Cathedral 02_NYC_Bar
build/release/bin/ucc bakeacoustics all
build/release/bin/ucc bakeacoustics all --force
```

Maps whose file is valid are skipped unless `--force` is given. It runs below normal priority on
half the cores; every map takes about 3.5 minutes on a Ryzen 9 7950X, at most 2.4 GB of memory,
and about 700 MB on disk. In the audio section of the ini, `AmbrosiaBakeAcoustics`,
`AmbrosiaBakedReverb` and `AmbrosiaPathing` (all on unless set to `False`) turn off background
baking, the baked reverb (back to the live simulation) and pathing.

Background work (baking, and Steam Audio's own simulation threads) uses at most
`AmbrosiaBackgroundThreads` threads, under `[Engine.Engine]`: all but one of the machine's threads
by default, and 3 on the Switch. `ucc bakeacoustics` uses half of them.

## Command-line options for development

`--help` lists each program's options. A typo, an unknown option or a bad value stops the program
with exit code 2, and the log says what is wrong.

### The game

```shell
ambrosia [map] [options]
```

The map is one to start on, such as `00_Training`, instead of the game's own start.

| Option | What |
|---|---|
| **Automated runs** | |
|`--do "<steps>"`| Runs steps, separated by semicolons, after each frame from the first; see Steps. |
|`--script <file>`| Runs the steps in a file, one a line, with `#` comments. `--do` and `--script` can be given more than once, and run in the order given. |
|`--frames <N>`| Stops after `N` frames. |
|`--hidden`| No visible window, no sound and no hold on the mouse, so a run does not disturb whoever is using the desktop. Screenshots still work. |
|`--width <N>`, `--height <N>`| The window's size. |
|`--seed <N>`| Seeds every random stream with `N` instead of the clock, for comparing runs with the `worldhash` step. |
| **Level inspection** | |
|`--no-mission-script`| Each level loads without its mission script and its camera points (cutscene camera paths), so `00_Intro` and the `99_Endgame` maps play no cutscene and nothing leaves the level. The level is as placed in the editor, without the flags, story changes to its actors and level changes the script would make. |
|`--no-triggers`| Each level loads without its triggers (`Trigger` and its subclasses, such as `ConversationTrigger` and `DataLinkTrigger`), and no actor is given conversations, so no NPC starts one when you come in range, use them or pass by. Example: `ambrosia 11_Paris_Everett --no-mission-script --no-triggers --do "wait 20; cheats; wait 10; view Light170"` |
| **Diagnostics** | |
|`--debug-overlay`| Opens the developer overlay from the first frame (see Developer overlay). |
|`--null-sound`| Plays sound to a device that discards it, so the mixer runs on its own thread at the pace of a sound card but nothing is heard. With `--hidden`, sound is then mixed this way instead of not at all. For profiling the mixer. |
|`--clear-shader-cache`| Removes the GL driver's shader cache before the window is made, where the game can (the newer Mesa's `sdmc:/.mesa` on the Switch); elsewhere it says it cannot. |
|`--console`| On Windows, also writes the log to the terminal the game was started from, or to a console window of its own (see Log). |

A run with `--frames`, or with a `quit` step, is an automated run: each frame is 1/30 s of game
time, run as fast as it goes without vsync or the frame limit, and the window is the size the
command line gives (1280x720 without one) rather than the one saved. Steps without `quit` play in
real time, for setting up a level to look at by hand.

#### Steps

Steps run in order after each frame, starting after the first; each runs at once, until one
waits:

| Step | What |
|---|---|
|`wait <N>`| The next step runs `N` frames later. |
|`wait <N>s`| `N` seconds later, at 30 frames a second. |
|`wait loaded`| Waits until no level change is pending, so the next step sees the new level, such as `exec open 02_NYC_Bar; wait loaded; shot bar.bmp`. |
|`quit`| Ends the run, once a `shot` asked for just before it is saved. |

A script for a recording:

```text
# training.txt: ambrosia 00_Training --hidden --null-sound --script training.txt
wait 300
exec DebugOverlay perf-record-for 600f
wait 610
quit
```

The game's own steps (the editor has its own, below):

| Step | What |
|---|---|
|`exec <command>`| Runs a console command and logs what it prints, such as `exec DebugOverlay perf-record-for 20`. |
|`shot <file>.bmp`| Saves the next frame, such as `--do "wait 60; shot out.bmp; quit"`. |
|`press <key>`, `release <key>`, `type <text>`, `mouse <dx> <dy>`| Presses or lets go of a key or mouse button by its input name (`Escape`, `LeftMouse`, `1`), types text, or moves the mouse as a player would. |
|`pad <button>`, `paddown <button>`, `padup <button>`| `pad` presses a controller button (`a`, `b`, `lb`, `up`...) or back paddle (`r4`, `l4`, `r5`, `l5`) and lets go in the same frame; `paddown` and `padup` hold it from one frame to another. Both go through the remapping in `[Ambrosia.Gamepad]`, as a real press does. |
|`known <source>`| Treats a text or conversation as read, such as `known text:01_Datacube03`. |
|`frob <actor>`| Uses an actor by name, such as `frob Keypad0`; computers open directly, bypassing remote animation and range checks. |
|`menu <class>`| Opens a menu screen over the game, as its button would, such as `--do "wait 60; menu DeusEx.MenuScreenControls; wait 90; shot controls.bmp; quit"`. |
|`focus`| Logs the screen on top, its focused control, the controls controller focus can reach and the shown ones it cannot, and the panels the right stick scrolls, for checking a screen works with a controller. |
|`cheats`| Turns on the stock cheats, then `god`, `ghost` (fly through walls) and `invisible 1` (the AI ignores the player), for inspecting a level. |
|`mark <selector>`| Marks actors over the level, with their collision cylinder (hitbox) and a label; shows the overlay passive if it was hidden. The selector is an actor's name, a class, or `*`, then any `Property=value` filters, such as `mark ScriptedPawn` or `mark * LightType=LT_Flicker`. `unmark <selector>` or `unmark all` takes marks away; the console has the same as `DebugOverlay mark ...`, and **Game → Marks** in the overlay lists them. |
|`view <selector> [distance]`| Holds the player for 30 frames at a clear spot about `distance` units (300) from the first actor the selector matches, or from the point `<x> <y> <z>`, looking at it. Use after `cheats`, so the spot can be in mid-air. Example: `ambrosia 06_HongKong_WanChai_Garage --do "wait 20; cheats; mark * LightType=LT_Flicker; wait 10; view Light322"` |
|`trigger <selector>`| Triggers every actor the selector matches, the player as instigator, as an event with its Tag would: turns on a TriggerLight, opens a mover. Logs each one's Tag and the actors whose Event names it. Example: `trigger TriggerLight1` turns on the club strobe in `06_HongKong_WanChai_Underworld`. |
|`camera at <x> <y> <z> <pitch> <yaw>`| Holds the player at a place and view rotation for measuring a fixed view; also `camera spin`, `camera path`, `camera free` and `camera where` (see Test runs from the PC). |
|`memstats <label>`| Logs the memory counters, for the soak tests (`tests/soak`). |
|`worldhash`| Logs a hash of every live actor's script properties and state and of the random streams, for comparing runs or builds (with `--seed`). |

### The editor

```shell
dented [map] [options]
```

The map is a map's name or the path of a `.dx` file; `01_NYC_UNATCOIsland` without one. `--new
<name>` starts on a new, empty map of that name instead. `--do`, `--script`, `--frames`,
`--hidden`, `--width`, `--height` and `--console` are as the game's. Its own steps are `exec
<command>` (an editor command, as typed in the Log panel), `shot <file>.bmp`, `state` (logs the
selection, the actor count, the undo history and the status line), `expect <check>`, and mouse and
key input: `click <x> <y>`, `rclick`, `drag <x1> <y1> <x2> <y2>`, `rdrag`, `wheel <x> <y> <delta>`,
`key <name>`, `keydown`, `keyup` and `type <text>`, with `clickview <view> <x> <y>` and `dragview`
taking places in a view as fractions of it. A click takes three frames and a drag eleven, starting
on the frame after its step, so the step after one waits for it. The editor tests are scripts in
`tests/editor`; a failed `expect` makes the editor exit with 1.

### Developer overlay

`--debug-overlay`, or `DebugOverlay` in the console, opens the developer overlay (Dear ImGui): a
menu bar whose menus open one window per tool: Game debug (`gamedebug`: the original game's actor
display, tool windows, window statistics and player cheats, as the console commands a player would
type), Audio (`audio`: audio switches, the playing voices with solo and mute, reverb, mixing cost,
where the listener is), Acoustic scene (`acoustics`: the raytraced sound mode's scene), Memory
(`memory`: objects by class, the name table and the garbage collector's runs), Performance
(`performance`: the built-in frame recorder, see Profiling), Render (`render`: switches that leave
out render stages, and the renderer's counts) and Input (`input`: each pad's SDL name and type, its
prompt style, which pad is in play, its live buttons, axes and gyro, and the touch screen's
fingers). Which tools are open, where their windows are and the overlay's scale are kept in
`DebugOverlay.ini` in the user data folder, except in hidden runs. The overlay scales itself to the
display (its content scale, or the window's height over 1080 pixels on a 4K screen at 100%);
**Overlay → Scale** or `DebugOverlay scale <auto|1.5>` overrides that, and the editor has the same
choice under **View → UI Scale**. Explanations that would crowd a tool are behind its `(?)` markers
and tooltips. `DebugOverlay open <tool>` and `DebugOverlay close <tool>` open and close a tool by
name (`all` for every one), for scripted runs. It opens interactive: the mouse works the overlay,
and holding the right mouse button over the game looks around. `DebugOverlay` in the console
switches it between interactive and passive, where it stays drawn and live but the game has the
mouse and keys as in normal play; `DebugOverlay off` hides it (`DebugOverlay passive` and
`DebugOverlay interactive` set either directly). Bind keys with `set Input ScrollLock DebugOverlay`
and `set Input Backslash DebugOverlay off`. `DebugOverlay mesh on` draws the raytraced sound mode's
scene over the level, `DebugOverlay markers on` marks each positioned voice, and `DebugOverlay ray
<sound name or id>` draws the ray to a voice (`ray none` clears it), for scripted runs.

## Profiling

`--tracy` builds a preset with debug information and the Tracy profiler's client
(`AMBROSIA_TRACY`), in `build/<preset>-tracy`. Its programs record zones for each stage of the frame, the
world tick, rendering, the HUD scripts, map loading, the mixer and the raytraced sound mode's
threads, plus lock waits, allocations and per-frame totals (script, physics, AI, mesh building)
as plots, and stream them to a Tracy viewer or `tracy-capture` on the same machine. Without
`AMBROSIA_TRACY` none of Tracy is built; the zones still feed the built-in frame recorder below. The viewer must be Tracy 0.15.0, whose protocol the
client speaks.

```shell
nu x.nu build --tracy       # or: cmake --preset release -B build/release-tracy -DAMBROSIA_TRACY=ON
nu x.nu build --tracy --test
```

With the viewer, `tracy-profiler.exe`, start the game (`build/release-tracy/bin/Release/ambrosia.exe` on
Windows) and connect to it from the viewer's list. The client keeps everything from the start
until something connects, so connecting late still shows the map load.

To record a run without the viewer, start `tracy-capture` first and then the game; it connects
when the game starts and saves after the given seconds:

```shell
tracy-capture -o training.tracy -s 20
build/release-tracy/bin/Release/ambrosia.exe 00_Training --hidden --null-sound --frames 100000
```

`--frames` runs the frames as fast as they go, without vsync or the frame limit, each one
1/30 s of game time. Open the `.tracy` file in the viewer, or get zone statistics with
`tracy-csvexport training.tracy` (`-u -p` lists every zone and plot point). The client settings
for each platform are in [cmake/tracy.cmake](cmake/tracy.cmake); sampling, call stacks and system
tracing are off. Zones and plots are added with the macros in
[src/core/profile.h](src/core/profile.h) and, for GPU time, [src/render/gpu_profile.h](src/render/gpu_profile.h).

### The built-in frame recorder

Every build, the Switch's included, also has a frame recorder of its own that needs no Tracy and
no PC: the same zones, `DX_TIME` totals and `DX_PLOT` values, kept per frame for the last 600
frames and summed since recording started. It is off until the overlay's Performance tool
(`DebugOverlay open performance`) or a console command turns it on; off, each zone costs a
load and a branch on entry and exit. It measures CPU time only, so on a GPU-bound machine the
wait shows up in `Swap`. The Performance tool shows the frame time graph with its mean, worst and 1% low (the mean
of the slowest 1% of frames), the zones nested as they run, and the totals and plots, and writes
them as CSV. For a run with no overlay:

```shell
build/release/bin/Release/ambrosia.exe 00_Training --hidden --null-sound --do "wait 300; exec DebugOverlay perf-record-for 20; wait 1200; quit"
```

`DebugOverlay perf-record-for <N>` records N seconds (`<N>f` for N frames) from the next frame and
then writes `Profiles/FramesNNNN.csv` in the user data folder (`sdmc:/switch/ambrosia/user/` on the
Switch), saying where in the log; `perf-record-for 600f spawn`
writes `Profiles/spawn.csv` instead (the console lowercases the name). `DebugOverlay perf-record on|off`
records with or without the tool, `DebugOverlay perf-csv <name|default>` writes what has been
recorded so far, and `DebugOverlay perf-reset now` starts the totals again. The file has the
frame times, a table of zones (by path, such as `Frame/Render/Scene/Mirrors`, with thread, mean
and worst milliseconds per frame, share of the frame and calls per frame), the totals and plots,
then one row per kept frame with a column for each zone and value. The plots include the
renderer's counts: draw calls, triangles, visible surfaces and actors, texture and lightmap
uploads.

The overlay's Render tool (`DebugOverlay open render`) turns render stages off to see what each
costs: `DebugOverlay render-<stage> off` with stage `mirrors`, `sky`,
`lightmap-animation`, `procedural`, `meshes`, `translucent`, `occlusion`, `coronas`, `post` (with its parts `msaa`, `ao` and `fxaa`), `gamma` or `hud`, and `DebugOverlay render-reset
now` turns them all back on. `DebugOverlay mirror-markers on` outlines every mirror surface and
mirror face on a mover, with a label.
These switches are not saved, and with any of them off the picture is not what the game
normally draws.

`DebugOverlay gpu-timers on` adds the GPU's time per stage to the plots, from OpenGL timestamp
queries, in every build: `GPU scene` (the whole 3D scene), `GPU mirrors`, `GPU translucent` (every
view's translucent pass, mirrors' included), `GPU post chain`, `GPU HUD` (the HUD and console
scripts' drawing and the canvas flush), `GPU main view` (scene less mirrors and post chain), and
`GPU timer frames late`. Results are read after the swap from the frame three frames earlier, only
if the GPU has finished it (never waiting), so these columns trail the CPU's by three rows, and a
frame not yet finished keeps the previous figures and counts as late. Off, the timers cost a load
and a branch per stage.

## Nintendo Switch

The game builds as a homebrew program, `build/switch/ambrosia.nro` (with the runner for test runs
from a PC, `build/switch/ambrosia-runner.nro`, below), in a container with devkitPro's devkitA64 and
its SDL3 fork (see [Containerfile.switch](container/Containerfile.switch)):

```shell
nu x.nu build switch                     # --clean removes the folder first
```

It needs a container runtime, as the [container builds](#container-builds) do. The `switch` preset also works outside the
container with a devkitPro install that has `DEVKITPRO` set and the SDL3 fork in its Switch portlibs.

The Switch build is `-O3` with link-time optimisation (`AMBROSIA_LTO`, on by default there), which
made frames 6-8% shorter on the console with the same play
([docs/switch-performance/cpu-and-scripts.md](docs/switch-performance/cpu-and-scripts.md)); the
final link takes longer. `--no-lto` builds without it for comparison, in `build/switch-no-lto`.

The code that differs is in the `switch` subdirectories of `src/core` and `src/game`,
with the desktop builds' in the `desktop` ones beside them; a build compiles one set. The menu
options each platform has are a table in `src/game/menu_options.cc`.

On the SD card, with every name exactly as shown:

```text
sdmc:/switch/ambrosia/
    ambrosia.nro        the program
    game-data/          the contents of the Deus Ex folder, not the folder itself
        System/DeusEx.u     (so this file is here; also Maps, Textures, Sounds, Music and the rest)
    user/               settings and saves, made by the game
    log.txt             written by the game at each start
    args.txt            optional command-line options; leave it out normally
```

`args.txt` holds options separated by spaces, with quotes around one that has spaces. Test options
such as `--frames`, `--hidden` or a `quit` step make the game end by itself, so delete a leftover copy.

Start the game with the whole console's memory: hold R while starting any installed game, then pick
Ambrosia in the homebrew menu. Started from the album, homebrew has too little memory (the game uses
about 600 MB), and Ambrosia says so and returns to the menu.

`log.txt` records each start-up step (memory, game data, SDL, the window, the OpenGL version, audio,
the first frame) and everything the game prints, and a crash's address. Any error that stops the
game is also shown on screen. Started through nxlink (`nxlink -s ambrosia.nro`), the same log
streams to the PC. In emulators it also goes to the debug log (Ryujinx with the Guest log level).

Saves are the same files on every platform, so a save made on one loads on the others. Each is a
folder, `QuickSave` or `Save0001` and up, in the `Save` folder of the user data:

| Platform | Saves |
| -- | -- |
| Windows | `%LOCALAPPDATA%\Ambrosia\Save` |
| Linux | `~/.local/share/ambrosia/Save` |
| Switch | `sdmc:/switch/ambrosia/user/Save` |

Copy a whole save folder across, keeping its name, or giving it a `SaveNNNN` number not used
there yet. `Save/Current` is the game in progress and is rebuilt when a save is loaded.

Standard output and errors go to the debug log, which emulators show (Ryujinx with the Guest
log level). In Ryujinx the `shot` step saves an image that does not show the frame, and so does
Ryujinx's own screenshot key; capture the emulator's window instead, with a tool that asks the
window for its picture (Windows' `PrintWindow` with `PW_RENDERFULLCONTENT`), as copying the screen
gets a black frame from its Vulkan window. `Ryujinx.exe <path>/ambrosia.nro` starts the game
directly, so a run needs nothing more than `args.txt` and the log.

### Recording frame timings on the Switch

The built-in frame recorder (see Profiling) works on the console with no PC. Put one line like
this in `sdmc:/switch/ambrosia/args.txt`:

```text
00_Training --do "wait 300; exec DebugOverlay perf-record-for 600f; wait 800; quit"
```

and start Ambrosia as usual. It loads the map, runs it without vsync or the frame limit (each frame
1/30 s of game time, as in every automated run), lets 300 frames go by while the map
settles, records the next 600, writes them, and returns to the homebrew menu at its `quit`.
Any map name works, such as `01_NYC_UNATCOIsland`. The file is the next free
`sdmc:/switch/ambrosia/user/Profiles/FramesNNNN.csv`, and `log.txt` names it on the line that starts
`frame recording: wrote`. To see what one render stage costs, leave it out from the start with
a step before the first wait, for example `exec DebugOverlay render-mirrors off;` (the stages are
listed under Profiling). Delete `args.txt` afterwards, or the game keeps doing this.

To get the files off, turn the console off, put its SD card in a card reader and copy
`switch/ambrosia/user/Profiles`, or copy them over the network with an FTP server homebrew while
the console stays on. Each file starts with the frame time and the table of zones (mean and
worst milliseconds, share of the frame), as the Performance tool shows them.

### Test runs from the PC

The Switch build also makes `build/switch/ambrosia-runner.nro`, a small program that lets a PC on
the same network run Ambrosia builds on the console with nobody at it: send a build and its
options, run it, wait for it to exit, and fetch the CSV and the log.
[scripts/switch-runner.py](scripts/switch-runner.py) drives it (`nu x.nu runner <arguments>` runs
the same script). The console runs one game at a time, so the script runs one build at a time and
waits for it; to compare builds, run each in turn.

#### The runner

**It runs whatever build it is sent.** It takes requests only from private network addresses
(10.x, 172.16-31.x, 192.168.x, 169.254.x) that carry the shared token, and it writes only under
`sdmc:/switch/ambrosia/`, but anyone on the network with the token can run code on the console, and
the token crosses the network unencrypted. Leave it running only on a trusted network, and stop it
(+) when you are done.

How it works: the homebrew loader (nx-hbloader, which the homebrew menu runs in) lets a program
name the next one to start when it exits (libnx's `envSetNextLoad`), and starts the homebrew menu
when it names none. The runner names the build, with `--ambrosia-runner <runner path>` in front of
the options, and exits. The game, seeing that option (in `src/game/switch/platform.cc`), names the
runner in turn, reads its options from the command line instead of `args.txt`, skips the error
screen that waits for a button, and keeps the console awake. When the game exits, the loader
starts the runner again, which finds the job it wrote down and waits for the PC to fetch the
results. Game builds older than the runner do not know the option and stop on an error.

First time:

1. On the PC, make the token: `python scripts/switch-runner.py token` writes
   `~/.ambrosia/switch-runner-token.txt` (or keeps the one there).
2. Copy to the SD card:

   ```text
   sdmc:/switch/ambrosia-runner/ambrosia-runner.nro   from build/switch/
   sdmc:/switch/ambrosia/runner/token.txt             the token file, the same one line
   sdmc:/switch/ambrosia/game-data/                   as for playing
   ```

3. Start the runner as you would Ambrosia, with the whole console's memory: hold R while starting
   any game, then pick Ambrosia runner. It shows the console's address (`Address 10.10.0.2:28300`),
   `State waiting for a job`, and where it read the token; a "Jobs are refused" line says what is
   missing. It keeps the console from dimming or sleeping, so leave it plugged in. The address is
   also under System Settings, Internet, Connection Status.
4. On the PC (`--console`, or set `AMBROSIA_SWITCH` once):

   ```shell
   python scripts/switch-runner.py --console 10.10.0.2 status
   python scripts/switch-runner.py --console 10.10.0.2 run build/switch/ambrosia.nro '00_Training --do "wait 300; exec DebugOverlay perf-record-for 600f; wait 800; quit"' --out scratch/runs/first
   ```

   In PowerShell put single quotes around the options, as here; it does not take `\"`.

`run` uploads the build, which the runner keeps in `sdmc:/switch/ambrosia/runner/cache` by its
SHA-256 (the last 8), so a build is sent only once. It then waits for the game, and saves `log.txt`
and the CSV files new in `user/Profiles` in `--out`, with `console.txt`. That holds handheld or
docked and the CPU, GPU and memory clocks in MHz that the runner reads after the game, which
show whether sys-clk changed them. `--result <sdmc:/switch/ambrosia/...>` fetches other files
instead (a path ending in `/` means the files new in that folder). Options follow the homebrew
loader's rules: separated by spaces, double quotes around one with spaces, no backslash escapes
and no quotes inside an option.

For the same views in every run, [scripts/perf-spots.txt](scripts/perf-spots.txt) lists camera
spots per map (such as `00_Training spawn`, the hall with the polished floor and its seven mirror
views a frame), each the arguments of the game's `camera` step. `camera at <x> <y> <z> <pitch>
<yaw>` holds the player at that place and view rotation every frame from then on,
standing still: `camera spin ... <step>` turns by a yaw step each frame and `camera path <place and
rotation> <place and rotation> <frames>` moves from one to the other and back, both counted in
frames from the step, so every run sees the same views frame for frame. `camera free` lets go,
and `camera where` logs the player's place and view rotation, for finding new spots (as a step,
or in a hidden run with a screenshot). For example, to record 600 frames at the training
spawn after 300 to settle, with frees during play held back to the level change (`gcdefer on`) so
the recording carries none:

```text
00_Training --do "gcdefer on; wait 1; camera at -1149.2 825.8 -80.2 0 0; wait 300; exec DebugOverlay perf-record-for 600f spawn; wait 603; quit"
```

A game that hangs or crashes needs someone at the console. A crash closes the runner too (it ends
the whole program the loader runs in), and a hang leaves the game on screen. After `--timeout`
seconds (900 by default) the script stops with an error saying so. Close Ambrosia if it is still up
(HOME, then X), start the runner again, and `fetch --out <folder>` gets that run's log, which ends
at the crash, and the newest crash report. A fatal error the game reports itself (bad
options, missing data) is logged and the game exits back to the runner, and the script names
the error and fetches the newest crash report.

In Ryujinx the runner starts, shows the address and reads the token, and Ryujinx accepts
connections to it, but jobs cannot run there. Ryujinx starts an `.nro` itself, with no homebrew
loader to start the next program, so the runner refuses jobs (`envHasNextLoad is false`), and it
never delivers data sent to a program's listening socket.

### A newer Mesa on the Switch

devkitPro's Switch OpenGL driver is Mesa 20.1.0-rc3 from 2020. For comparing drivers, the game
also builds against Mesa 26.2.4 from [mesa-switch](https://github.com/danfromtico/mesa-switch), an
unofficial port with its own libnx backend, using its nvc0 OpenGL driver:

```sh
nu x.nu build switch --mesa              # build/switch-mesa/ambrosia.nro (and the runner)
```

This builds a second image, `ambrosia-switch-mesa` from
[Containerfile.switch-mesa](container/Containerfile.switch-mesa), on top of the normal one. It
removes devkitPro's `switch-mesa` package, fetches mesa-switch at the pinned commit
`0ff52079ca9e9e837ed27eb95a8d239f439a1864`, builds and installs it in the portlibs, and the game
is configured with `-DAMBROSIA_SWITCH_MESA=ON` to link it (SDL is unchanged). Its licence files end
up in the image's `portlibs/switch/share/licenses/mesa-switch`. The normal build and image are not
affected. On a 32-thread PC Mesa compiles in about a minute, the image is about 0.5 GB larger, and
`ambrosia.nro` grows by about 12 MB (from 10.7 to 22.9 MB). `log.txt` tells the two apart: the
OpenGL line ends `Mesa 26.2.4` instead of `Mesa 20.1.0-rc3`. The newer driver writes a shader
cache to `sdmc:/.mesa`, so its first run on a console compiles shaders that later runs load.

### Tracy on the Switch

A second program, `build/switch-tracy/ambrosia.nro`, has the Tracy client in it:

```shell
nu x.nu build switch --tracy
```

Copy it to `sdmc:/switch/ambrosia/ambrosia.nro` in place of the normal one. It plays as the normal
one does, and listens for the viewer on TCP port 8086 of every network the console is on; the
viewer connects to the console, not the other way round. It keeps nothing until a viewer
connects, so a capture starts at the moment of connecting and the map load is not in it unless
the viewer is already waiting. Over wifi:

1. Put the console and the PC on the same network, and find the console's address under System
   Settings, Internet, Connection Status (IP Address).
2. Start Ambrosia and load a map, by hand or with a map name in `args.txt` (with `--frames`
   or a `quit` step, as above, for a run with no frame limit).
3. On the PC, either open `tracy-profiler` 0.15.0 and connect to the console's address (the
   client also announces itself with a broadcast on UDP port 8086, which may put it in the
   viewer's list, though none reached the PC from Ryujinx), or record without the viewer:

   ```shell
   tracy-capture -a <console address> -o training.tracy -s 20
   ```

   then `tracy-csvexport training.tracy` for the zone statistics.

Connecting needs nothing let in through the PC's firewall, as the PC makes the connection; the
broadcast does, as it comes in on UDP 8086 (Windows asks the first time the viewer runs; allow
it on private networks). A capture sends about 1 MB a second, which
wifi carries, and while connected the game runs slower than without: in Ryujinx about 4 ms more
per frame of the training level's 25 ms. Not connected, the Tracy build runs as fast as the
normal one. The client's settings for the Switch are in [cmake/tracy.cmake](cmake/tracy.cmake),
and what it needs from libnx in `src/core/switch/tracy`.

In Ryujinx, turn on Enable guest Internet access (Options, Settings, Network), without which the
client cannot listen, and connect to the PC's own network address, not 127.0.0.1: Ryujinx opens
the program's listening socket on the PC's network interface.

## UnrealScript compiler

`ucc` exports and compiles UnrealScript, like the UCC that shipped with the
original Deus Ex SDK. Sources and compiled packages live in the user data
directory, and packages in its `System` directory are loaded in place of the
shipped ones with the same name.

To export the sources of a package:

```shell
build/release/bin/ucc batchexport DeusEx.u class uc ~/.local/share/ambrosia/DeusEx/Classes
```

Compile:

```shell
build/release/bin/ucc make
```

## Tests

`nu x.nu build --test` and `nu x.nu package` run the mise-pinned `actionlint`
on the host before building. To check just the GitHub Actions workflows, run
`mise run lint-workflows`. Running CTest directly checks the engine tests only.

```shell
nu x.nu build --test
ctest --preset release
```

ctest runs each test file as its own entry (`unit-<file>`), the hidden
`[editor-scripts]` tests and the scripted editor tests. Most tests need the
game data and are tagged `[data]`; when it is not found (see Game data above)
each of them fails with `game data not found` and the places looked in.
`-DAMBROSIA_DATA_TESTS=OFF` registers only the others. To run `unit-tests` directly:

```shell
build/release/bin/unit-tests "~[data]"
build/release/bin/unit-tests "-#" "[#core_test]"
```

## Container builds

`--container` builds a preset in the Debian 13 image from [Containerfile](container/Containerfile),
which has every library the build needs, for a Linux host without them (such as an immutable
distribution) or to check a clean Debian build: `nu x.nu build release --container --clean --test`.
The build goes in `build/container-<preset>`, apart from the builds on the host (on Windows, apart
from the MSVC builds). `AMBROSIA_CONTAINER=1` in `mise.local.toml` makes the container the default,
and `--no-container` turns it off for one build; `nu x.nu run` then looks in the container
folders too. MinGW builds there by itself when its cross compiler is not installed.

The runtime is the first that answers of Docker, `wslc` (the container CLI that comes with WSL) and
Podman on Windows, and of Podman and Docker elsewhere; `CONTAINER_RUNTIME=podman` or
`--runtime podman` picks one. The image is built on first
use and later runs reuse its cached layers. On Linux the checkout is mounted at its own path and
the build runs as your user, with SELinux labelling off so it can read your home folder, so the
programs run on the host; on Windows it is mounted at `/src`. Compiler output is cached in the
`ambrosia-ccache` volume (`podman volume rm ambrosia-ccache` empties it). `DEUSEX_GAME_DATA` is passed
on and its folder mounted read-only. With Podman and Docker the container may use three quarters
of the host's memory and no swap, so a run that outgrows it is stopped rather than the desktop
(wslc has no such limit; WSL's own is `memory=` in `.wslconfig`); with Docker Desktop and Podman
machines that is the less of this machine's memory and their virtual machine's. The asan and tsan
tests run fewer at once than there are CPU threads, by the memory each needs (about 2 GB and
5 GB), on the host too; `--jobs N` sets the count. Run one sanitizer build at a time.

The tsan tests run under `setarch -R`, with address randomisation off: TSan cannot place its
shadow memory on a kernel with 32 bits of mmap randomisation, such as WSL's, and every test stops
at once with "unexpected memory mapping". In a container that takes `personality()`, which the
runtimes' seccomp profiles refuse, so Podman and Docker run the tsan tests with seccomp off for
that container. wslc cannot, so `--test` with tsan uses Podman or Docker (`--runtime docker`), and
says so if wslc is asked for.

## Code style

The C++ sources follow `.clang-format`, which needs clang-format 22 or newer.
`mise install` installs the version the project pins. Formatting is not part
of the build; run it yourself:

```shell
nu x.nu format            # rewrite the files
nu x.nu format --check    # fail if any file would change
```

It uses `$CLANG_FORMAT` when set, such as Visual Studio's, and clang-format on the
PATH otherwise.

## Desktop releases

On a native x64 Windows (Visual Studio 2026) or Linux build host:

```sh
nu x.nu package --version 0.1.0-rc.1
```

This uses `build/package`, separate from the normal `build/release` tree. It builds
with the shipped game-data search paths, runs CTest with game-data tests excluded,
then unpacks and verifies the distribution in a temporary directory. The version
comes from the argument and the revision defaults to `git rev-parse HEAD`. For a
release candidate, use a clean checkout. Without `--version` the package is a dev build: the
programs say `Ambrosia dev (<commit>)` and the archives are named
`ambrosia-dev-<short commit>-…`. Ordinary builds say `Ambrosia dev` and never ask Git for a
version; only one generated source file contains the stamp.

On Linux, `--container` builds and tests in the Debian 13 image from
[Containerfile](container/Containerfile) instead, in `build/container-package`, and
verifies the archive on the host. This is how CI builds the Linux release, so its
glibc (2.41) is the oldest the release runs on. The host's glibc must be at least that new to run
the check. `--runtime` picks Docker or Podman as for `build`.

`build/package/artifacts` (or `build/container-package/artifacts`) holds the game, DentEd, `ucc`, Steam Audio runtime and
licenses in a Windows ZIP or Linux tarball. Each package run empties the folder first.
Windows also produces a separate symbols ZIP. Crash dumps require the PDBs from the matching
build. Game data isn't included. To recheck a downloaded archive, run, for example:

```sh
nu x.nu verify-package ambrosia-0.1.0-rc.1-windows-x64.zip --version 0.1.0-rc.1 --commit <full-commit-sha>
```

All three programs support `--version` without game data. For a gameplay smoke
test, extract the archive outside the checkout, point `DEUSEX_GAME_DATA` at a
Deus Ex installation and start the extracted game. Open **Settings** to see the version
at the bottom right. The startup line in `Ambrosia.log` includes the commit too.

### GitHub Actions

**Package desktop builds** runs on every push to `main`, on every pull request (a newer push to
the pull request cancels its running build), on pushes of `v*` tags, and manually. A tag
`vX.Y.Z` or `vX.Y.Z-suffix` builds version `X.Y.Z[-suffix]`. A tag of any other form fails
the build. Everything else is a dev build. The run's **Artifacts** section lists each archive
as its own artifact, uploaded as it is rather than zipped again: the Linux `.tar.gz`, the
Windows `.zip` and the Windows symbols `.zip`. Actions keeps a SHA-256 digest of each. These
builds do not create GitHub Releases.
The jobs use `jdx/mise-action`
for the tools pinned in `mise.toml`. Windows builds with the runner's Visual Studio. Linux runs
`package --container` on an Ubuntu 26.04 runner, building in the same Debian 13 image as local
container builds and verifying the archive on the runner. The Linux release needs glibc 2.41 or
newer (Debian 13, Ubuntu 25.04 and later).

To publish a release:

1. Tag a commit on the default branch with the version prefixed by `v` (for example,
   `v0.1.0-rc.1`) and push the tag. That push packages the version. The tag alone does
   not publish a release.
2. Download the Linux and Windows archives from that run's artifacts and test them.
3. Run **Publish verified desktop builds** on the default branch with the tag. The
   workflow checks that the tag is on the default branch, finds the tag's successful
   package build at the commit it points to, and downloads its three archives by their
   versioned names, failing if any digest does not match. It then publishes them without
   rebuilding, each under a label instead of its file name: "Ambrosia X.Y.Z for Windows
   (x64)", "Ambrosia X.Y.Z for Linux (x64)", and "Windows debug symbols (for crash
   reports, not needed to play)". Versions with a suffix are marked as prereleases. If you
   move the tag, wait for its new build before publishing.

## Generated files

The property accessors in `src/engine/props_consys`, `props_deusex`, `props_engine`,
`props_extension` and `props_fire` (`.h` and `.cc`) are written by `ucc genprops` from
`src/engine/props_bindings.txt`. Each starts with a `Generated by` line and is marked
`linguist-generated` in `.gitattributes`. To change one, edit the list and run:

```shell
build/release/bin/ucc genprops src/engine/props_bindings.txt src/engine
```

A test regenerates them and fails if the checked-in files differ. Everything else under `src/`,
`tests/` and `scripts/` is written by hand; `vendor/` holds third-party code as released.

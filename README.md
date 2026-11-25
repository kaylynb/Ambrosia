# Ambrosia

<img src="assets/icon/ambrosia/64.png" alt="">

An open-source reimplementation of the Deus Ex engine and UnrealEd. It runs
the original game's UnrealScript bytecode from the shipped `.u` packages on a
new C++ engine built on SDL3 and OpenGL 3.3.

Ambrosia is a fork of [OpenDE](https://github.com/jarmitage99/OpenDE) by
jarmitage99.

The original game data is required and is not included.

[BUILD.md](BUILD.md) describes building and testing on Windows and Linux.

## Features

- Borderless fullscreen.
- Widescreen support with Hor+ field of view.
- [Extensive gamepad support](#gamepads).
- 3D headphone audio (HRTF) on 64-bit Windows and Linux.
- Many effects are no longer tied to framerate (texture animations, flickering lights, electrical arcs, etc.).
- [Automatic login and keypad entry](#computers-and-keypads).
- [A bunch of fixes](#other-fixes).

## Game Data

Game data is searched for in this order:
- `DEUSEX_GAME_DATA` environment variable.
- `game-data` directory next to executable.
- Windows: `%LOCALAPPDATA%\Ambrosia\game-data`, then `%PROGRAMDATA%\Ambrosia\game-data`
- Linux: `$XDG_DATA_HOME/ambrosia/game-data` (`~/.local/share/ambrosia/game-data` when
  unset), then `ambrosia/game-data` in each folder of `$XDG_DATA_DIRS`.
  (`/usr/local/share` and `/usr/share` when unset).
- Deus Ex in each Steam library (`steamapps/common/Deus Ex`).

The game data directory is never modified by Ambrosia.

### Deus Ex Versions & Mods
I have only tested on the GOTY Steam release.
Only partial and spot checks; not an entire playthrough.

I've also tested with:

- [DXM's Deus Ex Maps Patch](https://www.dxm.be/navigator.php?lang=en&content=202)
- [Confix/LDDP](https://www.moddb.com/mods/confix)


## Settings and saves

Controls, settings and saves live in `$XDG_DATA_HOME/ambrosia` when that is set, and otherwise in
`%LOCALAPPDATA%\Ambrosia` on Windows and `~/.local/share/ambrosia` on Linux.

## Gamepads

Gamepad options are in **Settings -> Gamepad**:
- Button prompts (Xbox, PlayStation, Switch). Defaults to auto detect.
- Auto pause on disconnect.
- Aim Assist.
- Remapping (Only via config file for now) with in-game layout UI.
- Quickslots mapped to d-pad.
- Radial menus to select items and augs, change ammo, and configure quickslots.
- Full inventory support (can move items, apply upgrades, etc. and autosort).
- Gyro controls: player or local gyro space & flick stick option.

## Computers and keypads

On a login screen, **Autologin** opens a window with known login info. Keypads
have a button to enter a code you already know. The coverage for mapping
conversations/datacubes/etc to known logins is currently only partially mapped
out and has not been hand-verified for each one.

## Enhancement Goals

The goal is to *not* do significant graphical or gameplay changes, but to
create a stable modern version of Deus Ex with full gamepad support and
optional enhancements where they make sense.

For example, the automatic login and keypad entry is done to support
playing with a controller and reduce the need to use a keyboard. The HRTF audio
enhances the audio and improves directionality/awareness without changing the
style of the original mixer.

Bug fixes are mostly around stability and comfort. Bugs aren't kept just
because they are used in speedruns, but many still do work (super jump, grenade
climbing).

Some fixes require deviation from the original in ways that possibly change the
"artistic intent". For example: texture animation speed, and the rate at which
lights flicker.

## Other Fixes

- Work towards eliminating memory leaks. Both from upstream OpenDE and Deus Ex.
- A bunch of stability fixes and work on safer resource lifetime management.
- Character controller fixes: swimming and climbing ladders should behave
  identically to Deus Ex.
- Upgrade to SDL3, mostly to allow modern controller support.

### Experimental Stuff
- Switch build. Very low performance. Very wip. To be playable at 60fps will
  need some challenging multithreading changes in the engine.
- Raytraced audio. Needs a lot of work. Deus Ex actually carries material type
  with each of its surfaces so mapping to Steam Audio materials is actually
  pretty straightforward. Unlike HRTF it does *significantly* change the
  artistic direction of the audio. Most notably makes cameras much harder to
  hear. Still has quite a few issues I haven't had the time to work on.

## DentEd
<img src="assets/icon/dented/64.png" alt=""> 

DentEd is Ambrosia's level editor. It opens a game map by name or a `.dx` file
and edits it with the engine's own renderer.

## License

Ambrosia is licensed under the [GNU Affero General Public License](LICENSE).

Original code from fork base is licensed under the original [MIT License](#LICENSE-MIT)

## Credits

The controller button prompts are Input Prompts (version 1.5) by
[Kenney](https://www.kenney.nl), released under
[CC0](http://creativecommons.org/publicdomain/zero/1.0/): the Xbox Series,
PlayStation Series and Nintendo Switch sets, with the Steam Deck L4/L5/R4/R5
glyphs for the back paddles. See `assets/gamepad/prompts/LICENSE-kenney.txt`.

The editor uses the fonts embedded in Dear ImGui:
[ProggyClean](https://github.com/bluescan/proggyfonts) by Tristan Grimmer and
[ProggyForever](https://github.com/ocornut/proggyforever) by Disco Hello and
Tristan Grimmer, both under the MIT License. The game's own text uses the
fonts in the Deus Ex data.

Vendored libraries, each under its own license in `vendor/`:
[SDL](https://www.libsdl.org) (zlib),
[Dear ImGui](https://github.com/ocornut/imgui) (MIT),
[ImPlot](https://github.com/epezent/implot) (MIT),
[ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) (MIT),
[stb_image](https://github.com/nothings/stb) (MIT or public domain),
a subset of [Tabler Icons](https://tabler.io/icons) (MIT),
[miniaudio](https://miniaud.io) (public domain or MIT-0),
[glad](https://github.com/Dav1dde/glad) (generated loader, CC0 or WTFPL, with
Khronos headers under Apache 2.0) and
[Catch2](https://github.com/catchorg/Catch2) (Boost Software License, tests only).
[Steam Audio](https://valvesoftware.github.io/steam-audio/) (Apache 2.0) is
downloaded at configure time for the HRTF mode.

## Thanks (from upstream OpenDE)

This project would not be possible without the incredible work from these
legends:

- gildor
- dpJudas
- jmarshall
- AnthraX
- Marco888

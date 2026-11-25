# Memory soak harness

Measures how much memory Ambrosia keeps as it runs. Two parts:

- `tests/unit/soak_test.cc`: Catch2 cases tagged `[.soak]`, hidden from the normal
  `unit-tests` run and from CTest. They drive `dx::World` directly (no renderer, UI or
  audio output) and read the process's private bytes from inside
  (`GetProcessMemoryInfo` PrivateUsage on Windows, RssAnon on Linux; `soak_memory.cc`),
  along with the engine's own counts: `objects::all().size()`, live and destroyed objects,
  bytes of object property data (`cls->size`), live actors, `objects::destroyCount()`,
  package images held by loaded and retired linkers (`objects::linkerStats`), the decoded-sound
  cache (`audio::cacheStats`) and live heap bytes (`HeapSummary`, Windows only).
- `tests/soak/exe-soak.ps1`: runs the real `ambrosia` hidden (GL context, offline audio
  mixing) driven by a script of steps and samples its private bytes from outside every 100 ms.
  At known frames a `memstats <label>` action prints the exe's counters on stderr (objects,
  destroyed objects and windows, linker images, decoded sounds, renderer cache entries, dead
  entries and bytes from `SceneRenderer::cacheStats`); the runner reads private bytes the
  moment each line arrives.
- `tests/soak/analyze.ps1`: turns a results folder into per-step figures split by source.

`tests/soak/run-all.ps1` runs everything, each scenario in a fresh process, with repeats.

## Running

Build `unit-tests` and `ambrosia` (see `BUILD.md`), then:

    pwsh tests/soak/run-all.ps1 -Bin build/release/bin/Release -OutDir soak-results
    pwsh tests/soak/analyze.ps1 -Results soak-results

or one scenario at a time:

    build/release/bin/Release/unit-tests "[soak-travel]"
    pwsh tests/soak/exe-soak.ps1 -Exe build/release/bin/Release/ambrosia.exe -Scenario travel -Iterations 10

The exe runner points `XDG_DATA_HOME` at its output folder, so its ini files and saves
never touch the user's own.

## Engine scenarios (`unit-tests "<tag>"`)

| tag | what | knobs |
| -- | -- | -- |
| `[soak-travel]` | UNATCO Island <-> UNATCO HQ, as `openMap` travels (carries the player's items, stores the departing level, revisits load the stored level); 10 s of ticks on each level | `SOAK_ITERATIONS` (12), `SOAK_STAY_SECONDS` (10) |
| `[soak-travel-small]` | 02_NYC_Street <-> 02_NYC_Bar, same loop | same |
| `[soak-save]` | quicksave x N, quickload x N (as `open ?loadgame=-1`), then 8 slots written and listed 10 x N times with `readSaveInfo` as the save and load menus do | `SOAK_ITERATIONS` (15), `SOAK_SLOTS` (8) |
| `[soak-idle]` | one level, no input, AI running, a sample each game minute | `SOAK_MAP` (06_HongKong_WanChai_Market), `SOAK_MINUTES` (10) |
| `[soak-fire]` | gives the loadout, holds the assault rifle's trigger into the floor (ammo and health topped up every tick) | `SOAK_MAP` (01_NYC_UNATCOIsland), `SOAK_MINUTES` (5) |
| `[soak-churn]` | spawns 12 transient classes (shell casing, bullet hole, rock chip, smoke trail, spark, tracer, fragments, blood, dart, fireball, explosion light) in turn and destroys each after 0.5 s | `SOAK_RATE` per second (60), `SOAK_MINUTES` (5) |
| `[soak-sounds]` | decodes every sound in a list of packages through `audio::decode` and reports the decoded-cache size | `SOAK_SOUND_PACKAGES` (comma list) |

Output lines:

- `SOAK,scenario,iteration,label,game_s,private_mib,table,objects,destroyed,data_mib,destroyed_data_mib,actors,destroy_count,linker_mib,retired,retired_mib,sound_mib,sounds,heap_mib`, one per sample
- `SUMMARY,scenario,quantity,mean=,sd=,min=,max=,n=,unit` for per-step deltas; the first
  step of each kind is left out as warm-up (first visit of a level, first load of a save)
- `CLASSES,scenario,class,count=,destroyed=,data_kib=`: objects created after warm-up, by class
- `TICK,scenario,minute,ms`: mean real milliseconds per `World::tick` over each game minute

## Exe scenarios (`exe-soak.ps1 -Scenario`)

| scenario | what |
| -- | -- |
| `travel` | `exec open` between `-MapA` and `-MapB`, `-StayFrames` (300) frames on each |
| `idle` | `-Map` for `-Minutes`, a marker per game minute (1800 frames) |
| `fire` | as idle, after `exec loadout`, belt slot 1 (assault rifle), looking down, holding the left mouse button; ammo refilled with `exec set DeusEx.Ammo762mm AmmoAmount 150` every 600 frames |
| `save` | `exec QuickSave` x N, then `exec open ?loadgame=-1` x N |
| `menus` | N rounds of opening and closing (Escape) the inventory, goals, conversations, images, skills, augmentations and health screens |
| `talk` | UNATCO Island: `frob PaulDenton0` and let the conversation play, a marker every 15 game seconds |

Each run's folder has `samples.csv` (every 100 ms), `marks.csv` (private MiB and every
counter at each marker) and the process's stdout and stderr. In `--frames` mode every frame is 1/30 s
of game time, so frames give game time regardless of how fast the machine renders.

## Caveats

- Private bytes include heap that was freed but not returned to the system, so a single
  step can read 0 when it reuses free heap and the next one jumps; use the means.
- Engine runs leave out the renderer's caches, the UI and audio decoding; the exe runs
  include them. The exe runs mix audio offline (`--hidden`), so sounds are decoded and
  cached as in the game.

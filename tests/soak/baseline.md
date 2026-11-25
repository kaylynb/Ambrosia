# Memory growth baseline, 2026-11-14

Measured on Windows 11 with an MSVC Release build of `d32e0e6`, plus the read-only
counters commit (`objects::linkerStats`, `audio::cacheStats`, `SceneRenderer::cacheStats`,
the `memstats` action), and Steam game data. Raw output is in `baseline-2026-11-14.txt`
(SUMMARY lines) and `baseline-2026-11-14-analysis.txt` (`analyze.ps1` output). How to repeat
it is in `README.md`.

Column meanings: "Private" is process private bytes. "Heap" is live bytes on the process
heap (`HeapSummary`), which excludes free heap kept for reuse. "Images" is the package file
images held by retired linkers. "Object data" is the property data (`cls->size`) of objects
in `objects::all()`. "Other C++" is Heap − Images − Object data: object headers, names,
arrays, strings, BSP and model data, collision and navigation structures, state frames.
MiB = 2^20 bytes.

Engine runs were repeated 3 times and exe runs twice, each in a fresh process. Engine runs
agree to within 0.1 MiB, so the spread given is between steps within a run. The first step
of each kind is left out as warm-up.

## Measured

### Engine only (`unit-tests "[soak-…]"`)

| scenario | private | heap | images | object data | other C++ | objects |
| -- | -- | -- | -- | -- | -- | -- |
| UNATCO Island ↔ HQ, per round trip | 31.3 ± 0.5 | 30.4 | 9.5 | 6.8 | 14.1 | +6,543 |
| NYC Street ↔ Bar, per round trip | 15.3 ± 0.6 | 14.8 | 4.3 | 3.3 | 7.2 | +3,480 |
| quickload, Island | 23.9 ± 0.5 | 23.3 | 7.7 | 4.8 | 10.8 | +4,734 |
| quicksave | 0.05 ± 0.2 | 0.01 | 0 | 0 | 0.01 | +17 |
| save list, 8 slots, `readSaveInfo` each | 0 | 0.02 | 0 | 0 | 0.01 | +16 |
| idle, UNATCO Island, per game minute | 0.35 ± 0.3 | 0.28 | 0 | 0.02 | 0.26 | +452 |
| idle, Market, Street, Canal, per minute | ≤ 0.01 | ≤ 0.004 | 0 | 0 | 0 | ≤ 3 |
| assault-rifle fire, 330 rounds/min, per minute | 4.97 ± 0.56 | 4.54 | 0 | 3.18 | 1.35 | +3,548 |
| churn, 60 transient spawns/s, per minute | 8.6 ± 0.8 | 7.80 | 0 | 5.35 | 2.44 | +6,144 |

All figures are MiB, mean ± sd, with n = 11 round trips, 14 quickloads and saves, 149
listings, and 10 or 5 minutes.

Per level, measured on revisits:
- Island (9.0 MB map): +24.4 MiB and +4,854 objects.
- HQ (2.5 MB): +6.9 MiB and +1,689 objects.
- Street (5.2 MB): +12.8 MiB and +2,689 objects.
- Bar (0.6 MB): +2.5 MiB and +791 objects.

A level load keeps about 1 MiB + 2.6 × its map file size. First visits cost more because
they also load shared classes, textures and meshes: Island +138, HQ +54, Street +122,
Bar +11 MiB.

Per destroyed actor, measured:
- **Fire:** 3.52 KiB heap (2.47 property data), 2.69 objects; 4.0 actors destroyed per
  round.
- **Churn:** 3.14 KiB heap (2.15 property data), 2.42 objects.
- **Idle Island:** each `AISenderEvent` keeps 0.64 KiB heap.

### Real executable (`exe-soak.ps1`, GL context, offline audio mixing)

The counters come from the exe's own `memstats` lines.

| scenario | private | images | object data | decoded sound | renderer caches | destroyed windows |
| -- | -- | -- | -- | -- | -- | -- |
| UNATCO round trip | 33.8 | 9.49 | 6.53 | 0.01 | ~0 (cleared at level change) | 137 (0.10 MiB data; the HUD is rebuilt per level) |
| Street ↔ Bar round trip | 17.2 | 4.27 | 3.06 | 0 | ~0 | 130 |
| quickload, Island | 26.0 | 7.71 | 4.78 | 0 | ~0 | 65 |
| quicksave | ~0 | 0 | 0 | 0.02 | 0.02 | 0 |
| fire, per minute | 9.0 | 0 | 2.18 | 0 | **3.27** | 0 |
| idle Market, per minute (after a +32 MiB first minute) | ~0 | 0 | 0.01 | 0.16 | 0 | 6 |
| 7 menu screens opened and closed | 0.82 | 0 | 0.36 | 0.01 | 0.01 | **508** |
| conversation (Paul Denton, ~52 s of speech) | +49 over 45 s | 0 | 0.01 | **+13.2** | +1.2 | 24 |

All figures are MiB, means of 2 runs × 9–39 steps.

- **Renderer caches during fire:** per minute, `meshDrawCaches` gains 807 entries (809
  dead, i.e. keyed on destroyed actors: 2.97 MiB) and `actorLightCache` 582 entries. Pose
  caches add 0.23 MiB. `occlusion` and its GL queries do not grow. The caches are bounded
  by `flushLevel`, so a travel round trip nets ~0 here.
- **Decoded sound, measured directly:** effects (WAV) take 3.0× their packed size. Speech
  (MP3) takes **44.3×**, i.e. 15.1 MiB decoded and 17.7 MiB private per minute of speech
  (`[soak-sounds]`, whole packages). The real conversation above confirms it: 13.2 MiB
  for ~52 s of speech.
- **Windows:** each round of 7 screens destroys 508 windows. They keep 0.36 MiB of
  property data, and 0.82 MiB private in all, so ~1.6 KiB per destroyed window.

Absolute levels: the engine alone sits at 202 MiB after `engine::initialize` and 340 MiB
on UNATCO Island; the exe sits at 655 MiB on the Island. After 12 UNATCO round trips the
engine is at 763 MiB, with 170,527 objects in `objects::all()` against 90,671 after the
first load.

### Per-tick cost (scenario 6)

| run | start → end |
| -- | -- |
| `World::tick`, idle (4 maps, 10 min) | flat: 0.16–0.50 ms, slope ≤ ±0.008 ms/min |
| `World::tick`, fire 10 min | flat in both runs: 0.8–1.2 ms (one run at 1.9 ms throughout under machine load) |
| `World::tick`, churn 60/s, 5 min | first run 0.78 → 1.28 ms; reruns 0.02–0.68 ms/min with other builds on the machine: **inconclusive, at most ~0.1 ms/min when quiet** |
| exe frame, fire, 5 min | 3.6 → 11 ms (wall time per game minute 6.5 s → 20 s) |
| exe frame, idle Market, 10 min | flat, ~2.2 ms |

The exe frame growth during fire is **live bullet-hole decals**, not retained memory:
- Pointed at one patch of floor, BulletHole actors went from 30 to 480 in 2 minutes
  (`actorstats`), and the exe's live actors grew 2,747 → 3,930 in 5 minutes.
- In the engine-only run the same decals expire, because they are never rendered.
- This looks like the original's "keep decals that are being looked at" behaviour; whether
  the original caps them is a parity question, outside this task.

The growing object table is not measurably slowing `World::tick` in ordinary play.

## Extrapolated

Assumptions:

- **Maps:** 77 campaign maps (`game-data/Maps`, without training and multiplayer), mean
  3.44 MB, so a mean level load keeps **~10 MiB**.
- **Level loads** per playthrough, revisits included: low 100, typical 150, high 250.
- **Quickloads:** low 30, typical 100, high 300, at ~10 MiB each (~13 MiB in the high
  case).
- **Speech:** the conversation packages hold ~590 min (203 MB of MP3 at 0.34 MB/min). A
  playthrough hears 25 / 40 / 60% of it, at 17.7 MiB/min; each sound is cached once.
- **Combat:** 10 / 30 / 60 minutes of continuous-rifle-fire equivalent, at 4.97 MiB/min
  (destroyed actors) + 3.27 MiB/min (renderer caches, bounded per level).
- **AI events:** ~0.1 MiB per minute averaged over levels; playtime 15 / 25 / 40 h.
- **UI screens:** 500 / 1,500 / 3,000 opened per playthrough (inventory, goals, logs,
  images, skills, augs, health, conversations, menus), at 0.117 MiB each.
- **First visits:** ~45 MiB per new hub, 14 hubs. Partly content a fixed engine would keep
  while in use.

### A typical level (30 minutes, mean-size map)

| source | low | typical | high |
| -- | -- | -- | -- |
| arriving (one level load) | 3 | 10 | 24 (Island-size) |
| quickloads | 0 | 3 × 10 = 30 | 10 × 24 = 240 |
| speech heard | 2 min = 35 | 8 min = 140 | 15 min = 265 |
| combat | 0 | 1.5 min = 12 | 5 min = 41 |
| UI screens | 10 × 0.12 = 1 | 40 = 5 | 100 = 12 |
| ambient AI events | 0 | 3 | 10 |
| **total** | **~40 MiB** | **~200 MiB** | **~590 MiB** |

For a 20-minute stay take two-thirds; for 40 minutes, four-thirds. Arrival is the only
item that does not scale with time.

### A full playthrough

| source | low | typical | high |
| -- | -- | -- | -- |
| decoded speech | 2.6 GiB | 4.2 GiB | 6.1 GiB |
| level loads (travel) | 1.0 | 1.5 | 2.4 |
| quickloads | 0.3 | 1.0 | 3.8 |
| first visits of new areas | 0.6 | 0.6 | 0.6 |
| destroyed UI windows | 0.06 | 0.17 | 0.34 |
| destroyed actors (combat) | 0.05 | 0.15 | 0.29 |
| renderer caches (combat, freed at each level change) | 0.03 | 0.10 | 0.19 |
| ambient AI events | 0.09 | 0.15 | 0.23 |
| quicksaves, save list | ~0 | ~0 | ~0 |
| **growth** | **~4.7 GiB** | **~7.9 GiB** | **~14 GiB** |
| **process size at the end** (plus 0.65 GiB at start) | **~5.4 GiB** | **~8.5 GiB** | **~14.6 GiB** |

**In sessions** (restarting resets everything): a 2.5-hour session is about a tenth of a
typical 25-hour game, so expect ~0.8 GiB of growth and ~1.5 GiB at the end. A session in a
talk-heavy stretch (UNATCO HQ, Hong Kong) can be twice that. In one sitting the process
passes 4 GiB partway through the campaign, and machines with 8 GiB of RAM page well
before the end.

## Share by source (typical playthrough, 7.9 GiB)

Sources follow the order of the background list. Bases marked "measured" come from the
counters; the share itself uses the playthrough assumptions above.

| source | share | basis |
| -- | -- | -- |
| Old level never freed at level change or quickload | **32%** (2.5 GiB) | measured split per round trip, below |
| … retired package file images | 9.5% | `linkerStats().retiredBytes`: 9.5 MiB per UNATCO round trip, 7.7 per Island quickload |
| … the level's objects: property data | 7% | `cls->size` sums: 6.8 / 4.8 MiB |
| … the level's objects: other C++ allocations | 14% | heap minus the two above: 14.1 / 10.8 MiB |
| … heap slack | 1% | private minus heap: ~1 / 0.6 MiB |
| Destroyed actors keeping object and data | **2%** | 3.1–3.5 KiB heap per destroyed actor |
| `World::cleanupDestroyed` cost | time only | no consistent growth measured |
| Renderer per-actor caches | **1.3%** | 3.27 MiB per minute of fire, ~all entries dead; zero across level changes |
| Decoded-sound cache | **53%** (4.2 GiB) | `cacheStats().decodedBytes`: 44× MP3 size, 15 MiB per minute of speech |
| Destroyed UI windows' members | **2.2%** | 508 windows / 0.82 MiB per 7 screens; 44% of it property data |
| *not on the list:* `AISenderEvent` objects | **1.9%** | 0.64 KiB each, 452/min on UNATCO Island, ~0 elsewhere |
| *not on the list:* first-use content of new areas | **7.6%** | partly legitimate |

Order for the fix:
1. **The sound cache.** Bound it, or keep speech compressed or as 16-bit only; dropping the
   float copy alone saves two-thirds.
2. **Free the old level and its package** at level change and quickload. Within that, the
   objects (21%) matter more than the file images (9.5%).
3. **Destroyed actors, windows and AI events.** Together ~6%; they matter for long
   sessions.
4. **Renderer caches.** Already bounded per level; they only grow within a long fight on
   one level.

## Surprises

- **Speech, not maps, dominates.** It decodes to 44.1 kHz mono, stored as float plus
  int16, 44× the packed size. One minute of dialogue keeps ~17.7 MiB. The AIBarks package
  alone is 727 MiB if all of it is heard.
- **The save list is negligible.** One package per slot per listing does happen, but each
  SaveInfo is 0.5 KiB with no snapshot (+2 objects per slot).
- **Quicksave is free; quickload costs a full level load.**
- **AI events leak on their own.** ~450 `AISenderEvent` objects a minute on UNATCO Island
  with nobody moving.
- **Renderer cache entries are almost all dead.** During fire, 100% of the new
  `meshDrawCaches` entries belong to destroyed actors. `occlusion` and its GL query objects
  do not grow, since the transient actors are not meshes it tracks.
- **The exe frame-time growth during fire** comes from bullet-hole decals that stay alive
  while in view. It is not a leak; it is a parity question.
- **The renderer gives its level memory back.** Per-leg exe figures swing (+47 MiB
  arriving on the Island, −17 MiB arriving at HQ), but a round trip nets out to the engine
  figure plus ~2–3 MiB.

## Not covered

- C++ members of destroyed windows apart from the object total: the 0.46 MiB per 7 screens
  beyond property data is everything non-property, measured only in aggregate.
- Per-class C++ sizes inside "other C++": only the total is measured.
- Linux: the soak cases compile there, but `heapAllocatedBytes` returns 0, and nothing was
  run.

## Repeating after the fix

    pwsh tests/soak/run-all.ps1 -Bin build/release/bin/Release -OutDir soak-after
    pwsh tests/soak/analyze.ps1 -Results soak-after

Compare with `baseline-2026-11-14-analysis.txt`. A working fix should show:
- **Level change:** round-trip and quickload private, heap, images and objects near 0.
- **Sound cache:** `sound_decoded` capped in the talk run and in `[soak-sounds]`.
- **Destroyed actors:** the per-destroyed-actor heap near 0.
- **Windows:** `destroyed_window_data` stops growing in the menus run.
- **Renderer caches:** `meshdraw_dead` staying near 0 in the fire run.

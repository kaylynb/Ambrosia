# Differences from the original

Ambrosia purposely behaves differently from Deus Ex GOTY 1112fm in some areas:

## Gameplay and physics

| Area | Original | Ambrosia |
|---|---|---|
| Random numbers | C runtime `rand()` | Own random streams: same range and distributions, different sequences |
| `ULevel::SpawnActor` | Returns an actor destroyed during the spawn; calls `SpawnNotify` | Returns None for it; no `SpawnNotify` (unused by Deus Ex) |
| `ULevel::MoveActor` | Sends `BumpWall` | Doesn't (unused by Deus Ex) |
| `AActor::physMovingBrush` | Clamps `KeyNum` above 7 to 8 | Clamps to 7; no map reaches it |
| BSP traces and point checks | Single precision | Double precision; only matters exactly on a boundary |
| Conversation binding | `ownerRefCount` carries over | Reset on level load, as conversations stay loaded between maps |
| `DumpLocation` natives | Return the location data | Return empty values (debug only) |
| AI event scoring | Stops after 2 ms of real time per tick; the receivers not reached miss that round | Every receiver every tick, so play does not depend on how fast the machine is |
| Natives | 891 | 834; missing editor brush builders, stat logging, networking, `Console.SaveTimeDemo` and five unused `ScriptedTexture` calls |

## Script VM

| Area | Original | Ambrosia |
|---|---|---|
| `Percent_FloatFloat`, `Sqrt` | NaN for `x % 0` and negative roots | 0 (on purpose) |
| Sine and cosine | Lookup tables | Computed; differ in the last bits |
| `Divide_IntInt` | `MinInt / -1` crashes | Wraps to `MinInt` (on purpose) |

## Frame-rate dependence

Code whose result depends on the tick length. `FrameTiming` in the video section of the ini
(`dx::frameTiming`, `engine/world.h`) is **Steady** by default.

| Area | Original | Steady |
|---|---|---|
| Level tick | Ticks clamped to 0.005–0.4 s, so the game runs fast above 200 fps | Frames under 5 ms are gathered until 5 ms pass |
| Animation | At most 4 passes of the main sequence per tick | Up to 64; blend slots unchanged |
| `Timer` | Fires at most once per tick | Once per period passed, up to 64 |
| Texture animation | Unlimited ones step once a frame; rate-limited ones lose leftover time and run slow (30/s runs at 20 at 60 fps) | Fixed rate (`MaxFrameRate` clamped to 0.01–100, else 60/s), leftover kept, up to 8 steps of catch-up |
| `FindStairRotation` | Eases pitch by `min(1, rate × dt)` | By `1 − e^(−rate × dt)` |
| Flicker & strobe lights| Updated each frame. Lights with associated sounds also restart often. | Moved below 3hz |
| Electricity (`ElectricityEmitter`) | Ticks each frame: a new random arc every frame, and its flicker (beam, hum and light) checked up to once a frame; its beam's bends drawn anew each frame | Ticks 30 times a second, and its bends change only then, so it looks as at 30 fps |
| Lasers confused by EMP (`LaserTrigger`, `BeamTrigger`) | Each frame turns the beam on with a chance of 1 in 20, off otherwise | Ticks 30 times a second while confused, so it flickers as at 30 fps |

## UI

None of these changes gameplay.

| Area | Original | Ambrosia |
|---|---|---|
| Sliders | Page repeatedly while the track is held | Page once, take the mouse wheel, drag the thumb from its center |
| Layout | Immediate | Once a frame, so script reads of `x`, `y`, `width` or `height` right after a change see the old values (delays `CreditsScrollWindow`'s end check by a frame) |
| Conversations | Speech and replies start at the left edge of the lower letterbox, with no scrolling | Speech is centered as a short line or a limited-width column; replies share a left edge in a centered column, and the letterbox scrolls, with a slim scrollbar, to keep the focused reply visible without moving the 3D view. `CenteredConversations=False` in the video section, or `set ini:Engine.Engine.ViewportManager CenteredConversations False` in the console, restores the original layout |
| Right-click in lists | Doesn't select | Selects rows |
| Console line (console, `Say`) | Typed only | Ctrl+V pastes the system clipboard as one line: line breaks and tabs become spaces, `~` and `` ` `` are dropped |
| Game Options | UI/Menu Background row (`UIBackground`) | Row removed; menus draw over the 3D view. A saved non-zero `UIBackground` still applies. |
| Sound Settings | Sample Rate, Sound Quality, Reverse Stereo, Dolby Surround, 3D Hardware Support | Removed and their ini keys ignored; mixing is at the device's rate. Where Steam Audio loads, the Audio Mixer row (HRTF, below) takes Sample Rate's place. |
| Fullscreen | Switches display mode | Borderless at desktop resolution; `ExclusiveFullscreen=True` switches modes as before |
| Screen Resolution (windowed) | Every mode from 640x480 | Standard sizes from 720 lines that fit the display (its aspect, 16:9, 16:10) plus the current size; `SetRes` and the ini take any size, but the shipped 640x480 counts as unset |
| Savegame snapshots | None with the OpenGL renderer | Always taken |

## Rendering and audio

| Area | Original | Ambrosia |
|---|---|---|
| `Actor.LastRenderTime` | Set by software span-buffer occlusion | GPU occlusion queries, a frame late; `bDisappear`, AI events and the light augmentation see it a frame late |
| Reverb | SSE or MMX filter by CPU, and EAX hardware reverb | SSE filter only; no EAX |
| Reverb send | Every sound, interface and infolink included | Interface sounds and the infolink stay dry, like music |
| HRTF (optional) | Stereo panning, or 3D sound hardware | Sound Settings' Audio Mixer row, Stereo or HRTF (`AmbrosiaHRTF`): Steam Audio binaural rendering for headphones |
| Raytraced sound (optional, off by default, not tuned and recommend using HRTF if playing) | One line check obstructs a hidden sound; reverb only in the 15 of 80 maps with reverb zones | `AmbrosiaRaytracedAudio=True`, on top of HRTF: Steam Audio occlusion and transmission through the level's BSP and movers, simulated reverb everywhere, and sound paths around corners from per-level baked acoustics (BUILD.md, "Baked acoustics"). Conversation is not occluded. `AmbrosiaReverbLevel`, `AmbrosiaPathing` and `AmbrosiaBakedReverb` tune or turn off parts of it. |

## Nintendo Switch

| Area | Original | Switch build |
|---|---|---|
| Controller | None | Built-in layout with Nintendo's A and B (A interacts, B crouches and goes back); prompts always Switch, no Button Prompts row |
| Typing | Keyboard | System keyboard on a focused text field; passwords and PINs hidden |
| Touch screen | None | In handheld mode, works menus and cursor screens as a mouse; nothing in play |
| Settings | Key bindings, Toggle Full-Screen Mode, Screen Resolution, Mouse Sensitivity | Removed; the screen is a fixed 1280x720 |

## Log

| Area | Original | Ambrosia |
|---|---|---|
| Log file | `System\DeusEx.log`, `Category: text` lines | `Ambrosia.log` / `DentEd.log` in the user data directory, with time, level and thread; the previous run's kept as `.old.log`. Categories named as the original's where one fits. |
| Console | Log file only (a log window with `-log`) | Log file only on Windows unless `--console` or redirected output; no log window |
| Fatal errors | Message box | Message box, unless standard error is redirected (scripts, tests) |

## Memory

The original collects garbage at level load and on `OBJ GARBAGE`, and deletes destroyed actors
once their references are cleared. Ambrosia collects at the same moments. Where it differs:

| Area | Original | Ambrosia |
|---|---|---|
| The level that was left | Kept alive by notes, goals and keys made inside the player (`new(Self)`) and carried to the next level | Deleted once the next level loads; carried objects are moved under the new player or the transient package, and leftover references to the old level are set to None. `GarbageCollection=False` keeps everything. |
| `OBJ GARBAGE` | Collects garbage | Not a console command |
| Object table slots | Most recently freed slot reused first | Oldest freed slot first, with a generation in the handle so stale handles read as None; none reused in the editor |
| Destroyed windows | Deleted at once, or when the last lock is released | At the end of the tick |
| Freed objects for debugging | None | `KeepFreedObjects=True` (or the overlay's Memory window) holds them until the next level change and logs script reads of them |
| Memory log | None | `memlog` lines at each level change and on quit |
| Decoded sound | Kept from first use until unregistered, no limit | Up to 64 MB, least recently used dropped first; `SoundCacheMegabytes` sets it, `0` keeps all |

## Fixes only mods reach

Cases shipped Deus Ex never reaches: mods, maps saved by other editors, or corrupt data.

| Area | Fixed |
|---|---|
| Pathfinding | Reach specs with a missing start or end are skipped (left by deleting a navigation point in some editors) |
| AI events | Event slot indices loaded from a save are masked to the 16 slots |
| `PostTouch` | `PendingTouch` is checked again after `PostTouch`, where the original would crash |
| `findObject` | An export not loaded outside the editor (`NotForClient`/`NotForServer`) no longer crashes the lookup |
| Radial menu icons | The crop handles texture formats other than P8 and RGBA8, and mips smaller than their size |
| Window child loops | Layout, visibility, tick and draw keep going when a child's script destroys or moves its next sibling |
| Meshes | A triangle with an out-of-range vertex index is skipped |
| Compiler array size | Zero, negative or above the engine's maximum is rejected; the original produced a corrupt layout. `[1]` stays legal. |
| Compiler default properties | A negative subscript is rejected; the original wrote before the property |
| Source export | `#exec` file paths that are absolute or leave the export directory are refused |

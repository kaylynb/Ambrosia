#include "game/game.h"

#include "audio/audio.h"
#include "core/gc.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/random.h"
#include "core/vm.h"
#include "debugui/actor_marks.h"
#include "engine/ai.h"
#include "deusex/known_logins.h"
#include "deusex/script.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "extension/extension.h"
#include "render/scene_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__SWITCH__)
#include <switch.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace dx::game {
    namespace {
        // Investigation: an FNV-1a hash of the game state script can see, for telling whether
        // two builds (compiler flags) play a run the same. Object references count by the
        // referenced object's name; strings and arrays by their contents.
        struct StateHash {
            uint64_t value = 1469598103934665603ull;

            void bytes(const void* data, size_t size)
            {
                const auto* p = static_cast<const uint8_t*>(data);
                for (size_t i = 0; i < size; i++) {
                    value = (value ^ p[i]) * 1099511628211ull;
                }
            }

            void property(const dx::UProperty* property, const uint8_t* memory, int depth)
            {
                if (depth > 8) {
                    return;
                }
                switch (property->type) {
                case dx::PropType::Byte:
                case dx::PropType::Int:
                case dx::PropType::Bool:
                case dx::PropType::Float:
                    bytes(memory, property->elementSize());
                    break;
                case dx::PropType::Name: {
                    // By its text, so the order names entered the table does not count.
                    const std::string& text = reinterpret_cast<const dx::Name*>(memory)->str();
                    bytes(text.data(), text.size());
                    break;
                }
                case dx::PropType::Object:
                case dx::PropType::Class: {
                    const dx::UObject* target = *reinterpret_cast<dx::UObject* const*>(memory);
                    const std::string& name = target ? target->name.str() : std::string();
                    bytes(name.data(), name.size());
                    break;
                }
                case dx::PropType::Str:
                case dx::PropType::String: {
                    const auto& text = *reinterpret_cast<const std::string*>(memory);
                    bytes(text.data(), text.size());
                    break;
                }
                case dx::PropType::Struct:
                    if (property->structType) {
                        for (const dx::UProperty* member : property->structType->properties) {
                            for (int32_t i = 0; i < member->arrayDim; i++) {
                                this->property(member, memory + member->offset + static_cast<size_t>(i) * member->elementSize(), depth + 1);
                            }
                        }
                    }
                    break;
                case dx::PropType::FixedArray:
                    if (property->inner) {
                        for (int32_t i = 0; i < property->fixedCount; i++) {
                            this->property(property->inner, memory + static_cast<size_t>(i) * property->inner->elementSize(), depth + 1);
                        }
                    }
                    break;
                case dx::PropType::Array:
                    if (property->inner) {
                        const auto& values = *reinterpret_cast<const std::vector<uint8_t>*>(memory);
                        size_t size = property->inner->elementSize();
                        for (size_t at = 0; size != 0 && at + size <= values.size(); at += size) {
                            this->property(property->inner, values.data() + at, depth + 1);
                        }
                    }
                    break;
                case dx::PropType::Map:
                case dx::PropType::Pointer:
                    break;
                }
            }
        };
    }

    void Game::scriptedAction(const std::string& action)
    {
        size_t space = action.find(' ');
        std::string verb = action.substr(0, space);
        std::string rest = space == std::string::npos ? std::string() : action.substr(space + 1);
        if (verb == "press" || verb == "release") {
            uint8_t key = dx::console::keyFromName(rest);
            if (dx::extension::wantsMouse() && (key == dx::IK_LeftMouse || key == dx::IK_RightMouse)) {
                dx::extension::mouseButton(key, verb == "press");
            } else {
                handleKey(key, verb == "press");
            }
        } else if (verb == "cursor") {
            float x = 0;
            float y = 0;
            std::sscanf(rest.c_str(), "%f %f", &x, &y);
            dx::extension::mouseMove(x, y);
        } else if (pad.scriptedAction(verb, rest)) {
            // A gamepad action, such as "pad a" or "radial belt 3".
        } else if (verb == "note") {
            // "note <text>" adds a note, as reading a datacube does.
            if (world && world->player()) {
                dx::vm::Call(world->player(), dx::Name("AddNote")).s(rest).run();
            }
        } else if (verb == "known") {
            // "known <source>" counts a text or conversation as read, such as
            // "known text:01_Datacube06", for checking the known-login buttons.
            dx::knownlogins::markRead(rest);
        } else if (verb == "frob") {
            // "frob <actor>" uses an actor by name, as pressing use on it would.
            for (dx::UObject* actor : world ? world->actors : std::vector<dx::UObject*> {}) {
                if (actor && !actor->destroyed && actor->name == dx::Name(rest)) {
                    // Remote computer animations can be asleep or still deactivating.
                    // This test action opens their screen directly, independent of range.
                    if (dx::script::isA(actor, "Computers")) {
                        dx::vm::Call(world->player(), dx::Name("InvokeComputerScreen")).obj(actor).f(-300).f(world->timeSeconds).run();
                    } else {
                        dx::vm::Call(actor, dx::Name("Frob")).obj(world->player()).obj(nullptr).run();
                    }
                    break;
                }
            }
        } else if (verb == "type") {
            for (char c : rest) {
                charEvent(c);
            }
        } else if (verb == "ccursor") {
            float x = 0;
            float y = 0;
            std::sscanf(rest.c_str(), "%f %f", &x, &y);
            x += std::floor(static_cast<float>(frameWidth - options.width) * 0.5f);
            y += std::floor(static_cast<float>(frameHeight - options.height) * 0.5f);
            dx::extension::mouseMove(x, y);
        } else if (verb == "mouse") {
            float dx = 0;
            float dy = 0;
            std::sscanf(rest.c_str(), "%f %f", &dx, &dy);
            dx::console::key(dx::IK_MouseX, dx::IST_Axis, dx);
            dx::console::key(dx::IK_MouseY, dx::IST_Axis, -dy);
        } else if (verb == "exec") {
            std::string output = dx::console::execute(world ? world->player() : nullptr, rest);
            if (!output.empty()) {
                dx::log::info("Log", "{}", output);
            }
        } else if (verb == "menu") {
            // "menu <class>" opens a menu screen, such as DeusEx.MenuScreenControls, over the
            // game as the menus' buttons do.
            dx::UClass* screen = dx::objects::findClass(rest);
            if (!screen || !dx::extension::rootWindow()) {
                dx::log::warn("Log", "menu: no menu screen class {}", rest);
            } else {
                dx::vm::Call(dx::extension::rootWindow(), dx::Name("InvokeMenuScreen")).obj(screen).run();
            }
        } else if (verb == "focus") {
            // "focus" logs what gamepad focus can reach on the screen on top, for checking that
            // every control works with a controller.
            dx::log::info("Log", "focus:\n{}", dx::extension::focusReport());
        } else if (verb == "shot") {
            pendingShot = rest;
        } else if (verb == "memstats") {
            // "memstats <label>" logs the memory counters, for the soak harness.
            printMemoryStats(rest, "memstats");
        } else if (verb == "worldhash") {
            // "worldhash" prints a hash of every live actor's script properties and state, and of
            // the random streams, for comparing builds; see StateHash.
            StateHash hash;
            size_t live = 0;
            for (dx::UObject* actor : world ? world->actors : std::vector<dx::UObject*> {}) {
                if (!actor || actor->destroyed || !actor->data || !actor->cls) {
                    continue;
                }
                live++;
                hash.bytes(actor->name.str().data(), actor->name.str().size());
                hash.bytes(dx::vm::stateName(actor).str().data(), dx::vm::stateName(actor).str().size());
                // Left out: the wall-clock date LevelInfo keeps, and ExtraTag, LightingTag and
                // SpecialTag, which the renderer's light gathering stamps, which depend on what is drawn.
                static const dx::Name skipped[] = { dx::Name("ExtraTag"), dx::Name("LightingTag"), dx::Name("SpecialTag"), dx::Name("Year"), dx::Name("Month"), dx::Name("Day"), dx::Name("DayOfWeek"), dx::Name("Hour"), dx::Name("Minute"), dx::Name("Second"), dx::Name("Millisecond") };
                for (const dx::UProperty* property : actor->cls->properties) {
                    if (std::find(std::begin(skipped), std::end(skipped), property->name) != std::end(skipped)) {
                        continue;
                    }
                    StateHash one;
                    for (int32_t i = 0; i < property->arrayDim; i++) {
                        hash.property(property, actor->data + property->offset + static_cast<size_t>(i) * property->elementSize(), 0);
                        if (rest == "dump") {
                            one.property(property, actor->data + property->offset + static_cast<size_t>(i) * property->elementSize(), 0);
                        }
                    }
                    if (rest == "dump") {
                        dx::log::info("DevHash", "worldhash-dump {}.{} {:016x} {}", actor->name.str(), property->name.str(), one.value, property->arrayDim == 1 ? property->toText(actor->data + property->offset) : std::string());
                    }
                }
            }
            uint64_t actorsOnly = hash.value;
            for (size_t s = 0; s < static_cast<size_t>(dx::random::Stream::Count); s++) {
                std::mt19937 copy = dx::random::engine(static_cast<dx::random::Stream>(s));
                uint32_t next = static_cast<uint32_t>(copy());
                hash.bytes(&next, sizeof(next));
            }
            dx::log::info("DevHash", "worldhash frame={} actors={} actorhash={:016x} hash={:016x}", frame, live, actorsOnly, hash.value);
            dx::log::flush();
        } else if (verb == "mixbench") {
            // "mixbench <voices> [seconds] [block]" times the stereo mixer offline; see
            // audio::benchmarkMix.
            int voices = 16;
            double seconds = 2;
            int block = 512;
            std::sscanf(rest.c_str(), "%d %lf %d", &voices, &seconds, &block);
            dx::log::info("DevAudio", "{}", dx::audio::benchmarkMix(voices, seconds, static_cast<size_t>(std::max(block, 1))));
            dx::log::flush();
        } else if (verb == "gctiming") {
            // "gctiming on" starts timing the parts of each gc::release and gc::freeObjects;
            // "gctiming report" prints the figures since.
            if (rest == "report") {
                dx::log::info("DevGarbage", "{}", dx::gc::releaseTimingReport());
                dx::log::flush();
            } else {
                dx::gc::setReleaseTiming(rest != "off");
            }
        } else if (verb == "gcdefer") {
            // "gcdefer on" frees nothing during play, as KeepFreedObjects=True does: windows,
            // actors and AI events wait for the level change's collection; for measuring frames
            // without play-time frees.
            dx::gc::setKeepFreed(rest != "off");
        } else if (verb == "camera") {
            // For measuring a fixed or scripted view (see CameraHold):
            //   camera at <x> <y> <z> <pitch> <yaw>             holds the player there
            //   camera spin <x> <y> <z> <pitch> <yaw> <step>    turns by step yaw units a frame
            //   camera path <x> <y> <z> <pitch> <yaw> <x> <y> <z> <pitch> <yaw> <frames>
            //                                                   moves to the second and back
            //   camera free                                     lets go
            //   camera where                                    prints the place and view rotation
            dx::UObject* player = world ? world->player() : nullptr;
            size_t split = rest.find(' ');
            std::string mode = rest.substr(0, split);
            std::string numbers = split == std::string::npos ? std::string() : rest.substr(split + 1);
            float v[11] = {};
            int read = std::sscanf(numbers.c_str(), "%f %f %f %f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]);
            CameraHold hold;
            hold.active = true;
            hold.startFrame = frame;
            hold.from = { v[0], v[1], v[2] };
            hold.fromRotation = { static_cast<int32_t>(v[3]), static_cast<int32_t>(v[4]), 0 };
            hold.to = hold.from;
            hold.toRotation = hold.fromRotation;
            if (mode == "free") {
                cameraHold.active = false;
            } else if (mode == "where") {
                if (player) {
                    dx::Vector location = dx::actor::Location(player);
                    dx::Rotator view = dx::pawn::ViewRotation(player);
                    dx::log::info("Log", "camera where {:.1f} {:.1f} {:.1f} {} {}", location.x, location.y, location.z, view.pitch & 65535, view.yaw & 65535);
                    dx::log::flush();
                }
            } else if (mode == "at" && read == 5) {
                cameraHold = hold;
            } else if (mode == "spin" && read == 6) {
                hold.spinPerFrame = static_cast<int>(v[5]);
                cameraHold = hold;
            } else if (mode == "path" && read == 11 && v[10] >= 1) {
                hold.to = { v[5], v[6], v[7] };
                hold.toRotation = { static_cast<int32_t>(v[8]), static_cast<int32_t>(v[9]), 0 };
                hold.pathFrames = static_cast<int>(v[10]);
                cameraHold = hold;
            } else {
                dx::log::warn("Log", "camera: expected at|spin|path|free|where with numbers, not \"{}\"", rest);
            }
            if (cameraHold.active) {
                holdCamera();
            }
        } else if (verb == "mark" || verb == "unmark") {
            // "mark <selector>" marks actors over the level, with their collision cylinders,
            // showing the overlay passive if it was hidden; "unmark <selector>|all" takes marks
            // away. The selector is as debugui/actor_marks.h describes, e.g. "* LightType=LT_Flicker".
            std::string output = dx::console::execute(world ? world->player() : nullptr, "DebugOverlay " + verb + " " + rest);
            dx::log::info("Log", "{}", output);
        } else if (verb == "cheats") {
            // "cheats" turns on the stock cheats and then god mode, ghost (fly through walls) and
            // invisible (bDetectable off, so the AI does not notice the player), for inspecting a
            // level.
            dx::UObject* player = world ? world->player() : nullptr;
            for (const char* command : { "set DeusExPlayer bCheatsEnabled True", "god", "ghost", "invisible 1" }) {
                std::string output = dx::console::execute(player, command);
                if (!output.empty()) {
                    dx::log::info("Log", "{}", output);
                }
            }
        } else if (verb == "view") {
            // "view <selector> [distance]" puts the player where they can see the first actor
            // the selector matches (debugui/actor_marks.h), or the point "<x> <y> <z>", about
            // distance units away (300 when not given), looking at it, and holds them there for half a second. Best after
            // "cheats", so the spot need not be one a player could stand on.
            viewActor(rest);
        } else if (verb == "trigger") {
            // "trigger <selector>" triggers every actor the selector matches, as an event with
            // its Tag would, the player being the instigator: turns on a TriggerLight, opens a
            // mover. Logs each one's Tag and the actors whose Event would trigger it in play.
            dx::UObject* player = world ? world->player() : nullptr;
            std::vector<dx::UObject*> found = dx::debugui::findActors(world.get(), rest);
            if (found.empty()) {
                dx::log::warn("Log", "trigger: nothing matches {}", rest);
            }
            for (dx::UObject* actor : found) {
                dx::Name tag = dx::actor::Tag(actor);
                std::string sources;
                for (dx::UObject* other : world->actors) {
                    if (other && !other->destroyed && other != actor && tag != dx::Name() && dx::actor::Event(other) == tag) {
                        sources += " " + other->name.str() + " (" + other->cls->name.str() + ")";
                    }
                }
                dx::log::info("Log", "trigger {} tag {}, triggered in play by{}", actor->name.str(), tag.str(), sources.empty() ? " nothing" : sources);
                dx::vm::Call(actor, dx::Name("Trigger")).obj(player).obj(player).run();
            }
        } else if (verb == "state") {
            dx::UObject* player = world ? world->player() : nullptr;
            if (player) {
                dx::Vector location = dx::actor::Location(player);
                dx::log::info("Log", "player at {:.1f} {:.1f} {:.1f} yaw {} state {} health {} level time {:.2f}{}", location.x, location.y, location.z, dx::actor::Rotation(player).yaw & 65535, dx::vm::stateName(player).str(), dx::pawn::Health(player), world->timeSeconds, world->paused ? " paused" : "");
            }
        }
    }

    void Game::holdCamera()
    {
        dx::UObject* player = world ? world->player() : nullptr;
        if (!player) {
            return;
        }
        const CameraHold& hold = cameraHold;
        int elapsed = std::max(frame - hold.startFrame, 0);
        dx::Vector location = hold.from;
        dx::Rotator rotation = hold.fromRotation;
        if (hold.pathFrames > 0) {
            // There and back: 0 to 1 over pathFrames, then 1 to 0.
            int period = hold.pathFrames * 2;
            int at = elapsed % period;
            float t = static_cast<float>(at <= hold.pathFrames ? at : period - at) / static_cast<float>(hold.pathFrames);
            location = hold.from + (hold.to - hold.from) * t;
            rotation.pitch = hold.fromRotation.pitch + static_cast<int32_t>(std::lround(static_cast<float>(hold.toRotation.pitch - hold.fromRotation.pitch) * t));
            rotation.yaw = hold.fromRotation.yaw + static_cast<int32_t>(std::lround(static_cast<float>(hold.toRotation.yaw - hold.fromRotation.yaw) * t));
        }
        rotation.yaw += hold.spinPerFrame * elapsed;
        dx::Vector current = dx::actor::Location(player);
        if (current.x != location.x || current.y != location.y || current.z != location.z) {
            if (!world->farMoveActor(player, location, false, false) && !cameraHold.reportedFailure) {
                cameraHold.reportedFailure = true;
                dx::log::warn("Log", "camera: could not move the player to {:.1f} {:.1f} {:.1f}", location.x, location.y, location.z);
            }
        }
        dx::actor::Velocity(player) = {};
        dx::actor::Acceleration(player) = {};
        dx::actor::Rotation(player) = { 0, rotation.yaw, 0 };
        dx::pawn::ViewRotation(player) = rotation;
    }

    void Game::viewActor(const std::string& argument)
    {
        dx::UObject* player = world ? world->player() : nullptr;
        if (!player) {
            return;
        }
        // A last word that is a number is the distance; the rest is the selector.
        std::string selector = argument;
        float distance = 300;
        size_t lastSpace = argument.find_last_of(' ');
        if (lastSpace != std::string::npos) {
            char* end = nullptr;
            float number = std::strtof(argument.c_str() + lastSpace + 1, &end);
            if (end && *end == 0 && number > 0) {
                distance = number;
                selector = argument.substr(0, lastSpace);
            }
        }
        // Three numbers are a point to look at, such as a spot on a mirror; else the first actor
        // the selector matches.
        dx::Vector target;
        std::string targetName;
        float x = 0;
        float y = 0;
        float z = 0;
        float given = 0;
        char extra = 0;
        // With a point, "<x> <y> <z> <distance> <height>" also asks for the eye about height
        // units above the point, as for looking down onto a floor mirror; else about level.
        std::optional<float> height;
        if (std::sscanf(argument.c_str(), "%f %f %f %f %f %c", &x, &y, &z, &distance, &given, &extra) == 5) {
            selector = std::format("{} {} {}", x, y, z);
            height = given;
        }
        if (std::sscanf(selector.c_str(), "%f %f %f %c", &x, &y, &z, &extra) == 3) {
            target = { x, y, z };
            targetName = std::format("{:.0f} {:.0f} {:.0f}", x, y, z);
        } else {
            std::vector<dx::UObject*> found = dx::debugui::findActors(world.get(), selector);
            if (found.empty()) {
                dx::log::warn("Log", "view: no actor matches \"{}\"", selector);
                return;
            }
            target = dx::actor::Location(found.front());
            targetName = found.front()->name.str();
        }
        float eyeHeight = dx::pawn::BaseEyeHeight(player);
        // Around the actor at the distance and a little nearer and further, from a little above
        // to well below it: the first spot by score that is out of the level's walls and sees it,
        // nearest the distance and about level with it.
        dx::Vector spot = target - dx::Vector { 0, 0, eyeHeight };
        float best = -1e9f;
        // The eye's height over the target the spot is scored by.
        float wanted = height ? *height : -40.0f;
        std::vector<float> rises { -120.0f, -80.0f, -40.0f, 0.0f, 40.0f, 80.0f, 120.0f };
        if (height) {
            for (float step : { -40.0f, -20.0f, 0.0f, 20.0f, 40.0f }) {
                rises.push_back(*height + step);
            }
        }
        for (float scale : { 1.0f, 0.75f, 1.4f, 1.8f }) {
            // Up to well above, for something on a floor.
            for (float dz : rises) {
                for (int k = 0; k < 32; k++) {
                    float angle = static_cast<float>(k) * 2.0f * dx::Pi / 32.0f;
                    float r = distance * scale;
                    dx::Vector p = target + dx::Vector { r * std::cos(angle), r * std::sin(angle), dz - eyeHeight };
                    dx::CheckResult hit;
                    if (world->pointCheck(hit, p, { 12, 12, 48 }, nullptr, false)) {
                        continue;
                    }
                    if (!world->fastLineCheck(target, p + dx::Vector { 0, 0, eyeHeight })) {
                        continue;
                    }
                    float score = -std::abs(r - distance) - std::abs(dz - wanted);
                    if (score > best) {
                        best = score;
                        spot = p;
                    }
                }
            }
        }
        if (best <= -1e9f) {
            dx::log::warn("Log", "view: no clear spot sees {}; it may be closed in, so the view is from where it is", targetName);
        }
        dx::Vector look = target - (spot + dx::Vector { 0, 0, eyeHeight });
        int32_t yaw = static_cast<int32_t>(std::lround(std::atan2(look.y, look.x) * 32768.0f / dx::Pi)) & 65535;
        int32_t pitch = static_cast<int32_t>(std::lround(std::atan2(look.z, std::hypot(look.x, look.y)) * 32768.0f / dx::Pi)) & 65535;
        CameraHold hold;
        hold.active = true;
        hold.startFrame = frame;
        hold.releaseFrame = frame + 30;
        hold.from = spot;
        hold.fromRotation = { pitch, yaw, 0 };
        hold.to = hold.from;
        hold.toRotation = hold.fromRotation;
        cameraHold = hold;
        holdCamera();
        dx::log::info("Log", "view {} from {:.0f} {:.0f} {:.0f} {} {}", targetName, spot.x, spot.y, spot.z, pitch, yaw);
    }

    uint64_t Game::processMemoryBytes()
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters {};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            return counters.PrivateUsage;
        }
        return 0;
#elif defined(__SWITCH__)
        u64 used = 0;
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        return used;
#elif defined(__linux__)
        unsigned long long pages = 0;
        unsigned long long resident = 0;
        if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
            if (std::fscanf(statm, "%llu %llu", &pages, &resident) != 2) {
                resident = 0;
            }
            std::fclose(statm);
        }
        return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
        return 0;
#endif
    }

    void Game::logLevelMemory(const std::string& map, bool exiting)
    {
        // A record per level change, for comparing memory over a playthrough: what the
        // collection did and what is left. "memlog" rather than "memstats", which the soak
        // harness reads at frames it chose.
        dx::gc::Stats gc = dx::gc::stats();
        const std::vector<dx::gc::LevelMemory>& history = dx::gc::levelMemory();
        size_t freedBefore = 0;
        for (const dx::gc::LevelMemory& level : history) {
            freedBefore += level.freedInLevel;
        }
        size_t freedInLevel = gc.freedObjects - std::min(freedBefore, gc.freedObjects);
        if (exiting) {
            // Quitting runs no collection: what the level freed during play, and what is left.
            dx::log::info("DevMemory", "memlog exit map={} freed_in_level={} free_max_ms={:.3f} frees={} freed={} free_ms={:.2f} kept_freed={}",
                map, freedInLevel, gc.longestFreeMilliseconds, gc.frees, gc.freedObjects, gc.freeMilliseconds, dx::gc::keptFreedObjects());
        } else {
            dx::log::info("DevMemory", "memlog collect map={} process_before={} collect_ms={:.2f} collected={} dangling_cleared={} freed_in_level={} free_max_ms={:.3f} frees={} freed={} free_ms={:.2f} kept_freed={}",
                map, processBeforeTravel, gc.lastCollectMilliseconds, gc.lastCollectedObjects, gc.lastDanglingCleared, freedInLevel, gc.longestFreeMilliseconds, gc.frees, gc.freedObjects, gc.freeMilliseconds, dx::gc::keptFreedObjects());
        }
        // What was freed during the level just left, by class, the most script data first: what
        // would pile up until the level change if nothing were freed during play.
        std::vector<dx::gc::ClassCount> byClass = dx::gc::freedByClassInLevel();
        constexpr size_t Shown = 12;
        std::string classes;
        uint64_t levelBytes = 0;
        for (size_t i = 0; i < byClass.size(); i++) {
            levelBytes += byClass[i].freedBytes;
            if (i < Shown) {
                classes += std::format(" {}={}/{}", byClass[i].className, byClass[i].freed, byClass[i].freedBytes);
            }
        }
        if (byClass.size() > Shown) {
            classes += std::format(" (+{} classes)", byClass.size() - Shown);
        }
        dx::log::info("DevMemory", "memlog freed map={} classes={} bytes={} top(count/bytes):{}", map, byClass.size(), levelBytes, classes);
        MemoryTotals totals = printMemoryStats(map, "memlog");
        dx::gc::LevelMemory record;
        record.map = map;
        record.processBytes = totals.process;
        record.objects = totals.objects;
        record.scriptBytes = totals.data;
        record.collectMilliseconds = gc.lastCollectMilliseconds;
        record.collected = gc.lastCollectedObjects;
        record.danglingCleared = gc.lastDanglingCleared;
        record.freedInLevel = freedInLevel;
        record.longestFreeMilliseconds = gc.longestFreeMilliseconds;
        dx::gc::recordLevelMemory(std::move(record));
        dx::gc::resetLevelFigures();
    }

    Game::MemoryTotals Game::printMemoryStats(const std::string& label, const char* prefix)
    {
        size_t objects = 0;
        size_t destroyed = 0;
        uint64_t dataBytes = 0;
        uint64_t destroyedDataBytes = 0;
        size_t windows = 0;
        size_t destroyedWindows = 0;
        uint64_t destroyedWindowBytes = 0;
        static const dx::Name windowName("Window");
        for (const dx::UObject* object : dx::objects::all()) {
            if (!object) {
                continue;
            }
            uint64_t size = object->data && object->cls ? object->cls->size : 0;
            bool isWindow = object->isA(windowName);
            objects++;
            dataBytes += size;
            windows += isWindow ? 1u : 0u;
            if (object->destroyed) {
                destroyed++;
                destroyedDataBytes += size;
                destroyedWindows += isWindow ? 1u : 0u;
                destroyedWindowBytes += isWindow ? size : 0u;
            }
        }
        dx::objects::LinkerStats linkers = dx::objects::linkerStats();
        dx::audio::CacheStats sounds = dx::audio::cacheStats();
        dx::SceneRenderer::CacheStats caches = renderer ? renderer->cacheStats() : dx::SceneRenderer::CacheStats {};
        MemoryTotals totals;
        totals.process = processMemoryBytes();
        totals.objects = objects;
        totals.data = dataBytes;
        dx::log::info("DevMemory",
            "{} {} process={} frame={} table={} objects={} destroyed={} data={} destroyed_data={} windows={} destroyed_windows={} destroyed_window_data={} actors={}"
            " linkers={} linker_bytes={} retired={} retired_bytes={} sounds={} sound_source={} sound_decoded={} modules={}"
            " meshdraw={} meshdraw_dead={} meshdraw_bytes={} pose={} pose_dead={} pose_bytes={} light={} light_dead={} light_bytes={}"
            " occlusion={} occlusion_dead={} queries={} brushdraw={} brushdraw_dead={}",
            prefix, label, totals.process, frame, dx::objects::all().size(), objects, destroyed, dataBytes, destroyedDataBytes, windows, destroyedWindows, destroyedWindowBytes, world ? world->actors.size() : 0,
            linkers.loaded, linkers.loadedBytes, linkers.retired, linkers.retiredBytes, sounds.sounds, sounds.sourceBytes, sounds.decodedBytes, sounds.modules,
            caches.meshDraw, caches.meshDrawDead, caches.meshDrawBytes, caches.pose, caches.poseDead, caches.poseBytes, caches.light, caches.lightDead, caches.lightBytes,
            caches.occlusion, caches.occlusionDead, caches.occlusionQueries, caches.brushDraw, caches.brushDrawDead);
        dx::log::flush();
        return totals;
    }
}

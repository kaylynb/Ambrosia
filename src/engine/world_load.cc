#include "engine/world.h"

#include "core/config.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/platform.h"
#include "core/profile.h"
#include "core/settings.h"
#include "core/vm.h"
#include "engine/actor.h"
#include "engine/level_events.h"
#include "engine/resources.h"
#include "engine/savegame.h"

#include <algorithm>
#include <cctype>

namespace dx {
    namespace {
        struct ParsedUrl {
            std::string map;
            std::string portal;
            std::vector<std::string> options;
        };

        ParsedUrl parseUrl(const std::string& text)
        {
            ParsedUrl url;
            std::string rest = text;
            size_t hash = rest.find('#');
            if (hash != std::string::npos) {
                // The portal runs to the next option, so "Map#Portal?Opt" keeps its options.
                size_t end = rest.find('?', hash);
                url.portal = rest.substr(hash + 1, end == std::string::npos ? std::string::npos : end - hash - 1);
                rest.erase(hash, end == std::string::npos ? std::string::npos : end - hash);
            }
            size_t question = rest.find('?');
            url.map = rest.substr(0, question);
            while (question != std::string::npos) {
                size_t next = rest.find('?', question + 1);
                url.options.push_back(rest.substr(question + 1, next == std::string::npos ? std::string::npos : next - question - 1));
                question = next;
            }
            size_t dot = url.map.rfind('.');
            if (dot != std::string::npos) {
                url.map = url.map.substr(0, dot);
            }
            size_t slash = url.map.find_last_of("/\\");
            if (slash != std::string::npos) {
                url.map = url.map.substr(slash + 1);
            }
            return url;
        }

        std::string option(const ParsedUrl& url, std::string_view key)
        {
            for (const std::string& item : url.options) {
                size_t equals = item.find('=');
                std::string itemKey = item.substr(0, equals);
                if (iequals(itemKey, key)) {
                    return equals == std::string::npos ? std::string() : item.substr(equals + 1);
                }
            }
            return {};
        }

        std::string optionsString(const ParsedUrl& url)
        {
            std::string result;
            for (const std::string& item : url.options) {
                result += "?" + item;
            }
            return result;
        }

    }

    bool World::loadMap(const std::string& mapUrl)
    {
        DX_ZONE_NAMED("Load map");
        ParsedUrl parsed = parseUrl(mapUrl);
        std::vector<std::string> defaults;
        for (const auto& [key, value] : config::user().section("DefaultPlayer")) {
            if (option(parsed, key).empty() && std::none_of(parsed.options.begin(), parsed.options.end(), [&](const std::string& item) { return iequals(item.substr(0, item.find('=')), key); })) {
                defaults.push_back(key + "=" + value);
            }
        }
        parsed.options.insert(parsed.options.begin(), defaults.begin(), defaults.end());
        Linker* linker = savegame::takePendingLevel(parsed.map);
        bool restored = linker != nullptr;
        if (!linker) {
            DX_ZONE_NAMED("Open map package");
            linker = objects::loadPackage(Name(parsed.map));
        }
        if (!linker) {
            log::error("DevLoad", "map not found: {}", parsed.map);
            return false;
        }
        int32_t levelRef = linker->findExport(Name("MyLevel"), Name("Level"), "");
        if (levelRef == 0) {
            log::error("DevLoad", "no level in {}", parsed.map);
            return false;
        }
        // The level this world had goes from here, loaded or not.
        if (levelAnnounced) {
            levelAnnounced = false;
            levelevents::levelUnloading(*this);
        }
        // A saved level cut short only fails here, when its exports are read.
        try {
            DX_ZONE_NAMED("Load level objects");
            level = cast<ULevel>(linker->object(levelRef));
            objects::ensureLoaded(level);
            objects::flushLoads();
        } catch (const std::exception& failure) {
            log::error("DevLoad", "cannot load {}: {}", parsed.map, failure.what());
            // Drain what the broken package left queued so the next map's loads do not hit it;
            // each failure has taken its object off the queue.
            for (;;) {
                try {
                    objects::flushLoads();
                    break;
                } catch (const std::exception&) {
                }
            }
            return false;
        }
        if (!level) {
            return false;
        }
        if (replaceTravelInfo) {
            level->travelInfo = arrivalTravelInfo;
        }
        url = mapUrl;
        actors.clear();
        actorsVersion++;
        for (UObject* a : level->actors) {
            if (a || actors.size() < 2) {
                actors.push_back(a);
            }
        }
        level->actors = actors;
        collisionHash.clear();
        collisionHash.sync(actors);
        levelInfo = actors.empty() ? nullptr : actors[0];
        if (!levelInfo) {
            return false;
        }
        timeSeconds = 0;
        for (UObject* a : actors) {
            if (!a) {
                continue;
            }
            actor::XLevel(a) = level;
            actor::Level(a) = levelInfo;
            if (!levelinfo::bBegunPlay(levelInfo)) {
                for (int i = 0; i < 4; i++) {
                    actor::Touching(a, i) = nullptr;
                }
            }
        }
        if (engine::gameHooks().levelLoaded) {
            engine::gameHooks().levelLoaded(*this);
        }
        if (restored) {
            savegame::levelRestored(*this);
        }
        std::string computerName = platform::hostName();
        std::transform(computerName.begin(), computerName.end(), computerName.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        levelinfo::ComputerName(levelInfo) = computerName;
        levelinfo::EngineVersion(levelInfo) = "1100";
        levelinfo::MinNetVersion(levelInfo) = "1100";
        levelinfo::bHighDetailMode(levelInfo) = 1;

        UObject* game = levelinfo::Game(levelInfo);
        if (!game) {
            UClass* gameClass = nullptr;
            std::string gameName = option(parsed, "Game");
            if (!gameName.empty()) {
                gameClass = objects::findClass(gameName);
            }
            if (!gameClass) {
                gameClass = cast<UClass>(levelinfo::DefaultGameType(levelInfo));
            }
            if (!gameClass) {
                gameClass = objects::findClass(settings::get(settings::defaultGame));
            }
            objects::flushLoads();
            game = spawn(gameClass, nullptr, Name(), {}, {}, nullptr, true);
            levelinfo::Game(levelInfo) = game;
        }
        gameInfo = game;

        if (!levelinfo::bBegunPlay(levelInfo)) {
            DX_ZONE_NAMED("Begin play");
            levelinfo::bBegunPlay(levelInfo) = 1;
            levelinfo::bStartup(levelInfo) = 1;
            std::string options = optionsString(parsed);
            if (gameInfo) {
                vm::Call(gameInfo, Name("InitGame")).s(options).s("").run();
            }
            auto forEachActor = [this](const std::function<void(UObject*)>& visit) {
                for (size_t i = 0; i < actors.size(); i++) {
                    UObject* a = actors[i];
                    if (a && !a->destroyed) {
                        visit(a);
                    }
                }
            };
            forEachActor([](UObject* a) {
                vm::event(a, Name("PreBeginPlay"));
            });
            forEachActor([](UObject* a) {
                vm::event(a, Name("BeginPlay"));
            });
            forEachActor([this](UObject* a) {
                region(a) = { levelInfo, -1, 0 };
                if (isPawn(a)) {
                    *reinterpret_cast<PointRegion*>(pawn::FootRegion(a)) = { levelInfo, -1, 0 };
                    *reinterpret_cast<PointRegion*>(pawn::HeadRegion(a)) = { levelInfo, -1, 0 };
                }
                setLocationZone(a, actor::Location(a), true);
            });
            forEachActor([](UObject* a) {
                actor::LastRenderTime(a) = -10.0f;
            });
            forEachActor([](UObject* a) {
                vm::event(a, Name("PostBeginPlay"));
            });
            forEachActor([](UObject* a) {
                vm::event(a, Name("SetInitialState"));
            });
            forEachActor([this](UObject* a) {
                Name attachTag = actor::AttachTag(a);
                if (!attachTag.none()) {
                    for (UObject* other : actors) {
                        if (other && !other->destroyed && actor::Tag(other) == attachTag) {
                            setBase(a, other, false);
                            break;
                        }
                    }
                    return;
                }
                if (actor::Base(a) || !actor::bCollideWorld(a)) {
                    return;
                }
                bool candidate = a->isA(engineClasses().decoration) || a->isA(engineClasses().inventory) || isPawn(a);
                uint8_t physics = actor::Physics(a);
                if (candidate && (physics == PHYS_None || physics == PHYS_Rotating)) {
                    findBase(a);
                    if (actor::Base(a) == levelInfo) {
                        setBase(a, nullptr, false);
                    }
                }
            });
            levelinfo::bStartup(levelInfo) = 0;
        }
        for (size_t i = 0; i < actors.size(); i++) {
            if (actors[i] && !actors[i]->destroyed) {
                vm::event(actors[i], Name("PostPostBeginPlay"));
            }
        }
        std::vector<UObject*> sorted(actors.begin(), actors.begin() + std::min<ptrdiff_t>(2, static_cast<ptrdiff_t>(actors.size())));
        for (size_t i = 2; i < actors.size(); i++) {
            if (actors[i] && actor::bStatic(actors[i])) {
                sorted.push_back(actors[i]);
            }
        }
        firstDynamicActor = sorted.size();
        for (size_t i = 2; i < actors.size(); i++) {
            if (actors[i] && !actor::bStatic(actors[i])) {
                sorted.push_back(actors[i]);
            }
        }
        actors = sorted;
        actorsVersion++;
        level->actors = actors;
        collisionHash.sync(actors);
        if (gameInfo) {
            vm::event(gameInfo, Name("DetailChange"));
        }

        UClass* playerClass = nullptr;
        std::string className = option(parsed, "Class");
        if (!className.empty()) {
            playerClass = objects::findClass(className);
        }
        if (!playerClass) {
            playerClass = objects::findClass(settings::get(settings::playerClass));
        }
        objects::flushLoads();
        if (gameInfo && playerClass) {
            vm::Call login(gameInfo, Name("Login"));
            login.s(parsed.portal).s(optionsString(parsed)).s("").obj(playerClass).run();
            UObject* pawn = login.returnValue() ? *reinterpret_cast<UObject**>(login.returnValue()) : nullptr;
            if (!pawn) {
                std::string error = login.parameter(2) ? *reinterpret_cast<std::string*>(login.parameter(2)) : std::string();
                log::error("DevLoad", "login failed: {}", error);
                return false;
            }
            if (!viewport) {
                viewport = objects::newObject(engineClasses().viewport, objects::packageObject(Name("Transient")), Name("Viewport0"), 0);
                UClass* consoleClass = objects::findClass(settings::get(settings::console));
                if (consoleClass) {
                    UObject* consoleObject = objects::newObject(consoleClass, objects::packageObject(Name("Transient")), Name(), 0);
                    console::Viewport(consoleObject) = viewport;
                    player::Console(viewport) = consoleObject;
                }
            }
            UObject* oldActor = player::Actor(viewport);
            if (oldActor) {
                playerpawn::Player(oldActor) = nullptr;
            }
            playerpawn::Player(pawn) = viewport;
            player::Actor(viewport) = pawn;
            localPlayer = pawn;
            vm::event(pawn, Name("Possess"));
            vm::event(pawn, Name("TravelPreAccept"));
            std::vector<UObject*> accepted = applyTravel(pawn);
            for (auto it = accepted.rbegin(); it != accepted.rend(); ++it) {
                vm::event(*it, Name("TravelPreAccept"));
            }
            vm::Call(gameInfo, Name("AcceptInventory")).obj(pawn).run();
            for (auto it = accepted.rbegin(); it != accepted.rend(); ++it) {
                vm::event(*it, Name("TravelPostAccept"));
            }
            vm::event(pawn, Name("TravelPostAccept"));
            vm::Call(gameInfo, Name("PostLogin")).obj(pawn).run();
        }
        levelAnnounced = true;
        levelevents::levelLoaded(*this);
        return true;
    }
}

#include "fixtures.h"
#include "test_paths.h"

#include "audio/audio.h"
#include "core/gc.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/savegame.h"
#include "extension/extension.h"

#include <catch_amalgamated.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace dx::test {
    namespace {
        std::atomic<unsigned> tempCounter { 0 };
        std::vector<std::function<void()>> pendingUndo;

        class UndoAtTestEnd : public Catch::EventListenerBase {
        public:
            using Catch::EventListenerBase::EventListenerBase;

            void testCaseEnded(const Catch::TestCaseStats&) override
            {
                while (!pendingUndo.empty()) {
                    std::function<void()> undo = std::move(pendingUndo.back());
                    pendingUndo.pop_back();
                    undo();
                }
            }
        };
    }

    void atTestEnd(std::function<void()> undo)
    {
        pendingUndo.push_back(std::move(undo));
    }

    TempDir::TempDir(std::string_view label)
    {
        std::string name = "ambrosia-test-" + std::to_string(getpid()) + "-" + std::to_string(++tempCounter) + "-" + std::string(label);
        directory = std::filesystem::temp_directory_path() / name;
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(directory, error);
    }

    TempDir::~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    SaveRoot::SaveRoot(std::string_view label)
        : folder(label)
        , previous(savegame::saveRoot())
    {
        savegame::setSaveRoot(folder.path());
    }

    SaveRoot::~SaveRoot()
    {
        savegame::setSaveRoot(previous);
    }

    ConfigSnapshot::ConfigSnapshot()
        : system(config::system())
        , user(config::user())
    {
    }

    ConfigSnapshot::~ConfigSnapshot()
    {
        config::system() = system;
        config::user() = user;
    }

    EditorMode::EditorMode()
        : previous(objects::editorMode())
    {
        objects::setEditorMode(true);
    }

    EditorMode::~EditorMode()
    {
        objects::setEditorMode(previous);
    }

    ScriptEditing::ScriptEditing()
    {
        engine::initialize(dx::test::gameData());
        INFO("the engine started outside editor mode; run the [editor-scripts] tests in a process of their own");
        REQUIRE(objects::objectClass()->scriptText != nullptr);
    }

    PackageGuard::PackageGuard(std::string name)
        : package(std::move(name))
    {
        unload();
    }

    PackageGuard::~PackageGuard()
    {
        unload();
    }

    void PackageGuard::unload() const
    {
        objects::unloadPackage(Name(package));
        objects::unloadPackage(Name("SaveGame_" + package));
    }

    namespace {
        std::string initialized(std::string map)
        {
            engine::initialize(dx::test::gameData());
            return map;
        }
    }

    LoadedMap::LoadedMap(std::string map)
        : guard(initialized(map))
    {
        REQUIRE(world.loadMap(map));
    }

    LoadedMap::~LoadedMap() = default;

    PlayFreeing::PlayFreeing()
    {
        engine::initialize(dx::test::gameData());
        gc::libraryReported("render");
        gc::libraryReported("game");
    }

    PlayFreeing::~PlayFreeing()
    {
        gc::forgetLibraryReported("game");
    }

    void PlayFreeing::requireReported()
    {
        if (gc::everythingReported()) {
            return;
        }
        std::string types;
        for (const std::string& type : gc::unreportedTypes()) {
            types += " " + type;
        }
        SKIP("an earlier test in this process left objects that do not report their references, so nothing may be freed:" + types);
    }

    LevelChanges::LevelChanges()
    {
        engine::initialize(dx::test::gameData());
    }

    LevelChanges::~LevelChanges()
    {
        close();
    }

    bool LevelChanges::open(const std::string& url, bool carryItems)
    {
        PendingTravel travel;
        std::vector<std::pair<std::string, std::string>> travelInfo;
        if (carryItems && world) {
            travel = world->collectTravel();
            if (UObject* player = world->player()) {
                UObject* replication = pawn::PlayerReplicationInfo(player);
                std::string key = replication ? playerreplicationinfo::PlayerName(replication) : std::string("Player");
                travelInfo.emplace_back(key, world->travelString(player));
            }
        }
        if (world) {
            savegame::storeDepartingLevel(*world, url);
            savegame::selectLevel(url);
        }
        leave();
        world = std::make_unique<World>();
        world->incomingTravel = travel.take();
        world->arrivalTravelInfo = std::move(travelInfo);
        world->replaceTravelInfo = true;
        if (!world->loadMap(url)) {
            return false;
        }
        collect();
        return true;
    }

    bool LevelChanges::load(int32_t index)
    {
        std::string map;
        if (!savegame::prepareLoad(index, map)) {
            return false;
        }
        std::string target = map + "?loadgame=" + std::to_string(index);
        savegame::selectLevel(target);
        leave();
        world = std::make_unique<World>();
        if (!world->loadMap(target)) {
            return false;
        }
        collect();
        world->url = world->level->url;
        return true;
    }

    void LevelChanges::close()
    {
        leave();
    }

    void LevelChanges::leave()
    {
        extension::reset();
        if (world && world->level) {
            objects::unloadPackage(world->level->outermost()->name);
        }
        world.reset();
    }

    // What the game does once the new level is up.
    void LevelChanges::collect()
    {
        World::releaseTravel(world->incomingTravel);
        released = levelrelease::collect(world.get());
    }

    OfflineAudio::OfflineAudio(uint32_t sampleRate)
    {
        engine::initialize(dx::test::gameData());
        audio::Volumes before = audio::volumes();
        sound = before.sound;
        music = before.music;
        speech = before.speech;
        cacheLimit = audio::cacheStats().limitBytes;
        REQUIRE(audio::initializeOffline(sampleRate));
        audio::setVolumes(1.0f, 1.0f, 1.0f);
    }

    OfflineAudio::~OfflineAudio()
    {
        shutdown();
    }

    void OfflineAudio::shutdown()
    {
        if (!active) {
            return;
        }
        active = false;
        audio::shutdown();
        audio::setVolumes(sound, music, speech);
        audio::setSoundCacheLimit(cacheLimit);
    }
}

CATCH_REGISTER_LISTENER(dx::test::UndoAtTestEnd)

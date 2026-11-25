#pragma once

#include "core/config.h"
#include "engine/level_release.h"
#include "engine/world.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Set-up shared by the unit tests. Engine state is process-wide, so each fixture puts back what
// it changed when it goes out of scope, and tests do not depend on the order they run in.
namespace dx::test {
    // Runs undo when the current test case ends, whether it passed or not, latest first. For
    // helpers that change process-wide state and return a plain value, so the test has no
    // fixture to hold.
    void atTestEnd(std::function<void()> undo);

    // A folder of its own under the system temp folder, removed with everything in it at the end.
    // The name carries the label, the process id and a counter, so tests running at the same time
    // in other processes, or one after another in this one, never share it.
    class TempDir {
    public:
        explicit TempDir(std::string_view label);
        ~TempDir();
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const std::filesystem::path& path() const
        {
            return directory;
        }

        std::filesystem::path operator/(const std::filesystem::path& child) const
        {
            return directory / child;
        }

    private:
        std::filesystem::path directory;
    };

    // Saved games go to a fresh temp folder until this goes, then back to where they went before.
    class SaveRoot {
    public:
        explicit SaveRoot(std::string_view label = "saves");
        ~SaveRoot();
        SaveRoot(const SaveRoot&) = delete;
        SaveRoot& operator=(const SaveRoot&) = delete;

        const std::filesystem::path& path() const
        {
            return folder.path();
        }

    private:
        TempDir folder;
        std::filesystem::path previous;
    };

    // Keeps a copy of the system and user ini settings and puts it back at the end, for tests
    // that change settings such as Engine.Engine Root.
    class ConfigSnapshot {
    public:
        ConfigSnapshot();
        ~ConfigSnapshot();
        ConfigSnapshot(const ConfigSnapshot&) = delete;
        ConfigSnapshot& operator=(const ConfigSnapshot&) = delete;

    private:
        Ini system;
        Ini user;
    };

    // Editor mode on until this goes, then back to what it was.
    class EditorMode {
    public:
        EditorMode();
        ~EditorMode();
        EditorMode(const EditorMode&) = delete;
        EditorMode& operator=(const EditorMode&) = delete;

    private:
        bool previous;
    };

    // For the [editor-scripts] tests: the engine started in editor mode, which keeps the script
    // text, and editor mode on until this goes. The engine starts once per process and keeps the
    // mode it started in, so these tests and the game tests cannot share a process: ctest runs
    // them in an entry of their own.
    class ScriptEditing {
    public:
        ScriptEditing();

    private:
        EditorMode editor;
    };

    // Unloads a package, and its saved-game copy, now and again at the end.
    class PackageGuard {
    public:
        explicit PackageGuard(std::string name);
        ~PackageGuard();
        PackageGuard(const PackageGuard&) = delete;
        PackageGuard& operator=(const PackageGuard&) = delete;

    private:
        void unload() const;
        std::string package;
    };

    // The engine initialised and a map loaded into a World, with the map's package unloaded
    // before and after. The load is REQUIREd; the package is unloaded even when it fails.
    class LoadedMap {
    public:
        explicit LoadedMap(std::string map);
        ~LoadedMap();
        LoadedMap(const LoadedMap&) = delete;
        LoadedMap& operator=(const LoadedMap&) = delete;

        UObject* player() const
        {
            return world.player();
        }

    private:
        PackageGuard guard;

    public:
        World world;
    };

    // Freeing as the game does it. The game and render libraries report their references when
    // the game starts and the renderer is made, neither of which a unit test does; this says
    // they report for the length of a test and takes the game's back afterwards, so the tests
    // after it run as before. With every library reporting, destroyed actors are deleted during
    // play and a level change runs the full collection.
    class PlayFreeing {
    public:
        PlayFreeing();
        ~PlayFreeing();
        PlayFreeing(const PlayFreeing&) = delete;
        PlayFreeing& operator=(const PlayFreeing&) = delete;

        // Skips the test when some live object does not report its references, so nothing may
        // be freed. That only happens in a run of several test files in one process, after a
        // test that left behind classes the script compiler made or window classes of its own;
        // the per-file ctest run never skips.
        static void requireReported();
    };

    // Changes levels in the order Game::openMap does, without a window, renderer or gamepad:
    // the travel data is collected, the level being left is stored, the new one is loaded and
    // then the old one is collected. Saved levels go wherever the save root is set.
    class LevelChanges {
    public:
        LevelChanges();
        ~LevelChanges();
        LevelChanges(const LevelChanges&) = delete;
        LevelChanges& operator=(const LevelChanges&) = delete;

        // Travels to a map, carrying the player's items when asked as a travel with them does.
        bool open(const std::string& url, bool carryItems);
        // Loads a saved game, as a quickload does.
        bool load(int32_t index);
        // Leaves the level in play with nothing loaded after it.
        void close();

        std::unique_ptr<World> world;
        // What the collection after the last change did.
        levelrelease::Result released;

    private:
        void leave();
        void collect();
    };

    // Offline audio started at a sample rate with every volume at full. At the end it shuts
    // down, which drops the decoded-sound cache keyed on USound pointers, and the volumes and
    // the cache limit go back to what they were. Declare the USound and UMusic objects it plays
    // before it, so it shuts down while they still exist, or call shutdown() first.
    class OfflineAudio {
    public:
        explicit OfflineAudio(uint32_t sampleRate = 44100);
        ~OfflineAudio();
        OfflineAudio(const OfflineAudio&) = delete;
        OfflineAudio& operator=(const OfflineAudio&) = delete;

        // Shuts audio down and restores the volumes and cache limit now; the destructor then
        // does nothing more.
        void shutdown();

    private:
        bool active = true;
        float sound;
        float music;
        float speech;
        uint64_t cacheLimit;
    };
}

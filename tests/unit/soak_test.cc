// Memory soak scenarios. Hidden from the normal run by the "[.soak]" tag; run one at a
// time, each in its own process, with for example
//     unit-tests "[soak-travel]"
// Every sample is printed as a "SOAK," CSV line and each scenario ends with "SUMMARY"
// lines. SOAK_ITERATIONS and SOAK_MINUTES change the length of a run.
// See tests/soak/README.md.
#include "audio/audio.h"
#include "test_paths.h"
#include "core/gc.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/level_release.h"
#include "engine/actor.h"
#include "engine/ai.h"
#include "engine/console.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "fixtures.h"
#include "soak_memory.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr float TickSeconds = 1.0f / 30.0f;
    constexpr double MiB = 1024.0 * 1024.0;

    int envInt(const char* name, int fallback)
    {
        const char* value = std::getenv(name);
        return value && *value ? std::atoi(value) : fallback;
    }

    struct Counts {
        uint64_t privateBytes = 0;
        size_t tableSize = 0;
        size_t objects = 0;
        size_t destroyed = 0;
        uint64_t dataBytes = 0;
        uint64_t destroyedDataBytes = 0;
        size_t actors = 0;
        uint64_t destroyCount = 0;
        uint64_t linkerBytes = 0;
        uint64_t retiredBytes = 0;
        size_t retired = 0;
        uint64_t soundBytes = 0;
        size_t sounds = 0;
        uint64_t heapBytes = 0;
    };

    Counts measure(const dx::World* world)
    {
        Counts counts;
        const std::vector<dx::UObject*>& all = dx::objects::all();
        counts.tableSize = all.size();
        for (const dx::UObject* object : all) {
            if (!object) {
                continue;
            }
            counts.objects++;
            uint64_t size = object->data && object->cls ? object->cls->size : 0;
            counts.dataBytes += size;
            if (object->destroyed) {
                counts.destroyed++;
                counts.destroyedDataBytes += size;
            }
        }
        if (world) {
            for (const dx::UObject* a : world->actors) {
                if (a && !a->destroyed) {
                    counts.actors++;
                }
            }
        }
        counts.destroyCount = dx::objects::destroyCount();
        dx::objects::LinkerStats linkers = dx::objects::linkerStats();
        counts.linkerBytes = linkers.loadedBytes;
        counts.retiredBytes = linkers.retiredBytes;
        counts.retired = linkers.retired;
        dx::audio::CacheStats sounds = dx::audio::cacheStats();
        counts.soundBytes = sounds.decodedBytes;
        counts.sounds = sounds.sounds;
        counts.privateBytes = soak::privateBytes();
        counts.heapBytes = soak::heapAllocatedBytes();
        return counts;
    }

    void printSample(const char* scenario, int iteration, const std::string& label, double gameSeconds, const Counts& c)
    {
        std::printf("SOAK,%s,%d,%s,%.1f,%.3f,%zu,%zu,%zu,%.3f,%.3f,%zu,%llu,%.3f,%zu,%.3f,%.3f,%zu,%.3f\n", scenario, iteration, label.c_str(), gameSeconds, static_cast<double>(c.privateBytes) / MiB, c.tableSize, c.objects, c.destroyed, static_cast<double>(c.dataBytes) / MiB, static_cast<double>(c.destroyedDataBytes) / MiB, c.actors, static_cast<unsigned long long>(c.destroyCount), static_cast<double>(c.linkerBytes) / MiB, c.retired, static_cast<double>(c.retiredBytes) / MiB, static_cast<double>(c.soundBytes) / MiB, c.sounds, static_cast<double>(c.heapBytes) / MiB);
        std::fflush(stdout);
    }

    void printHeader()
    {
        std::printf("SOAK,scenario,iteration,label,game_s,private_mib,table,objects,destroyed,data_mib,destroyed_data_mib,actors,destroy_count,linker_mib,retired,retired_mib,sound_mib,sounds,heap_mib\n");
    }

    struct Spread {
        double mean = 0;
        double sd = 0;
        double min = 0;
        double max = 0;
        size_t n = 0;
    };

    Spread spread(const std::vector<double>& values)
    {
        Spread s;
        s.n = values.size();
        if (values.empty()) {
            return s;
        }
        s.min = *std::min_element(values.begin(), values.end());
        s.max = *std::max_element(values.begin(), values.end());
        for (double v : values) {
            s.mean += v;
        }
        s.mean /= static_cast<double>(values.size());
        for (double v : values) {
            s.sd += (v - s.mean) * (v - s.mean);
        }
        s.sd = values.size() > 1 ? std::sqrt(s.sd / static_cast<double>(values.size() - 1)) : 0;
        return s;
    }

    // Least-squares slope of y against x.
    double slope(const std::vector<double>& x, const std::vector<double>& y)
    {
        size_t n = std::min(x.size(), y.size());
        if (n < 2) {
            return 0;
        }
        double mx = 0;
        double my = 0;
        for (size_t i = 0; i < n; i++) {
            mx += x[i];
            my += y[i];
        }
        mx /= static_cast<double>(n);
        my /= static_cast<double>(n);
        double num = 0;
        double den = 0;
        for (size_t i = 0; i < n; i++) {
            num += (x[i] - mx) * (y[i] - my);
            den += (x[i] - mx) * (x[i] - mx);
        }
        return den > 0 ? num / den : 0;
    }

    void summary(const char* scenario, const char* quantity, const std::vector<double>& values, const char* unit)
    {
        Spread s = spread(values);
        std::printf("SUMMARY,%s,%s,mean=%.4f,sd=%.4f,min=%.4f,max=%.4f,n=%zu,%s\n", scenario, quantity, s.mean, s.sd, s.min, s.max, s.n, unit);
        std::fflush(stdout);
    }

    void note(const char* scenario, const std::string& text)
    {
        std::printf("SUMMARY,%s,%s\n", scenario, text.c_str());
        std::fflush(stdout);
    }

    // Counts of objects created since a table index, by class, largest data first.
    void classHistogram(const char* scenario, size_t fromIndex, size_t top)
    {
        struct Entry {
            size_t count = 0;
            size_t destroyed = 0;
            uint64_t bytes = 0;
        };

        std::map<std::string, Entry> byClass;
        const std::vector<dx::UObject*>& all = dx::objects::all();
        for (size_t i = fromIndex; i < all.size(); i++) {
            const dx::UObject* object = all[i];
            if (!object || !object->cls) {
                continue;
            }
            Entry& entry = byClass[object->cls->name.str()];
            entry.count++;
            entry.destroyed += object->destroyed ? 1u : 0u;
            entry.bytes += object->data ? object->cls->size : 0;
        }
        std::vector<std::pair<std::string, Entry>> sorted(byClass.begin(), byClass.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.second.bytes > b.second.bytes;
        });
        for (size_t i = 0; i < sorted.size() && i < top; i++) {
            std::printf("CLASSES,%s,%s,count=%zu,destroyed=%zu,data_kib=%.1f\n", scenario, sorted[i].first.c_str(), sorted[i].second.count, sorted[i].second.destroyed, static_cast<double>(sorted[i].second.bytes) / 1024.0);
        }
        std::fflush(stdout);
    }

    uint64_t fileBytes(const std::filesystem::path& path)
    {
        std::error_code error;
        uintmax_t size = std::filesystem::file_size(path, error);
        return error ? 0 : static_cast<uint64_t>(size);
    }

    // Drives levels the way the game's openMap does, without a renderer, UI or audio, and adds
    // up what each collection freed.
    class Session : public dx::test::LevelChanges {
    public:
        explicit Session(const char* saveRootName)
        {
            // The game and the renderer report their references when the game starts; with them,
            // the collector frees during play as the game does.
            dx::gc::libraryReported("render");
            dx::gc::libraryReported("game");
            saves.emplace(saveRootName);
            dx::savegame::deleteCurrent();
        }

        ~Session()
        {
            close();
            saves.reset();
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        bool open(const std::string& url, bool carryItems)
        {
            if (!LevelChanges::open(url, carryItems)) {
                return false;
            }
            record();
            return true;
        }

        bool load(int32_t index)
        {
            if (!LevelChanges::load(index)) {
                return false;
            }
            record();
            return true;
        }

        // Ticks for a stretch of game time, returning the mean real milliseconds per tick.
        double run(double seconds)
        {
            int ticks = std::max(1, static_cast<int>(std::lround(seconds / static_cast<double>(TickSeconds))));
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < ticks; i++) {
                world->tick(TickSeconds);
                gameSeconds += static_cast<double>(TickSeconds);
            }
            std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
            return elapsed.count() / ticks;
        }

        Counts sample(const char* scenario, int iteration, const std::string& label) const
        {
            Counts c = measure(world.get());
            printSample(scenario, iteration, label, gameSeconds, c);
            return c;
        }

        double gameSeconds = 0;
        // What every collection so far freed, added up.
        dx::levelrelease::Result total;

    private:
        std::optional<dx::test::SaveRoot> saves;

        void record()
        {
            const dx::levelrelease::Result& result = released;
            total.packages += result.packages;
            total.freed += result.freed;
            total.kept += result.kept;
            total.cleared += result.cleared;
            total.pinned += result.pinned;
            std::printf("RELEASE,packages=%zu,freed=%zu,kept=%zu,cleared=%zu,pinned=%zu\n", result.packages, result.freed, result.kept, result.cleared, result.pinned);
        }
    };

    std::filesystem::path mapFile(const char* map)
    {
        return std::filesystem::path(dx::test::gameData()) / "Maps" / (std::string(map) + ".dx");
    }

    void travelLoop(const char* scenario, const char* first, const char* second)
    {
        int iterations = envInt("SOAK_ITERATIONS", 12);
        double stay = envInt("SOAK_STAY_SECONDS", 10);
        printHeader();
        Session session(scenario);
        Counts start = measure(nullptr);
        printSample(scenario, 0, "init", 0, start);
        REQUIRE(session.open(first, false));
        session.run(stay);
        Counts previous = session.sample(scenario, 0, std::string("at_") + first);
        size_t tableMark = 0;
        std::vector<double> toSecondMib;
        std::vector<double> toFirstMib;
        std::vector<double> toSecondObjects;
        std::vector<double> toFirstObjects;
        std::vector<double> loopMib;
        std::vector<double> loopObjects;
        std::vector<double> loopData;
        std::vector<double> loopRetired;
        std::vector<double> loadMs;
        for (int i = 1; i <= iterations; i++) {
            Counts loopStart = previous;
            const char* targets[2] = { second, first };
            for (const char* target : targets) {
                auto t0 = std::chrono::steady_clock::now();
                REQUIRE(session.open(target, true));
                std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - t0;
                loadMs.push_back(took.count());
                session.run(stay);
                Counts now = session.sample(scenario, i, std::string("at_") + target);
                double mib = (static_cast<double>(now.privateBytes) - static_cast<double>(previous.privateBytes)) / MiB;
                double objects = static_cast<double>(now.tableSize) - static_cast<double>(previous.tableSize);
                // The first loop loads each level once from the map file and stores it; later
                // loops revisit the stored levels, as replaying a hub does.
                if (i > 1) {
                    (target == second ? toSecondMib : toFirstMib).push_back(mib);
                    (target == second ? toSecondObjects : toFirstObjects).push_back(objects);
                }
                previous = now;
            }
            if (i > 1) {
                loopMib.push_back((static_cast<double>(previous.privateBytes) - static_cast<double>(loopStart.privateBytes)) / MiB);
                loopObjects.push_back(static_cast<double>(previous.tableSize) - static_cast<double>(loopStart.tableSize));
                loopData.push_back((static_cast<double>(previous.dataBytes) - static_cast<double>(loopStart.dataBytes)) / MiB);
                loopRetired.push_back((static_cast<double>(previous.retiredBytes) - static_cast<double>(loopStart.retiredBytes)) / MiB);
            } else {
                tableMark = previous.tableSize;
            }
        }
        std::string arriveSecond = std::string("arrive_") + second + "_private";
        std::string arriveFirst = std::string("arrive_") + first + "_private";
        summary(scenario, arriveSecond.c_str(), toSecondMib, "MiB per transition");
        summary(scenario, arriveFirst.c_str(), toFirstMib, "MiB per transition");
        summary(scenario, (std::string("arrive_") + second + "_objects").c_str(), toSecondObjects, "objects per transition");
        summary(scenario, (std::string("arrive_") + first + "_objects").c_str(), toFirstObjects, "objects per transition");
        summary(scenario, "round_trip_private", loopMib, "MiB per round trip");
        summary(scenario, "round_trip_objects", loopObjects, "objects per round trip");
        summary(scenario, "round_trip_object_data", loopData, "MiB of object data per round trip");
        summary(scenario, "round_trip_retired_linkers", loopRetired, "MiB of package images retired per round trip");
        summary(scenario, "transition_wall_ms", loadMs, "ms");
        note(scenario, std::string("map_file_mib,") + first + "=" + std::to_string(static_cast<double>(fileBytes(mapFile(first))) / MiB) + "," + second + "=" + std::to_string(static_cast<double>(fileBytes(mapFile(second))) / MiB));
        uint64_t stored = 0;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(dx::savegame::currentDirectory(), error)) {
            stored += fileBytes(entry.path());
        }
        note(scenario, "stored_levels_mib=" + std::to_string(static_cast<double>(stored) / MiB));
        note(scenario, "total_private_growth_mib=" + std::to_string((static_cast<double>(previous.privateBytes) - static_cast<double>(start.privateBytes)) / MiB));
        classHistogram(scenario, tableMark, 25);
    }

    dx::UObject* findInventory(dx::UObject* owner, const char* className)
    {
        int guard = 0;
        for (dx::UObject* item = dx::actor::Inventory(owner); item && guard < 64; item = dx::actor::Inventory(item), guard++) {
            if (item->isA(dx::Name(className))) {
                return item;
            }
        }
        return nullptr;
    }

    int32_t* intProperty(dx::UObject* object, const char* name)
    {
        uint8_t* address = object ? object->propertyAddress(dx::Name(name)) : nullptr;
        return reinterpret_cast<int32_t*>(address);
    }

    void minuteSamples(Session& session, const char* scenario, int minutes, std::vector<double>& minuteMib, std::vector<double>& minuteObjects, std::vector<double>& minuteDestroyed, std::vector<double>& tickMs, const std::function<void()>& perTick)
    {
        Counts previous = session.sample(scenario, 0, "start");
        for (int minute = 1; minute <= minutes; minute++) {
            auto t0 = std::chrono::steady_clock::now();
            int ticks = static_cast<int>(std::lround(60.0 / static_cast<double>(TickSeconds)));
            for (int i = 0; i < ticks; i++) {
                if (perTick) {
                    perTick();
                }
                session.world->tick(TickSeconds);
                session.gameSeconds += static_cast<double>(TickSeconds);
            }
            std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - t0;
            tickMs.push_back(took.count() / ticks);
            Counts now = session.sample(scenario, minute, "minute");
            std::printf("TICK,%s,%d,%.4f\n", scenario, minute, took.count() / ticks);
            minuteMib.push_back((static_cast<double>(now.privateBytes) - static_cast<double>(previous.privateBytes)) / MiB);
            minuteObjects.push_back(static_cast<double>(now.tableSize) - static_cast<double>(previous.tableSize));
            minuteDestroyed.push_back(static_cast<double>(now.destroyCount) - static_cast<double>(previous.destroyCount));
            previous = now;
        }
    }

    void tickTrend(const char* scenario, const std::vector<double>& tickMs)
    {
        if (tickMs.size() < 4) {
            return;
        }
        std::vector<double> x;
        for (size_t i = 0; i < tickMs.size(); i++) {
            x.push_back(static_cast<double>(i + 1));
        }
        size_t quarter = std::max<size_t>(1, tickMs.size() / 4);
        std::vector<double> first(tickMs.begin(), tickMs.begin() + static_cast<std::ptrdiff_t>(quarter));
        std::vector<double> last(tickMs.end() - static_cast<std::ptrdiff_t>(quarter), tickMs.end());
        char text[256];
        std::snprintf(text, sizeof(text), "tick_ms,first_quarter=%.4f,last_quarter=%.4f,slope_per_minute=%.5f", spread(first).mean, spread(last).mean, slope(x, tickMs));
        note(scenario, text);
    }
}

TEST_CASE("soak: travel between UNATCO Island and UNATCO HQ", "[.soak][soak-travel]")
{
    travelLoop("travel_unatco", "01_NYC_UNATCOIsland", "01_NYC_UNATCOHQ");
}

TEST_CASE("soak: travel between the NYC street and the bar", "[.soak][soak-travel-small]")
{
    travelLoop("travel_street_bar", "02_NYC_Street", "02_NYC_Bar");
}

TEST_CASE("soak: quicksave, quickload and the save list", "[.soak][soak-save]")
{
    const char* scenario = "save";
    const char* map = "01_NYC_UNATCOIsland";
    int iterations = envInt("SOAK_ITERATIONS", 15);
    printHeader();
    Session session(scenario);
    REQUIRE(session.open(map, false));
    session.run(5);
    Counts previous = session.sample(scenario, 0, "loaded");

    // Quicksave.
    std::vector<double> saveMib;
    std::vector<double> saveObjects;
    for (int i = 1; i <= iterations; i++) {
        REQUIRE(dx::savegame::saveGame(*session.world, dx::savegame::QuickSaveIndex, "Quick Save"));
        session.run(1);
        Counts now = session.sample(scenario, i, "quicksave");
        if (i > 1) {
            saveMib.push_back((static_cast<double>(now.privateBytes) - static_cast<double>(previous.privateBytes)) / MiB);
            saveObjects.push_back(static_cast<double>(now.tableSize) - static_cast<double>(previous.tableSize));
        }
        previous = now;
    }
    summary(scenario, "quicksave_private", saveMib, "MiB per quicksave");
    summary(scenario, "quicksave_objects", saveObjects, "objects per quicksave");
    note(scenario, "quicksave_slot_mib=" + std::to_string(static_cast<double>(dx::savegame::slotSize(dx::savegame::QuickSaveIndex)) / MiB));

    // Quickload.
    std::vector<double> loadMib;
    std::vector<double> loadObjects;
    std::vector<double> loadRetired;
    std::vector<double> loadData;
    size_t loadMark = previous.tableSize;
    for (int i = 1; i <= iterations; i++) {
        REQUIRE(session.load(dx::savegame::QuickSaveIndex));
        session.run(1);
        Counts now = session.sample(scenario, i, "quickload");
        if (i > 1) {
            loadMib.push_back((static_cast<double>(now.privateBytes) - static_cast<double>(previous.privateBytes)) / MiB);
            loadObjects.push_back(static_cast<double>(now.tableSize) - static_cast<double>(previous.tableSize));
            loadRetired.push_back((static_cast<double>(now.retiredBytes) - static_cast<double>(previous.retiredBytes)) / MiB);
            loadData.push_back((static_cast<double>(now.dataBytes) - static_cast<double>(previous.dataBytes)) / MiB);
        }
        previous = now;
    }
    summary(scenario, "quickload_private", loadMib, "MiB per quickload");
    summary(scenario, "quickload_objects", loadObjects, "objects per quickload");
    summary(scenario, "quickload_retired_linkers", loadRetired, "MiB of package images retired per quickload");
    summary(scenario, "quickload_object_data", loadData, "MiB of object data per quickload");
    classHistogram(scenario, loadMark, 10);

    // The save and load menus read every slot's SaveInfo each time they list them.
    int slots = envInt("SOAK_SLOTS", 8);
    // Slot 0 asks for a new slot number, so number them from 1.
    for (int slot = 1; slot <= slots; slot++) {
        REQUIRE(dx::savegame::saveGame(*session.world, slot, "slot " + std::to_string(slot)));
    }
    previous = session.sample(scenario, 0, "slots_written");
    std::vector<double> listMib;
    std::vector<double> listObjects;
    // Each listing retains little, so take many to see it in private bytes.
    int listings = iterations * 10;
    Counts listStart = previous;
    for (int i = 1; i <= listings; i++) {
        int read = 0;
        std::string listed;
        for (int32_t index : dx::savegame::slotIndices()) {
            bool ok = dx::savegame::readSaveInfo(index) != nullptr;
            read += ok ? 1 : 0;
            listed += " " + std::to_string(index) + (ok ? "" : "!");
        }
        INFO("slots" << listed);
        REQUIRE(read >= slots);
        Counts now = session.sample(scenario, i, "list_" + std::to_string(read));
        if (i > 1) {
            listMib.push_back((static_cast<double>(now.privateBytes) - static_cast<double>(previous.privateBytes)) / MiB);
            listObjects.push_back(static_cast<double>(now.tableSize) - static_cast<double>(previous.tableSize));
        }
        previous = now;
    }
    summary(scenario, "list_private", listMib, "MiB per listing");
    note(scenario, "list_average_kib=" + std::to_string((static_cast<double>(previous.privateBytes) - static_cast<double>(listStart.privateBytes)) / 1024.0 / listings) + ",saveinfo_file_kib=" + std::to_string(static_cast<double>(fileBytes(dx::savegame::slotDirectory(1) / "SaveInfo.dxs")) / 1024.0));
    summary(scenario, "list_objects", listObjects, "objects per listing");
    note(scenario, "slots_listed=" + std::to_string(dx::savegame::slotIndices().size()));
}

TEST_CASE("soak: stand on a busy level with its AI running", "[.soak][soak-idle]")
{
    const char* scenario = "idle";
    std::string map = "06_HongKong_WanChai_Market";
    if (const char* chosen = std::getenv("SOAK_MAP")) {
        map = chosen;
    }
    int minutes = envInt("SOAK_MINUTES", 10);
    printHeader();
    Session session(scenario);
    REQUIRE(session.open(map, false));
    // Warm up: first ticks run BeginPlay chains, conversations and first-use loads.
    session.run(10);
    size_t mark = dx::objects::all().size();
    std::vector<double> minuteMib;
    std::vector<double> minuteObjects;
    std::vector<double> minuteDestroyed;
    std::vector<double> tickMs;
    minuteSamples(session, scenario, minutes, minuteMib, minuteObjects, minuteDestroyed, tickMs, nullptr);
    note(scenario, "map=" + map);
    summary(scenario, "minute_private", minuteMib, "MiB per game minute");
    summary(scenario, "minute_objects", minuteObjects, "objects per game minute");
    summary(scenario, "minute_destroyed", minuteDestroyed, "actors destroyed per game minute");
    tickTrend(scenario, tickMs);
    dx::UObject* player = session.world->player();
    note(scenario, "player_health=" + std::to_string(player ? dx::pawn::Health(player) : -1));
    classHistogram(scenario, mark, 20);
}

TEST_CASE("soak: fire the assault rifle into the floor", "[.soak][soak-fire]")
{
    const char* scenario = "fire";
    std::string map = "01_NYC_UNATCOIsland";
    if (const char* chosen = std::getenv("SOAK_MAP")) {
        map = chosen;
    }
    int minutes = envInt("SOAK_MINUTES", 5);
    printHeader();
    Session session(scenario);
    REQUIRE(session.open(map, false));
    session.run(2);
    dx::UObject* player = session.world->player();
    REQUIRE(player != nullptr);
    std::string loadout = dx::console::execute(player, "loadout");
    INFO(loadout);
    dx::UObject* gun = findInventory(player, "WeaponAssaultGun");
    REQUIRE(gun != nullptr);
    dx::vm::Call(player, dx::Name("PutInHand")).obj(gun).run();
    session.run(3);
    dx::Rotator view = dx::pawn::ViewRotation(player);
    view.pitch = -4000;
    dx::pawn::ViewRotation(player) = view;
    dx::UObject* ammo = dx::weapon::AmmoType(gun);
    REQUIRE(ammo != nullptr);
    int32_t* health = intProperty(player, "Health");
    // A rough count of rounds fired: ammo is topped up every tick, so count the drops.
    uint64_t shots = 0;
    int32_t lastAmmo = dx::ammo::AmmoAmount(ammo);
    // Warm-up minute with the weapon firing, so first-use loads do not count.
    auto perTick = [&]() {
        dx::pawn::bFire(player) = 1;
        dx::pawn::ViewRotation(player).pitch = -4000;
        if (dx::vm::stateName(gun) == dx::Name("Idle")) {
            dx::vm::Call(player, dx::Name("Fire")).f(0).run();
        }
        int32_t amount = dx::ammo::AmmoAmount(ammo);
        if (amount < lastAmmo) {
            shots += static_cast<uint64_t>(lastAmmo - amount);
        }
        dx::ammo::AmmoAmount(ammo) = 200;
        lastAmmo = 200;
        if (health) {
            *health = 100;
        }
    };
    for (int i = 0; i < 30 * 30; i++) {
        perTick();
        session.world->tick(TickSeconds);
        session.gameSeconds += static_cast<double>(TickSeconds);
    }
    size_t mark = dx::objects::all().size();
    uint64_t warmShots = shots;
    Counts before = measure(session.world.get());
    std::vector<double> minuteMib;
    std::vector<double> minuteObjects;
    std::vector<double> minuteDestroyed;
    std::vector<double> tickMs;
    minuteSamples(session, scenario, minutes, minuteMib, minuteObjects, minuteDestroyed, tickMs, perTick);
    Counts after = measure(session.world.get());
    double destroyed = static_cast<double>(after.destroyCount - before.destroyCount);
    double fired = static_cast<double>(shots - warmShots);
    note(scenario, "map=" + map);
    note(scenario, "rounds_fired=" + std::to_string(fired) + ",per_minute=" + std::to_string(fired / minutes));
    note(scenario, "destroyed_actors=" + std::to_string(destroyed) + ",per_round=" + std::to_string(fired > 0 ? destroyed / fired : 0));
    note(scenario, "private_kib_per_destroyed=" + std::to_string(destroyed > 0 ? (static_cast<double>(after.privateBytes) - static_cast<double>(before.privateBytes)) / 1024.0 / destroyed : 0));
    note(scenario, "objects_per_destroyed=" + std::to_string(destroyed > 0 ? (static_cast<double>(after.tableSize) - static_cast<double>(before.tableSize)) / destroyed : 0));
    note(scenario, "data_kib_per_destroyed=" + std::to_string(destroyed > 0 ? (static_cast<double>(after.destroyedDataBytes) - static_cast<double>(before.destroyedDataBytes)) / 1024.0 / destroyed : 0));
    summary(scenario, "minute_private", minuteMib, "MiB per minute of continuous fire");
    summary(scenario, "minute_objects", minuteObjects, "objects per minute of continuous fire");
    summary(scenario, "minute_destroyed", minuteDestroyed, "actors destroyed per minute");
    tickTrend(scenario, tickMs);
    classHistogram(scenario, mark, 20);
}

TEST_CASE("soak: spawn and destroy transient effect classes", "[.soak][soak-churn]")
{
    const char* scenario = "churn";
    int minutes = envInt("SOAK_MINUTES", 5);
    // Actors spawned per second; heavy combat with several shooters.
    int perSecond = envInt("SOAK_RATE", 60);
    printHeader();
    Session session(scenario);
    REQUIRE(session.open("01_NYC_UNATCOIsland", false));
    session.run(2);
    dx::UObject* player = session.world->player();
    REQUIRE(player != nullptr);
    const char* names[] = { "DeusEx.ShellCasing", "DeusEx.BulletHole", "DeusEx.Rockchip", "DeusEx.SmokeTrail", "DeusEx.Spark", "DeusEx.Tracer", "DeusEx.MetalFragment", "DeusEx.WoodFragment", "DeusEx.BloodSpurt", "DeusEx.Dart", "DeusEx.Fireball", "DeusEx.ExplosionLight" };
    std::vector<dx::UClass*> classes;
    std::string found;
    for (const char* name : names) {
        dx::UClass* cls = dx::objects::findClass(name);
        if (cls) {
            classes.push_back(cls);
            found += std::string(" ") + name;
        }
    }
    dx::objects::flushLoads();
    REQUIRE(!classes.empty());
    note(scenario, "classes" + found);
    dx::Vector origin = dx::actor::Location(player) + dx::Vector { 200, 0, 0 };
    std::vector<std::pair<dx::UObject*, double>> alive;
    size_t next = 0;
    double owed = 0;
    auto perTick = [&]() {
        owed += static_cast<double>(perSecond) * static_cast<double>(TickSeconds);
        while (owed >= 1) {
            owed -= 1;
            dx::UClass* cls = classes[next++ % classes.size()];
            dx::UObject* spawned = session.world->spawn(cls, nullptr, dx::Name(), origin, {}, nullptr, true);
            if (spawned) {
                alive.emplace_back(spawned, session.gameSeconds);
            }
        }
        // Destroy what has lived half a second, as short-lived effects do.
        auto keep = std::remove_if(alive.begin(), alive.end(), [&](const std::pair<dx::UObject*, double>& entry) {
            if (entry.first->destroyed) {
                return true;
            }
            if (session.gameSeconds - entry.second >= 0.5) {
                session.world->destroy(entry.first);
                return true;
            }
            return false;
        });
        alive.erase(keep, alive.end());
    };
    for (int i = 0; i < 30 * 30; i++) {
        perTick();
        session.world->tick(TickSeconds);
        session.gameSeconds += static_cast<double>(TickSeconds);
    }
    size_t mark = dx::objects::all().size();
    Counts before = measure(session.world.get());
    std::vector<double> minuteMib;
    std::vector<double> minuteObjects;
    std::vector<double> minuteDestroyed;
    std::vector<double> tickMs;
    minuteSamples(session, scenario, minutes, minuteMib, minuteObjects, minuteDestroyed, tickMs, perTick);
    Counts after = measure(session.world.get());
    double destroyed = static_cast<double>(after.destroyCount - before.destroyCount);
    note(scenario, "rate_per_second=" + std::to_string(perSecond));
    note(scenario, "destroyed_actors=" + std::to_string(destroyed));
    note(scenario, "private_kib_per_destroyed=" + std::to_string(destroyed > 0 ? (static_cast<double>(after.privateBytes) - static_cast<double>(before.privateBytes)) / 1024.0 / destroyed : 0));
    note(scenario, "objects_per_destroyed=" + std::to_string(destroyed > 0 ? (static_cast<double>(after.tableSize) - static_cast<double>(before.tableSize)) / destroyed : 0));
    note(scenario, "data_kib_per_destroyed=" + std::to_string(destroyed > 0 ? (static_cast<double>(after.destroyedDataBytes) - static_cast<double>(before.destroyedDataBytes)) / 1024.0 / destroyed : 0));
    summary(scenario, "minute_private", minuteMib, "MiB per minute");
    summary(scenario, "minute_objects", minuteObjects, "objects per minute");
    tickTrend(scenario, tickMs);
    classHistogram(scenario, mark, 20);
}

TEST_CASE("soak: size of the decoded-sound cache per sound package", "[.soak][soak-sounds]")
{
    // The audio module keeps every sound it has decoded, as float and as 16-bit samples,
    // until shutdown. Decoding a whole package gives the cache size if all of it is played.
    const char* scenario = "sounds";
    std::string list = "DeusExSounds,Ambient,MoverSFX,DeusExConAudioAIBarks,DeusExConAudioMission01,DeusExConAudioMission02,DeusExConAudioHK_Shared";
    if (const char* chosen = std::getenv("SOAK_SOUND_PACKAGES")) {
        list = chosen;
    }
    dx::test::OfflineAudio audio;
    std::stringstream names(list);
    std::string package;
    double totalSource = 0;
    double totalDecoded = 0;
    while (std::getline(names, package, ',')) {
        dx::Linker* linker = dx::objects::loadPackage(dx::Name(package));
        if (!linker) {
            note(scenario, "missing package " + package);
            continue;
        }
        std::vector<dx::USound*> sounds;
        const dx::Package& file = linker->file();
        for (int32_t ref = 1; ref <= static_cast<int32_t>(file.exports().size()); ref++) {
            if (file.classOf(ref) == "Sound") {
                dx::UObject* object = linker->object(ref);
                dx::objects::ensureLoaded(object);
                sounds.push_back(dynamic_cast<dx::USound*>(object));
            }
        }
        dx::objects::flushLoads();
        uint64_t before = soak::privateBytes();
        double source = 0;
        double decoded = 0;
        double seconds = 0;
        size_t compressed = 0;
        for (dx::USound* sound : sounds) {
            if (!sound) {
                continue;
            }
            source += static_cast<double>(sound->bytes.size());
            std::shared_ptr<const dx::audio::DecodedSound> pcm = dx::audio::decode(sound);
            if (pcm) {
                decoded += static_cast<double>(pcm->fixed.size() * sizeof(int16_t));
                seconds += static_cast<double>(pcm->duration());
                compressed += pcm->compressed ? 1u : 0u;
            }
        }
        uint64_t after = soak::privateBytes();
        totalSource += source;
        totalDecoded += decoded;
        char text[512];
        std::snprintf(text, sizeof(text), "package=%s,sounds=%zu,compressed=%zu,audio_minutes=%.1f,file_mib=%.2f,sound_data_mib=%.2f,decoded_mib=%.2f,expansion=%.1f,private_growth_mib=%.2f", package.c_str(), sounds.size(), compressed, seconds / 60.0, static_cast<double>(fileBytes(dx::objects::packageFile(dx::Name(package)))) / MiB, source / MiB, decoded / MiB, source > 0 ? decoded / source : 0, (static_cast<double>(after) - static_cast<double>(before)) / MiB);
        note(scenario, text);
    }
    char text[256];
    std::snprintf(text, sizeof(text), "total,sound_data_mib=%.2f,decoded_mib=%.2f", totalSource / MiB, totalDecoded / MiB);
    note(scenario, text);
}
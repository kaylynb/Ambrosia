#include "test_paths.h"
#include "core/config.h"
#include "core/derived_cache.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/level_geometry.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
    dx::derivedcache::Consumer testConsumer()
    {
        dx::derivedcache::Consumer consumer;
        consumer.name = "UnitTest";
        consumer.extension = ".dxtest";
        consumer.magic = { 'A', 'M', 'B', 'T', 'E', 'S', 'T', '\n' };
        consumer.otherVersion = "other version";
        consumer.otherLibrary = "other library";
        consumer.otherLevel = "other level";
        consumer.otherSettings = { "other table", "other settings" };
        consumer.notOurs = "not ours";
        return consumer;
    }

    std::vector<char> fileBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    void writeBytes(const std::filesystem::path& path, const std::vector<char>& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
}

TEST_CASE("derived data lives in a folder per consumer under the user data folder", "[cache]")
{
    dx::derivedcache::Consumer consumer = testConsumer();
    CHECK(dx::derivedcache::directory(consumer) == dx::config::userDirectory() / "UnitTestCache");
    CHECK(dx::derivedcache::path(consumer, "01_NYC_UNATCOHQ") == dx::config::userDirectory() / "UnitTestCache" / "01_nyc_unatcohq.dxtest");
    CHECK(dx::derivedcache::path(consumer, "Odd name.x").filename() == "odd_name_x.dxtest");
}

TEST_CASE("a derived data file reads back whole for its key and is refused otherwise", "[cache]")
{
    dx::derivedcache::Consumer consumer = testConsumer();
    std::filesystem::path path = dx::derivedcache::path(consumer, "Level");
    dx::derivedcache::Key key { 3, 7, 0x1234, { 0x55, 0x66 } };
    std::vector<uint8_t> payload;
    for (int i = 0; i < 300; i++) {
        payload.push_back(static_cast<uint8_t>(i * 13));
    }
    std::string error;
    REQUIRE(dx::derivedcache::write(consumer, path, key, payload, &error));
    CHECK_FALSE(std::filesystem::exists(path.string() + ".partial"));
    dx::derivedcache::Read read = dx::derivedcache::read(consumer, path, key);
    REQUIRE(read.state == dx::derivedcache::State::Valid);
    CHECK(read.payload == payload);
    CHECK(fileBytes(path).size() == 40 + payload.size() + 8);

    // Each part of the key, with the consumer's reason; the first that differs is given.
    auto staleFor = [&](dx::derivedcache::Key other) {
        dx::derivedcache::Read stale = dx::derivedcache::read(consumer, path, other);
        CHECK(stale.state == dx::derivedcache::State::Stale);
        CHECK(stale.payload.empty());
        return stale.reason;
    };
    dx::derivedcache::Key other = key;
    other.version++;
    CHECK(staleFor(other) == "other version");
    other = key;
    other.library++;
    CHECK(staleFor(other) == "other library");
    other = key;
    other.level++;
    CHECK(staleFor(other) == "other level");
    other = key;
    other.settings[0]++;
    CHECK(staleFor(other) == "other table");
    other = key;
    other.settings[1]++;
    CHECK(staleFor(other) == "other settings");
    other.version++;
    CHECK(staleFor(other) == "other version");

    // Written again, the file is replaced whole.
    payload.resize(10);
    REQUIRE(dx::derivedcache::write(consumer, path, key, payload, nullptr));
    CHECK(dx::derivedcache::read(consumer, path, key).payload == payload);

    std::vector<char> bytes = fileBytes(path);
    std::vector<char> flipped = bytes;
    flipped[45] ^= 0x01;
    writeBytes(path, flipped);
    read = dx::derivedcache::read(consumer, path, key);
    CHECK(read.state == dx::derivedcache::State::Damaged);
    CHECK(read.reason == "its checksum does not match");
    CHECK(read.payload.empty());
    std::vector<char> foreign = bytes;
    foreign[3] = 'X';
    writeBytes(path, foreign);
    CHECK(dx::derivedcache::read(consumer, path, key).reason == "not ours");
    writeBytes(path, std::vector<char>(bytes.begin(), bytes.begin() + 20));
    CHECK(dx::derivedcache::read(consumer, path, key).reason == "it is too short");

    CHECK(dx::derivedcache::invalidate(path));
    CHECK_FALSE(dx::derivedcache::invalidate(path));
    CHECK(dx::derivedcache::read(consumer, path, key).state == dx::derivedcache::State::Missing);
}

TEST_CASE("invalidating a consumer removes its files and nothing else", "[cache]")
{
    dx::derivedcache::Consumer consumer = testConsumer();
    std::vector<uint8_t> payload { 1, 2, 3 };
    for (const char* entry : { "One", "Two", "Three" }) {
        REQUIRE(dx::derivedcache::write(consumer, dx::derivedcache::path(consumer, entry), {}, payload, nullptr));
    }
    std::filesystem::path other = dx::derivedcache::directory(consumer) / "notes.txt";
    writeBytes(other, { 'x' });
    CHECK(dx::derivedcache::invalidateAll(consumer) == 3);
    CHECK(std::filesystem::exists(other));
    CHECK(dx::derivedcache::invalidateAll(consumer) == 0);
    std::filesystem::remove(other);
}

TEST_CASE("a level's content hash is the same each time it loads and differs between levels", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    auto hashOf = [](const char* map) {
        dx::objects::unloadPackage(dx::Name(map));
        uint64_t hash = 0;
        {
            dx::World world;
            REQUIRE(world.loadMap(map));
            hash = dx::levelgeometry::contentHash(dx::levelgeometry::describe(world.level));
            // Where the movers are is not part of it.
            dx::levelgeometry::Level moved = dx::levelgeometry::describe(world.level);
            for (dx::levelgeometry::Mover& mover : moved.movers) {
                mover.transform.location.z += 100;
            }
            CHECK(dx::levelgeometry::contentHash(moved) == hash);
            REQUIRE_FALSE(moved.model.points.empty());
            moved.model.points[0].x += 1;
            CHECK(dx::levelgeometry::contentHash(moved) != hash);
        }
        dx::objects::unloadPackage(dx::Name(map));
        return hash;
    };
    uint64_t hq = hashOf("01_NYC_UNATCOHQ");
    CHECK(hashOf("01_NYC_UNATCOHQ") == hq);
    CHECK(hashOf("06_HongKong_WanChai_Garage") != hq);
}

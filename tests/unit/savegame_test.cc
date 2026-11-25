#include "core/objects.h"
#include "test_paths.h"
#include "core/package_writer.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "core/package.h"
#include "engine/props_deusex.h"
#include "engine/props_engine.h"
#include "engine/props_extension.h"
#include "engine/resources.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
    const char* MapName = "01_NYC_UNATCOIsland";

    dx::UObject* findActor(dx::World& world, const char* name)
    {
        for (dx::UObject* a : world.actors) {
            if (a && !a->destroyed && a->name == dx::Name(name)) {
                return a;
            }
        }
        return nullptr;
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

    bool flagValue(dx::UObject* player, const char* flag)
    {
        dx::UObject* flags = *reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("FlagBase")));
        dx::vm::Call call(flags, dx::Name("GetBool"));
        call.n(dx::Name(flag));
        call.run();
        return *reinterpret_cast<uint32_t*>(call.returnValue()) != 0;
    }

    void unloadMaps()
    {
        dx::objects::unloadPackage(dx::Name(MapName));
        dx::objects::unloadPackage(dx::Name(std::string("SaveGame_") + MapName));
    }
}

TEST_CASE("saved game restores actor, flag, inventory, reference and state values", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-restore");
    std::filesystem::path root = saves.path();
    dx::test::PackageGuard maps(MapName);
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UObject* terrorist = findActor(world, "Terrorist1");
        REQUIRE(terrorist != nullptr);
        dx::actor::Location(terrorist) = { -1234.0f, 567.0f, -99.0f };
        dx::pawn::Health(terrorist) = 42;
        REQUIRE(dx::vm::gotoState(terrorist, dx::Name("Wandering")));
        dx::pawn::Enemy(terrorist) = player;

        dx::UObject* flags = *reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("FlagBase")));
        REQUIRE(flags != nullptr);
        dx::vm::Call setFlag(flags, dx::Name("SetBool"));
        setFlag.n(dx::Name("SaveTestFlag")).b(true).b(true).i(3);
        setFlag.run();
        REQUIRE(flagValue(player, "SaveTestFlag"));

        dx::UClass* pistolClass = dx::objects::findClass("DeusEx.WeaponPistol");
        dx::objects::flushLoads();
        dx::UObject* pistol = world.spawn(pistolClass, nullptr, dx::Name(), dx::actor::Location(player), {}, nullptr, true);
        REQUIRE(pistol != nullptr);
        dx::vm::Call give(pistol, dx::Name("GiveTo"));
        give.obj(player);
        give.run();
        REQUIRE(findInventory(player, "WeaponPistol") != nullptr);

        REQUIRE(dx::savegame::saveGame(world, 7, "unit test"));
        dx::pawn::Health(terrorist) = 5;
        dx::actor::Location(terrorist) = { 0.0f, 0.0f, 0.0f };
        unloadMaps();
    }
    CHECK(std::filesystem::is_regular_file(root / "Save0007" / "SaveInfo.dxs"));
    CHECK(std::filesystem::is_regular_file(root / "Save0007" / "01_NYC_UNATCOIsland.dxs"));

    std::string map;
    REQUIRE(dx::savegame::prepareLoad(7, map));
    CHECK(map == MapName);
    std::string url = map + "?loadgame=7";
    REQUIRE(dx::savegame::selectLevel(url));
    dx::World loaded;
    REQUIRE(loaded.loadMap(url));
    CHECK(loaded.level->outermost()->name == dx::Name("SaveGame_01_NYC_UNATCOIsland"));
    dx::UObject* player = loaded.player();
    REQUIRE(player != nullptr);
    dx::UObject* terrorist = findActor(loaded, "Terrorist1");
    REQUIRE(terrorist != nullptr);
    dx::Vector location = dx::actor::Location(terrorist);
    CHECK(location.x == -1234.0f);
    CHECK(location.y == 567.0f);
    CHECK(location.z == -99.0f);
    CHECK(dx::pawn::Health(terrorist) == 42);
    CHECK(dx::pawn::Enemy(terrorist) == player);
    CHECK(dx::vm::stateName(terrorist) == "Wandering");
    CHECK(flagValue(player, "SaveTestFlag"));
    CHECK_FALSE(flagValue(player, "SomeOtherFlag"));
    dx::UObject* restoredPistol = findInventory(player, "WeaponPistol");
    REQUIRE(restoredPistol != nullptr);
    CHECK(dx::actor::Owner(restoredPistol) == player);
    CHECK(dx::deusexplayer::saveCount(player) == 1);
}

TEST_CASE("game directory enumerates save slots with their save info", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-directory");
    std::filesystem::path root = saves.path();
    dx::test::PackageGuard maps(MapName);
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        REQUIRE(dx::savegame::saveGame(world, 3, "first slot"));
        REQUIRE(dx::savegame::saveGame(world, 0, "second slot"));
        unloadMaps();
    }
    dx::UClass* directoryClass = dx::objects::findClass("DeusEx.GameDirectory");
    dx::objects::flushLoads();
    REQUIRE(directoryClass != nullptr);
    dx::UObject* directory = dx::objects::newObject(directoryClass, dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    {
        dx::vm::Call call(directory, dx::Name("SetDirType"));
        call.byte(1);
        call.run();
    }
    dx::vm::Call(directory, dx::Name("GetGameDirectory")).run();
    dx::vm::Call count(directory, dx::Name("GetDirCount"));
    count.run();
    REQUIRE(*reinterpret_cast<int32_t*>(count.returnValue()) == 2);
    dx::vm::Call filename(directory, dx::Name("GetDirFilename"));
    filename.i(1);
    filename.run();
    CHECK(*reinterpret_cast<std::string*>(filename.returnValue()) == "Save0004");

    dx::vm::Call info(directory, dx::Name("GetSaveInfoFromDirectoryIndex"));
    info.i(0);
    info.run();
    dx::UObject* saveInfo = *reinterpret_cast<dx::UObject**>(info.returnValue());
    REQUIRE(saveInfo != nullptr);
    CHECK(dx::deusexsaveinfo::DirectoryIndex(saveInfo) == 3);
    CHECK(dx::deusexsaveinfo::Description(saveInfo) == "first slot");
    CHECK(dx::deusexsaveinfo::MapName(saveInfo) == "01_NYC_UNATCOIsland");
    CHECK(dx::deusexsaveinfo::MissionLocation(saveInfo) == "NYC - Liberty Island");

    dx::vm::Call quick(directory, dx::Name("GetSaveInfo"));
    quick.i(-1);
    quick.run();
    CHECK(*reinterpret_cast<dx::UObject**>(quick.returnValue()) == nullptr);
}

TEST_CASE("an overwrite that fails part-way leaves the previous save loadable", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-failed-overwrite");
    std::filesystem::path root = saves.path();
    dx::test::PackageGuard maps(MapName);
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        REQUIRE(dx::savegame::saveGame(world, 8, "kept"));
        std::filesystem::create_directories(dx::savegame::currentDirectory() / "Broken.dxs");
        CHECK_FALSE(dx::savegame::saveGame(world, 8, "replacement"));
        unloadMaps();
    }
    CHECK_FALSE(std::filesystem::exists(root / "Save0008.new"));
    CHECK_FALSE(std::filesystem::exists(root / "Save0008.old"));
    dx::UObject* info = dx::savegame::readSaveInfo(8);
    REQUIRE(info != nullptr);
    CHECK(dx::deusexsaveinfo::Description(info) == "kept");
    std::filesystem::remove_all(dx::savegame::currentDirectory() / "Broken.dxs");
    std::string map;
    REQUIRE(dx::savegame::prepareLoad(8, map));
    CHECK(map == MapName);
}

TEST_CASE("a save keeps its snapshot texture in SaveInfo.dxs", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-snapshot");
    dx::test::PackageGuard maps(MapName);
    // P8 with a gray palette, as the original's snapshots are.
    std::vector<uint8_t> pixels(4 * 4);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = static_cast<uint8_t>(i * 5 + 1);
    }
    dx::savegame::setSnapshotSource([&pixels] {
        dx::UObject* transient = dx::objects::packageObject(dx::Name("Transient"));
        auto* palette = dx::cast<dx::UPalette>(dx::objects::newObject(dx::objects::findClass("Engine.Palette"), transient, dx::Name(), dx::RF_Public | dx::RF_Transient));
        for (uint32_t i = 0; i < 256; i++) {
            palette->colors.push_back(i | (i << 8) | (i << 16) | 0xff000000u);
        }
        auto* texture = dx::cast<dx::UTexture>(dx::objects::newObject(dx::objects::findClass("Engine.Texture"), transient, dx::Name(), dx::RF_Public | dx::RF_Transient));
        *texture->propertyAddress(dx::Name("Format")) = 0;
        *reinterpret_cast<dx::UObject**>(texture->propertyAddress(dx::Name("Palette"))) = palette;
        dx::TextureMip mip;
        mip.width = 4;
        mip.height = 4;
        mip.ubits = 2;
        mip.vbits = 2;
        mip.data = pixels;
        texture->mips.push_back(mip);
        return static_cast<dx::UObject*>(texture);
    });
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        REQUIRE(dx::savegame::saveGame(world, 4, "snapshot"));
        unloadMaps();
    }
    dx::savegame::setSnapshotSource(nullptr);
    dx::UObject* info = dx::savegame::readSaveInfo(4);
    REQUIRE(info != nullptr);
    auto* snapshot = dx::cast<dx::UTexture>(dx::deusexsaveinfo::Snapshot(info));
    REQUIRE(snapshot != nullptr);
    dx::objects::ensureLoaded(snapshot);
    CHECK(snapshot->format() == 0);
    REQUIRE(snapshot->mips.size() == 1);
    CHECK(snapshot->width() == 4);
    CHECK(snapshot->height() == 4);
    CHECK(snapshot->mips[0].data == pixels);
    dx::UPalette* palette = snapshot->palette();
    REQUIRE(palette != nullptr);
    REQUIRE(palette->colors.size() == 256);
    CHECK(palette->colors[200] == 0xffc8c8c8u);
    REQUIRE(snapshot->decodable());
    std::vector<uint8_t> rgba = snapshot->rgba(0);
    CHECK(rgba[0] == pixels[0]);
    CHECK(rgba[3] == 255);
}

TEST_CASE("package writer output is read back by the linker", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* infoClass = dx::objects::findClass("DeusEx.DeusExSaveInfo");
    dx::UClass* flagClass = dx::objects::findClass("Extension.FlagBool");
    dx::UObject* texture = dx::objects::loadObject("CoreTexBrick.Brick.rough_greywall", nullptr);
    dx::objects::flushLoads();
    REQUIRE(infoClass != nullptr);
    REQUIRE(flagClass != nullptr);
    REQUIRE(texture != nullptr);
    dx::UObject* transient = dx::objects::packageObject(dx::Name("Transient"));
    dx::UObject* info = dx::objects::newObject(infoClass, transient, dx::Name("RoundTripInfo"), 0);
    dx::UObject* first = dx::objects::newObject(flagClass, transient, dx::Name("RoundTripFirst"), 0);
    dx::UObject* second = dx::objects::newObject(flagClass, transient, dx::Name("RoundTripSecond"), 0);
    dx::deusexsaveinfo::Description(info) = "round trip";
    dx::deusexsaveinfo::saveCount(info) = 31;
    dx::deusexsaveinfo::bCheatsEnabled(info) = 1;
    dx::deusexsaveinfo::Snapshot(info) = texture;
    dx::flag::FlagName(first) = dx::Name("FirstFlag");
    dx::flag::expiration(first) = 12;
    dx::flag::nextFlag(first) = second;
    dx::flag::FlagName(second) = dx::Name("SecondFlag");

    dx::PackageWriter writer(dx::Name("RoundTrip"));
    writer.addExport(info, nullptr);
    writer.addExport(first, nullptr);
    writer.addExport(second, first);
    writer.setFilter([](dx::UObject* object) {
        return object->linker != nullptr || dx::cast<dx::UClass>(object) != nullptr || !object->outer;
    });
    dx::test::TempDir folder("package-writer-round-trip");
    std::filesystem::path file = folder / "RoundTrip.dxs";
    std::string error;
    REQUIRE(writer.save(file, &error));

    dx::Linker* linker = dx::objects::loadPackageFile(dx::Name("RoundTripPackage"), file, true);
    REQUIRE(linker != nullptr);
    int32_t infoRef = linker->findExport(dx::Name("RoundTripInfo"), dx::Name("DeusExSaveInfo"), "");
    int32_t firstRef = linker->findExport(dx::Name("RoundTripFirst"), dx::Name("FlagBool"), "");
    int32_t secondRef = linker->findExport(dx::Name("RoundTripSecond"), dx::Name("FlagBool"), "RoundTripFirst");
    REQUIRE(infoRef == 1);
    REQUIRE(firstRef == 2);
    REQUIRE(secondRef == 3);
    dx::UObject* loadedInfo = linker->object(infoRef);
    dx::UObject* loadedFirst = linker->object(firstRef);
    dx::objects::flushLoads();
    REQUIRE(loadedInfo != info);
    CHECK(dx::deusexsaveinfo::Description(loadedInfo) == "round trip");
    CHECK(dx::deusexsaveinfo::saveCount(loadedInfo) == 31);
    CHECK(dx::deusexsaveinfo::bCheatsEnabled(loadedInfo) == 1);
    CHECK(dx::deusexsaveinfo::Snapshot(loadedInfo) == texture);
    CHECK(dx::flag::FlagName(loadedFirst) == dx::Name("FirstFlag"));
    CHECK(dx::flag::expiration(loadedFirst) == 12);
    dx::UObject* loadedSecond = dx::flag::nextFlag(loadedFirst);
    REQUIRE(loadedSecond != nullptr);
    CHECK(loadedSecond->name == dx::Name("RoundTripSecond"));
    CHECK(loadedSecond->outer == loadedFirst);
    CHECK(dx::flag::FlagName(loadedSecond) == dx::Name("SecondFlag"));
    dx::objects::unloadPackage(dx::Name("RoundTripPackage"));
}

TEST_CASE("a loaded saved game keeps the touching lists of its actors", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-touching");
    std::filesystem::path root = saves.path();
    dx::test::PackageGuard maps(MapName);
    std::string toucherName;
    std::string touchedName;
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        for (int i = 0; i < 60; i++) {
            world.tick(1.0f / 30.0f);
        }
        dx::UObject* player = world.player();
        for (dx::UObject* a : world.actors) {
            if (!toucherName.empty() || !a || a->destroyed || a == player) {
                continue;
            }
            for (int i = 0; i < 4; i++) {
                dx::UObject* other = dx::actor::Touching(a, i);
                if (other && other != player && toucherName.empty()) {
                    toucherName = a->name.str();
                    touchedName = other->name.str();
                }
            }
        }
        REQUIRE_FALSE(toucherName.empty());
        REQUIRE(dx::savegame::saveGame(world, 5, "touching"));
        unloadMaps();
    }
    std::string map;
    REQUIRE(dx::savegame::prepareLoad(5, map));
    std::string url = map + "?loadgame=5";
    REQUIRE(dx::savegame::selectLevel(url));
    dx::World loaded;
    REQUIRE(loaded.loadMap(url));
    dx::UObject* toucher = findActor(loaded, toucherName.c_str());
    REQUIRE(toucher != nullptr);
    bool kept = false;
    for (int i = 0; i < 4; i++) {
        dx::UObject* other = dx::actor::Touching(toucher, i);
        kept = kept || (other && other->name.str() == touchedName);
    }
    CHECK(kept);
}

TEST_CASE("a save referring to an uninstalled actor class skips its instances", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-missing-class");
    dx::test::PackageGuard maps(MapName);
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        REQUIRE(dx::savegame::saveGame(world, 1, "missing theme"));
        unloadMaps();
    }
    std::string map;
    REQUIRE(dx::savegame::prepareLoad(1, map));
    // Change the imported class name in the test copy's name table, leaving the
    // export, its actor slot and the theme manager's references intact.
    auto file = dx::savegame::currentDirectory() / (map + ".dxs");
    {
        std::ifstream in(file, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        in.close();
        std::string name = "ColorThemeHUD_Default";
        size_t at = bytes.find(name + '\0');
        REQUIRE(at != std::string::npos);
        bytes[at] = 'X';
        std::ofstream(file, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    auto url = map + "?loadgame=1";
    REQUIRE(dx::savegame::selectLevel(url));
    {
        dx::World world;
        REQUIRE(world.loadMap(url));
        REQUIRE(world.player());
        for (auto* actor : world.actors) {
            REQUIRE((!actor || actor->isA(dx::Name("Actor"))));
        }
        world.tick(1.0f / 30.0f);
    }
    unloadMaps();
}

TEST_CASE("a loaded saved level keeps the options of its saved URL", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-url");
    std::filesystem::path root = saves.path();
    dx::test::PackageGuard maps(MapName);
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        REQUIRE(world.url == MapName);
        REQUIRE(dx::savegame::saveGame(world, 6, "url"));
        unloadMaps();
    }
    std::string map;
    REQUIRE(dx::savegame::prepareLoad(6, map));
    std::string url = map + "?loadgame=6";
    REQUIRE(dx::savegame::selectLevel(url));
    dx::World loaded;
    REQUIRE(loaded.loadMap(url));
    CHECK(loaded.level->url == "01_NYC_UNATCOIsland?Name=Player?Class=DeusEx.JCDentonMale");
}

TEST_CASE("plane struct values are written and read with their own member W before the inherited X Y Z", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* actorClass = dx::objects::findClass("Engine.Actor");
    REQUIRE(actorClass != nullptr);
    dx::UProperty* simAnim = actorClass->findProperty(dx::Name("SimAnim"));
    REQUIRE(simAnim != nullptr);
    REQUIRE(simAnim->structType->name == dx::Name("Plane"));
    dx::UObject* transient = dx::objects::packageObject(dx::Name("Transient"));
    dx::UObject* actor = dx::objects::newObject(actorClass, transient, dx::Name("PlaneOrderActor"), 0);
    const float values[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    for (int i = 0; i < 4; i++) {
        dx::UProperty* member = simAnim->structType->findProperty(dx::Name(std::string(1, "XYZW"[i])));
        REQUIRE(member != nullptr);
        *reinterpret_cast<float*>(actor->data + simAnim->offset + member->offset) = values[i];
    }

    dx::PackageWriter writer(dx::Name("PlaneOrder"));
    dx::Writer out;
    writer.writeTaggedProperties(out, actorClass, actor->data, actorClass->defaultObject->data);
    const std::vector<uint8_t>& bytes = out.data();
    dx::Writer tag;
    tag.index(writer.nameIndex(dx::Name("SimAnim")));
    tag.u8(0x4a);
    tag.index(writer.nameIndex(dx::Name("Plane")));
    auto at = std::search(bytes.begin(), bytes.end(), tag.data().begin(), tag.data().end());
    REQUIRE(at != bytes.end());
    size_t payload = static_cast<size_t>(at - bytes.begin()) + tag.size();
    REQUIRE(payload + 16 <= bytes.size());
    float written[4] = {};
    std::memcpy(written, bytes.data() + payload, sizeof(written));
    CHECK(written[0] == 4.0f);
    CHECK(written[1] == 1.0f);
    CHECK(written[2] == 2.0f);
    CHECK(written[3] == 3.0f);

    dx::PackageWriter package(dx::Name("PlaneOrder"));
    package.addExport(actor, nullptr);
    package.setFilter([](dx::UObject* object) {
        return object->linker != nullptr || dx::cast<dx::UClass>(object) != nullptr || !object->outer;
    });
    dx::test::TempDir folder("plane-order");
    std::filesystem::path file = folder / "PlaneOrder.dxs";
    std::string error;
    REQUIRE(package.save(file, &error));
    dx::Linker* linker = dx::objects::loadPackageFile(dx::Name("PlaneOrderPackage"), file, true);
    REQUIRE(linker != nullptr);
    dx::UObject* loaded = linker->object(linker->findExport(dx::Name("PlaneOrderActor"), dx::Name("Actor"), ""));
    dx::objects::flushLoads();
    REQUIRE(loaded != nullptr);
    for (int i = 0; i < 4; i++) {
        dx::UProperty* member = simAnim->structType->findProperty(dx::Name(std::string(1, "XYZW"[i])));
        CHECK(*reinterpret_cast<const float*>(loaded->data + simAnim->offset + member->offset) == values[i]);
    }
    dx::objects::unloadPackage(dx::Name("PlaneOrderPackage"));
}

TEST_CASE("save listing and level clean-up fail quietly when the save root is not a directory", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::TempDir folder("savegame-not-a-directory");
    std::filesystem::path root = folder / "root";
    {
        std::ofstream file(root);
        file << "not a directory";
    }
    dx::test::SaveRoot saves;
    dx::savegame::setSaveRoot(root);
    CHECK(dx::savegame::slotIndices().empty());
    CHECK(dx::savegame::newSlotIndex() == 1);
    CHECK(dx::savegame::slotSize(1) == 0);
    CHECK(dx::savegame::readSaveInfo(1) == nullptr);
    std::string map;
    CHECK_FALSE(dx::savegame::prepareLoad(1, map));
    CHECK_FALSE(dx::savegame::selectLevel(MapName));
    dx::savegame::deleteCurrent();
}

namespace {
    int32_t readCompact(const std::vector<uint8_t>& bytes, size_t& at)
    {
        uint8_t first = bytes.at(at++);
        int32_t value = first & 0x3f;
        if (first & 0x40) {
            int shift = 6;
            for (int i = 0; i < 4; i++) {
                uint8_t next = bytes.at(at++);
                value |= static_cast<int32_t>(next & 0x7f) << shift;
                shift += 7;
                if (!(next & 0x80)) {
                    break;
                }
            }
        }
        return (first & 0x80) ? -value : value;
    }

    void writeCompact(std::vector<uint8_t>& out, int32_t value)
    {
        uint32_t magnitude = static_cast<uint32_t>(value < 0 ? -value : value);
        uint8_t first = static_cast<uint8_t>((value < 0 ? 0x80 : 0) | (magnitude & 0x3f));
        magnitude >>= 6;
        if (magnitude) {
            first |= 0x40;
        }
        out.push_back(first);
        while (magnitude) {
            uint8_t next = static_cast<uint8_t>(magnitude & 0x7f);
            magnitude >>= 7;
            if (magnitude) {
                next |= 0x80;
            }
            out.push_back(next);
        }
    }

    void copyBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes, size_t& at, size_t count)
    {
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + count));
        at += count;
    }

    constexpr uint32_t RF_HasStackFlag = 0x02000000;

    // Rewrites the export table, the last thing in a package Ambrosia writes, so that one export's
    // data starts past the end of the file.
    void pointExportPastEnd(const std::filesystem::path& path, int32_t target)
    {
        std::vector<uint8_t> bytes;
        {
            std::ifstream in(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        uint32_t exportCount = 0;
        uint32_t exportOffset = 0;
        std::memcpy(&exportCount, bytes.data() + 20, 4);
        std::memcpy(&exportOffset, bytes.data() + 24, 4);
        std::vector<uint8_t> out(bytes.begin(), bytes.begin() + exportOffset);
        size_t at = exportOffset;
        for (uint32_t i = 1; i <= exportCount; i++) {
            writeCompact(out, readCompact(bytes, at));
            writeCompact(out, readCompact(bytes, at));
            copyBytes(out, bytes, at, 4);
            writeCompact(out, readCompact(bytes, at));
            uint32_t flags = 0;
            std::memcpy(&flags, bytes.data() + at, 4);
            at += 4;
            if (static_cast<int32_t>(i) == target) {
                // With a stack the loader reads before it guards the export's own data.
                flags |= RF_HasStackFlag;
            }
            const auto* flagBytes = reinterpret_cast<const uint8_t*>(&flags);
            out.insert(out.end(), flagBytes, flagBytes + 4);
            int32_t size = readCompact(bytes, at);
            writeCompact(out, size);
            if (size > 0) {
                int32_t offset = readCompact(bytes, at);
                writeCompact(out, static_cast<int32_t>(i) == target ? static_cast<int32_t>(bytes.size()) + 1000 : offset);
            }
        }
        REQUIRE(at == bytes.size());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
}

TEST_CASE("a saved level whose data runs past the end of the file makes loadMap fail and leaves the next map loadable", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("savegame-broken-level");
    std::filesystem::path root = saves.path();
    dx::test::PackageGuard maps(MapName);
    {
        dx::World world;
        REQUIRE(world.loadMap(MapName));
        REQUIRE(dx::savegame::saveGame(world, 9, "broken"));
        unloadMaps();
    }
    std::string map;
    REQUIRE(dx::savegame::prepareLoad(9, map));
    std::filesystem::path level = dx::savegame::currentDirectory() / (map + ".dxs");
    int32_t levelRef = 0;
    {
        dx::Package package(level);
        levelRef = package.findExport("Level", "MyLevel");
        REQUIRE(levelRef > 0);
    }
    pointExportPastEnd(level, levelRef);
    {
        dx::Package package(level);
        REQUIRE(package.exportAt(levelRef).offset > static_cast<int32_t>(package.data().size()));
    }
    std::string url = map + "?loadgame=9";
    REQUIRE(dx::savegame::selectLevel(url));
    {
        dx::World broken;
        CHECK_FALSE(broken.loadMap(url));
    }
    unloadMaps();
    {
        dx::World world;
        CHECK(world.loadMap(MapName));
        CHECK(world.player() != nullptr);
    }
    unloadMaps();
    dx::savegame::deleteCurrent();
}

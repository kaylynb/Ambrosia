#include "core/package.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/package_writer.h"
#include "deusex/startup.h"
#include "engine/reader_values.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

TEST_CASE("compact index decodes single and multi byte values")
{
    std::vector<uint8_t> bytes = { 0x05, 0x85, 0x40, 0x01, 0x7f, 0x7f, 0x40, 0x80, 0x02 };
    dx::Reader reader(bytes, 0);
    CHECK(reader.index() == 5);
    CHECK(reader.index() == -5);
    CHECK(reader.index() == 64);
    CHECK(reader.index() == 8191);
    CHECK(reader.index() == 16384);
    CHECK(reader.tell() == bytes.size());
}

TEST_CASE("reader throws when reading past the end")
{
    std::vector<uint8_t> bytes = { 0x01, 0x02 };
    dx::Reader reader(bytes, 0);
    CHECK_THROWS_AS(reader.u32(), std::runtime_error);
}

TEST_CASE("an array count must fit in the bytes left, and must not be negative")
{
    // 3 elements with 3 bytes after the count: fits.
    std::vector<uint8_t> fits = { 0x03, 0xaa, 0xbb, 0xcc };
    dx::Reader fitting(fits, 0);
    CHECK(dx::readers::readCount(fitting, "test") == 3);

    // 8191 elements claimed (0x7f 0x7f) with 2 bytes left: refused before anything is sized.
    std::vector<uint8_t> corrupt = { 0x7f, 0x7f, 0x00, 0x00 };
    dx::Reader overlong(corrupt, 0);
    CHECK_THROWS_WITH(dx::readers::readCount(overlong, "test"), "array count past the end of test");

    std::vector<uint8_t> negative = { 0x85 };
    dx::Reader backwards(negative, 0);
    CHECK_THROWS_WITH(dx::readers::readCount(backwards), "negative array count");

    // None at the very end is fine.
    std::vector<uint8_t> empty = { 0x00 };
    dx::Reader none(empty, 0);
    CHECK(dx::readers::readCount(none) == 0);
}

TEST_CASE("a string's length is checked against the bytes left, for narrow and wide strings")
{
    // 5 narrow characters claimed, 2 there.
    std::vector<uint8_t> narrow = { 0x05, 'a', 'b' };
    dx::Reader shortNarrow(narrow, 0);
    CHECK_THROWS_AS(shortNarrow.string(), std::runtime_error);

    // 3 wide characters claimed (-3), 1 there.
    std::vector<uint8_t> wide = { 0x83, 'a', 0x00 };
    dx::Reader shortWide(wide, 0);
    CHECK_THROWS_AS(shortWide.string(), std::runtime_error);

    // A narrow string drops its terminator; a wide one with a character past Latin-1 is UTF-8.
    std::vector<uint8_t> whole = { 0x03, 'h', 'i', 0x00, 0x82, 0xac, 0x20, 0x00, 0x00 };
    dx::Reader both(whole, 0);
    CHECK(both.string() == "hi");
    CHECK(both.string() == "\xe2\x82\xac");
    CHECK(both.remaining() == 0);
}

TEST_CASE("package header and tables of Engine.u", "[data]")
{
    dx::Package package(dx::test::gameData() + "/System/Engine.u");
    CHECK(package.version() == 68);
    CHECK(package.licensee() == 0);
    CHECK(package.names().size() == 3574);
    CHECK(package.exports().size() == 5622);
    CHECK(package.imports().size() == 60);
}

TEST_CASE("tagged properties of a texture", "[data]")
{
    dx::Package package(dx::test::gameData() + "/Textures/CoreTexBrick.utx");
    int32_t ref = package.findExport("Texture", "rough_greywall");
    REQUIRE(ref != 0);
    CHECK(package.groupOf(ref) == "Brick");

    dx::Object object = package.load(ref);
    CHECK(object.objectValue("Palette") == 3);
    CHECK(object.byteValue("UBits") == 7);
    CHECK(object.byteValue("VBits") == 7);
    CHECK(object.intValue("USize") == 128);
    CHECK(object.intValue("VSize") == 128);
    REQUIRE(object.find("InternalTime", 1) != nullptr);
    CHECK(object.find("InternalTime", 2) == nullptr);
    CHECK(object.find("MipZero")->structName == "Color");
    CHECK(package.objectName(3) == "Palette123");
    CHECK(package.classOf(3) == "Palette");
}

namespace {
    int32_t findImport(dx::Linker& linker, const std::string& name)
    {
        const dx::Package& file = linker.file();
        for (size_t i = 0; i < file.imports().size(); i++) {
            int32_t ref = -static_cast<int32_t>(i + 1);
            if (file.objectName(ref) == name) {
                return ref;
            }
        }
        return 0;
    }
}

TEST_CASE("imports resolve to exports of other packages", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::Linker* map = dx::objects::loadPackage(dx::Name("01_NYC_UNATCOHQ"));
    REQUIRE(map != nullptr);
    int32_t import = findImport(*map, "bluewater");
    REQUIRE(import != 0);
    CHECK(map->file().groupOf(import) == "water");

    dx::UObject* resolved = map->object(import);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->outermost()->name == dx::Name("Effects"));
    CHECK(resolved->name == dx::Name("bluewater"));
    CHECK(resolved->cls->name == dx::Name("Texture"));
}

TEST_CASE("imports from version 61 packages resolve", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::Linker* map = dx::objects::loadPackage(dx::Name("03_NYC_UNATCOHQ"));
    REQUIRE(map != nullptr);
    int32_t import = findImport(*map, "bluewater");
    REQUIRE(import != 0);
    dx::UObject* resolved = map->object(import);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->outermost()->name == dx::Name("CoreTexWater"));
}

namespace {
    // Compares the bytes the writer produces for a string the reader returned with the bytes it was read from.
    struct StringRoundTrip {
        size_t strings = 0;
        size_t wide = 0;
        size_t narrowHigh = 0;
        std::vector<std::string> mismatches;

        void check(std::span<const uint8_t> data, size_t offset, const std::string& where)
        {
            dx::Reader reader(data, offset);
            std::string value = reader.string();
            size_t end = reader.tell();
            dx::Writer writer;
            writer.string(value);
            std::vector<uint8_t> original(data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(end));
            strings++;
            if (!original.empty() && (original[0] & 0x80)) {
                wide++;
            } else if (std::any_of(value.begin(), value.end(), [](char c) { return (static_cast<unsigned char>(c) & 0x80) != 0; })) {
                narrowHigh++;
            }
            dx::Reader again(writer.data(), 0);
            if (writer.data() != original || again.string() != value) {
                mismatches.push_back(where + ": \"" + value + "\"");
            }
        }
    };
}

TEST_CASE("every name and string property in the shipped packages is written back byte for byte", "[data]")
{
    StringRoundTrip roundTrip;
    size_t files = 0;
    for (const char* folder : { "System", "Textures", "Sounds", "Music", "Maps" }) {
        std::error_code error;
        for (const auto& item : std::filesystem::directory_iterator(std::filesystem::path(dx::test::gameData()) / folder, error)) {
            std::string extension = item.path().extension().string();
            if (extension != ".u" && extension != ".utx" && extension != ".uax" && extension != ".umx" && extension != ".dx") {
                continue;
            }
            dx::Package package(item.path());
            files++;
            std::span<const uint8_t> data = package.data();
            if (package.version() >= 64) {
                dx::Reader header(data, 12);
                uint32_t nameCount = header.u32();
                dx::Reader names(data, header.u32());
                for (uint32_t i = 0; i < nameCount; i++) {
                    roundTrip.check(data, names.tell(), package.name() + " name " + std::to_string(i));
                    names.string();
                    names.u32();
                }
            }
            for (size_t i = 0; i < package.exports().size(); i++) {
                int32_t ref = static_cast<int32_t>(i + 1);
                if (package.exports()[i].size <= 0 || package.classOf(ref) == "Class") {
                    continue;
                }
                std::vector<dx::Property> properties;
                try {
                    properties = package.load(ref).properties();
                } catch (const std::exception&) {
                    continue;
                }
                for (const dx::Property& property : properties) {
                    if (property.type == dx::PropertyType::Str) {
                        roundTrip.check(data, property.offset, package.name() + "." + package.objectName(ref) + "." + property.name);
                    }
                }
            }
        }
    }
    INFO(roundTrip.strings << " strings in " << files << " packages, " << roundTrip.wide << " stored wide, " << roundTrip.narrowHigh << " narrow with bytes above 0x7F");
    for (size_t i = 0; i < std::min<size_t>(roundTrip.mismatches.size(), 20); i++) {
        UNSCOPED_INFO(roundTrip.mismatches[i]);
    }
    std::printf("string round trip: %zu strings in %zu packages, %zu stored wide, %zu narrow with bytes above 0x7F\n", roundTrip.strings, files, roundTrip.wide, roundTrip.narrowHigh);
    CHECK(files > 100);
    CHECK(roundTrip.mismatches.empty());
}
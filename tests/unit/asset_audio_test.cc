#include "compiler/package_dump.h"
#include "test_paths.h"
#include "core/package.h"
#include "ucc_runner.h"

#include <catch_amalgamated.hpp>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
    std::string lowerText(std::string text)
    {
        for (char& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return text;
    }

    std::vector<uint8_t> readFile(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    size_t compactIndex(const std::vector<uint8_t>& bytes, size_t at, int32_t& value)
    {
        uint8_t first = bytes[at++];
        uint32_t result = first & 0x3f;
        if (first & 0x40) {
            int shift = 6;
            while (true) {
                uint8_t next = bytes[at++];
                result |= static_cast<uint32_t>(next & 0x7f) << shift;
                shift += 7;
                if (!(next & 0x80)) {
                    break;
                }
            }
        }
        value = (first & 0x80) ? -static_cast<int32_t>(result) : static_cast<int32_t>(result);
        return at;
    }

}

TEST_CASE("batchexport sound wav writes the stored wave file", "[data]")
{
    // ucc (or the child run) needs the data too; this fails saying it is missing, not with an exit code.
    dx::test::gameData();
    const char* xdg = std::getenv("XDG_DATA_HOME");
    REQUIRE(xdg != nullptr);
    std::filesystem::path home = std::filesystem::path(xdg) / "ucc-audio-export";
    std::filesystem::remove_all(home);
    std::filesystem::path directory = home / "wav";
    REQUIRE(ucctest::runUcc(home, "batchexport DeusExSounds.u sound wav \"" + directory.string() + "\"") == 0);
    std::vector<uint8_t> wave = readFile(directory / "CatDie.wav");
    REQUIRE(wave.size() == 65656);
    CHECK(std::memcmp(wave.data(), "RIFF", 4) == 0);
    CHECK(std::memcmp(wave.data() + 8, "WAVEfmt ", 8) == 0);
    size_t files = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        if (item.path().extension() == ".wav") {
            files++;
        }
    }
    CHECK(files == 399);
    std::filesystem::remove_all(home);
}

TEST_CASE("exportsources and make rebuild DeusExSounds identical to the shipped package", "[data]")
{
    // ucc (or the child run) needs the data too; this fails saying it is missing, not with an exit code.
    dx::test::gameData();
    const char* xdg = std::getenv("XDG_DATA_HOME");
    REQUIRE(xdg != nullptr);
    std::filesystem::path home = std::filesystem::path(xdg) / "ucc-audio-make";
    std::filesystem::remove_all(home);
    std::filesystem::path user = home / "ambrosia";
    REQUIRE(ucctest::runUcc(home, "exportsources DeusExSounds.u \"" + user.string() + "\"") == 0);
    CHECK(std::filesystem::exists(user / "DeusExSounds" / "Sounds" / "Generic" / "AlarmUnitL.wav"));
    REQUIRE(ucctest::runUcc(home, "make") == 0);
    std::filesystem::path shipped = std::filesystem::path(dx::test::gameData()) / "System" / "DeusExSounds.u";
    std::filesystem::path built = user / "System" / "DeusExSounds.u";
    REQUIRE(std::filesystem::exists(built));
    std::filesystem::path system = std::filesystem::path(dx::test::gameData()) / "System";
    auto expected = dx::compiler::dumpPackage(shipped, system);
    auto actual = dx::compiler::dumpPackage(built, system);
    std::map<std::string, dx::compiler::DumpedExport> actualByPath;
    for (auto& [path, entry] : actual) {
        actualByPath[lowerText(path)] = entry;
    }
    CHECK(actualByPath.size() == expected.size());
    int sounds = 0;
    int mismatches = 0;
    for (auto& [path, entry] : expected) {
        auto it = actualByPath.find(lowerText(path));
        if (entry.className == "Sound") {
            sounds++;
        }
        if (entry.className == "Class" || entry.className == "TextBuffer") {
            continue;
        }
        bool same = it != actualByPath.end() && it->second.className == entry.className && it->second.flags == entry.flags && lowerText(it->second.body) == lowerText(entry.body);
        if (!same) {
            mismatches++;
        }
    }
    CHECK(sounds == 399);
    CHECK(mismatches == 0);
    dx::Package package(built);
    std::vector<uint8_t> bytes = readFile(built);
    int checked = 0;
    for (size_t i = 0; i < package.exports().size(); i++) {
        int32_t ref = static_cast<int32_t>(i + 1);
        if (package.classOf(ref) != "Sound") {
            continue;
        }
        const dx::ExportEntry& entry = package.exportAt(ref);
        int32_t value = 0;
        size_t at = compactIndex(bytes, static_cast<size_t>(entry.offset), value);
        at = compactIndex(bytes, at, value);
        uint32_t skip = 0;
        std::memcpy(&skip, bytes.data() + at, 4);
        CHECK(skip == static_cast<uint32_t>(entry.offset + entry.size));
        checked++;
    }
    CHECK(checked == 399);
    std::filesystem::remove_all(home);
}

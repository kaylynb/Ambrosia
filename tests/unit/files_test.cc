#include "core/files.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <fstream>
#include <iterator>
#include <string>

namespace {
    void writeText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream(path, std::ios::binary) << text;
    }

    std::string readText(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }
}

TEST_CASE("replacing a file moves it over the one already there", "[files]")
{
    dx::test::TempDir folder("files-replace");
    writeText(folder.path() / "new.tmp", "new");
    writeText(folder.path() / "kept.ini", "old");
    std::error_code error;
    dx::files::replace(folder.path() / "new.tmp", folder.path() / "kept.ini", error);
    CHECK_FALSE(error);
    CHECK(readText(folder.path() / "kept.ini") == "new");
    CHECK_FALSE(std::filesystem::exists(folder.path() / "new.tmp"));
    // With nothing to replace it is a plain rename, and a missing source is an error.
    writeText(folder.path() / "other.tmp", "other");
    dx::files::replace(folder.path() / "other.tmp", folder.path() / "fresh.ini", error);
    CHECK_FALSE(error);
    CHECK(readText(folder.path() / "fresh.ini") == "other");
    dx::files::replace(folder.path() / "missing.tmp", folder.path() / "fresh.ini", error);
    CHECK(error);
    CHECK(readText(folder.path() / "fresh.ini") == "other");
}

TEST_CASE("copying a file copies every byte over any file already there", "[files]")
{
    dx::test::TempDir folder("files-copy");
    std::string data;
    for (int i = 0; i < 200000; i++) {
        data.push_back(static_cast<char>(i * 7));
    }
    writeText(folder.path() / "level.dxs", data);
    writeText(folder.path() / "copy.dxs", "something longer that must go");
    CHECK(dx::files::copy(folder.path() / "level.dxs", folder.path() / "copy.dxs"));
    CHECK(readText(folder.path() / "copy.dxs") == data);
    writeText(folder.path() / "empty.dxs", "");
    CHECK(dx::files::copy(folder.path() / "empty.dxs", folder.path() / "copy.dxs"));
    CHECK(readText(folder.path() / "copy.dxs").empty());
    CHECK_FALSE(dx::files::copy(folder.path() / "missing.dxs", folder.path() / "copy.dxs"));
}

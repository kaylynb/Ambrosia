#include "core/steam.h"

#include <catch_amalgamated.hpp>

#include <filesystem>

using dx::steam::libraryFolders;

TEST_CASE("libraryFolders reads each library's path from libraryfolders.vdf", "[steam]")
{
    // As Steam writes it on Windows, with a second library and its apps block.
    const char* vdf = "\"libraryfolders\"\n"
                      "{\n"
                      "\t\"0\"\n"
                      "\t{\n"
                      "\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
                      "\t\t\"label\"\t\t\"\"\n"
                      "\t\t\"apps\"\n"
                      "\t\t{\n"
                      "\t\t\t\"228980\"\t\t\"1234\"\n"
                      "\t\t}\n"
                      "\t}\n"
                      "\t\"1\"\n"
                      "\t{\n"
                      "\t\t\"path\"\t\t\"D:\\\\Games\\\\Steam Library\"\n"
                      "\t\t\"apps\"\n"
                      "\t\t{\n"
                      "\t\t\t\"6910\"\t\t\"750000000\"\n"
                      "\t\t}\n"
                      "\t}\n"
                      "}\n";
    std::vector<std::filesystem::path> folders = libraryFolders(vdf);
    REQUIRE(folders.size() == 2);
    CHECK(folders[0] == std::filesystem::path("C:\\Program Files (x86)\\Steam"));
    CHECK(folders[1] == std::filesystem::path("D:\\Games\\Steam Library"));
}

TEST_CASE("libraryFolders reads Linux paths and CRLF line ends", "[steam]")
{
    const char* vdf = "\"libraryfolders\"\r\n{\r\n\t\"0\"\r\n\t{\r\n\t\t\"path\"\t\t\"/home/player/.local/share/Steam\"\r\n\t}\r\n}\r\n";
    std::vector<std::filesystem::path> folders = libraryFolders(vdf);
    REQUIRE(folders.size() == 1);
    CHECK(folders[0] == std::filesystem::path("/home/player/.local/share/Steam"));
}

TEST_CASE("libraryFolders skips what is not a path line", "[steam]")
{
    CHECK(libraryFolders("").empty());
    CHECK(libraryFolders("garbage\n{\n}").empty());
    // A key that only contains path, an unclosed value, an empty one, and one with no value.
    CHECK(libraryFolders("\"pathx\"  \"/a\"\n\"path\" \"/b\n\"path\" \"\"\n\"path\"\n").empty());
    // An escaped quote stays in the path.
    std::vector<std::filesystem::path> folders = libraryFolders("\"path\" \"/odd\\\"name\"\n");
    REQUIRE(folders.size() == 1);
    CHECK(folders[0] == std::filesystem::path("/odd\"name"));
}

TEST_CASE("gameFolder is the game's folder in a library", "[steam]")
{
    CHECK(dx::steam::gameFolder("/lib") == std::filesystem::path("/lib") / "steamapps" / "common" / "Deus Ex");
}

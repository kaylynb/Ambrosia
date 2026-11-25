#include "editor/command_args.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

TEST_CASE("command arguments read quoted paths as one token")
{
    std::istringstream input("\"C:\\My Maps\\a.t3d\" selected");
    CHECK(dx::editor::readArg(input) == "C:\\My Maps\\a.t3d");
    std::string scope;
    input >> scope;
    CHECK(scope == "selected");

    std::istringstream plain("map.t3d 1 4");
    CHECK(dx::editor::readArg(plain) == "map.t3d");
    int merge = 0;
    int flags = 0;
    plain >> merge >> flags;
    CHECK(merge == 1);
    CHECK(flags == 4);

    std::istringstream empty("   ");
    CHECK(dx::editor::readArg(empty).empty());
}

TEST_CASE("quoted command arguments round trip quotes and spaces")
{
    std::vector<std::string> samples = { "plain", "with space", "say \"hi\"", "\"", "", "trailing \"\"" };
    for (const std::string& sample : samples) {
        std::istringstream input(dx::editor::quoteArg(sample) + " next");
        CHECK(dx::editor::readArg(input) == sample);
        CHECK(dx::editor::readArg(input) == "next");
        CHECK(dx::editor::restArg(dx::editor::quoteArg(sample)) == sample);
    }
}

TEST_CASE("the rest of a command is one argument whether quoted or typed")
{
    CHECK(dx::editor::restArg("C:/My Maps/a.dx") == "C:/My Maps/a.dx");
    CHECK(dx::editor::restArg("  \"C:/My Maps/a.dx\"") == "C:/My Maps/a.dx");
    CHECK(dx::editor::restArg("\"unterminated name") == "unterminated name");
    CHECK(dx::editor::restArg("").empty());
}

TEST_CASE("UTF-8 text from the UI names files with spaces and non-ASCII characters")
{
    std::string folderName = "dented args \xc3\xa9t\xc3\xa9 \xe2\x98\x83";
    dx::test::TempDir temp("command-args");
    std::filesystem::path folder = temp / dx::editor::utf8Path(folderName);
    REQUIRE(std::filesystem::create_directories(folder));
    std::string text = dx::editor::utf8String(folder / dx::editor::utf8Path("m\xc3\xa4p one.t3d"));
    std::istringstream command("exportt3d " + dx::editor::quoteArg(text) + " selected");
    std::string verb;
    command >> verb;
    std::string file = dx::editor::readArg(command);
    CHECK(file == text);
    {
        std::ofstream out(dx::editor::utf8Path(file), std::ios::binary);
        out << "Begin Map";
        REQUIRE(out);
    }
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(folder, error), end; !error && entry != end; entry.increment(error)) {
        names.push_back(dx::editor::utf8String(entry->path().filename()));
    }
    CHECK(names == std::vector<std::string> { "m\xc3\xa4p one.t3d" });
    std::ifstream in(dx::editor::utf8Path(file), std::ios::binary);
    std::stringstream read;
    read << in.rdbuf();
    CHECK(read.str() == "Begin Map");
    in.close();
}

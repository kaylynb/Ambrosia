#include "compiler/props_generator.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
    std::string readFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
}

TEST_CASE("the checked-in property bindings match a fresh generation from the list", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    std::filesystem::path engine = dx::test::sourceDir() / "src" / "engine";
    dx::test::TempDir output("props");
    std::vector<std::string> errors;
    bool written = dx::compiler::writeProps(engine / "props_bindings.txt", output.path(), errors);
    for (const std::string& error : errors) {
        UNSCOPED_INFO(error);
    }
    REQUIRE(written);
    size_t count = 0;
    for (const auto& item : std::filesystem::directory_iterator(output.path())) {
        INFO(item.path().filename().string() << " differs; run ucc genprops src/engine/props_bindings.txt src/engine");
        CHECK(readFile(item.path()) == readFile(engine / item.path().filename()));
        count++;
    }
    CHECK(count == 10);
}

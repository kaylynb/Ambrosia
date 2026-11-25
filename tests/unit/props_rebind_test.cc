#include "compiler/exporter.h"
#include "core/objects.h"
#include "editor/script_workspace.h"
#include "engine/props_fire.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <string>

TEST_CASE("editor compiles rebind the engine's property offsets and refuse to drop a bound property", "[.][editor-scripts][data]")
{
    dx::test::ScriptEditing editing;
    dx::UClass* ice = dx::objects::findClass("Fire.IceTexture");
    REQUIRE(ice != nullptr);
    REQUIRE(ice->scriptText != nullptr);
    dx::objects::ensureLoaded(ice->scriptText);
    dx::objects::flushLoads();
    std::string original = dx::compiler::exportClass(ice);
    size_t before = dx::offsets_icetexture.TickAccu;
    REQUIRE(before == ice->findProperty(dx::Name("TickAccu"))->offset);
    dx::editor::ScriptWorkspace workspace;

    size_t header = original.find(';', original.find("class IceTexture"));
    REQUIRE(header != std::string::npos);
    std::string shifted = original.substr(0, header + 1) + "\r\nvar int EdTestShift;" + original.substr(header + 1);
    REQUIRE(workspace.compile({ { ice, shifted } }, false));
    size_t moved = ice->findProperty(dx::Name("TickAccu"))->offset;
    CHECK(moved > before);
    CHECK(dx::offsets_icetexture.TickAccu == moved);

    size_t line = shifted.find("TickAccu;");
    REQUIRE(line != std::string::npos);
    size_t start = shifted.rfind('\n', line) + 1;
    std::string dropped = shifted.substr(0, start) + shifted.substr(shifted.find('\n', line) + 1);
    REQUIRE_FALSE(workspace.compile({ { ice, dropped } }, false));
    bool reported = false;
    for (const dx::editor::CompileMessage& message : workspace.messages()) {
        if (message.error && message.text.find("Fire.IceTexture.TickAccu") != std::string::npos) {
            reported = true;
        }
    }
    CHECK(reported);
    REQUIRE(ice->findProperty(dx::Name("TickAccu")) != nullptr);
    CHECK(ice->findProperty(dx::Name("TickAccu"))->offset == moved);
    CHECK(dx::offsets_icetexture.TickAccu == moved);

    REQUIRE(workspace.compile({ { ice, original } }, false));
    CHECK(dx::offsets_icetexture.TickAccu == before);
}

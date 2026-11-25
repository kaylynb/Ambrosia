#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/props_bind.h"
#include "engine/props_deusex.h"
#include "engine/props_engine.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <iterator>
#include <string>
#include <vector>

TEST_CASE("binding reports a kind, size or array mismatch with expected and actual", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    size_t kind = 123;
    size_t size = 123;
    size_t array = 123;
    size_t good = 123;
    const dx::PropBinding properties[] = {
        { "bHidden", &kind, dx::PropType::Int, sizeof(int32_t), 1, nullptr, false },
        { "Location", &size, dx::PropType::Struct, 4, 1, "Vector", false },
        { "Physics", &array, dx::PropType::Byte, sizeof(uint8_t), 2, nullptr, false },
        { "DrawScale", &good, dx::PropType::Float, sizeof(float), 1, nullptr, false },
    };
    const dx::ClassBinding classes[] = { { "Engine.Actor", properties, std::size(properties) } };
    std::vector<std::string> failures;

    REQUIRE_FALSE(dx::bindClasses(classes, std::size(classes), failures));

    REQUIRE(failures.size() == 3);
    CHECK(failures[0] == "Engine.Actor.bHidden: expected int, 4 bytes, 1 element, found bool, 4 bytes, 1 element");
    CHECK(failures[1] == "Engine.Actor.Location: expected struct Vector, 4 bytes, 1 element, found struct Vector, 12 bytes, 1 element");
    CHECK(failures[2] == "Engine.Actor.Physics: expected byte, 1 byte, 2 elements, found byte, 1 byte, 1 element");
    // A failed bind writes nothing, so offsets bound earlier stay usable.
    CHECK(kind == 123);
    CHECK(good == 123);
}

TEST_CASE("binding reports a missing required property and a missing class", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    size_t missing = 0;
    size_t orphan = 0;
    const dx::PropBinding actor[] = { { "NoSuchProperty", &missing, dx::PropType::Int, sizeof(int32_t), 1, nullptr, false } };
    const dx::PropBinding gone[] = { { "Anything", &orphan, dx::PropType::Int, sizeof(int32_t), 1, nullptr, false } };
    const dx::ClassBinding classes[] = {
        { "Engine.Actor", actor, std::size(actor) },
        { "Engine.NoSuchClass", gone, std::size(gone) },
    };
    std::vector<std::string> failures;

    REQUIRE_FALSE(dx::bindClasses(classes, std::size(classes), failures));

    REQUIRE(failures.size() == 2);
    CHECK(failures[0] == "Engine.Actor.NoSuchProperty: expected int, 4 bytes, 1 element, found no such property");
    CHECK(failures[1] == "Engine.NoSuchClass.Anything: expected int, 4 bytes, 1 element, found no such class");
}

TEST_CASE("an absent optional property binds as absent and its accessor returns null", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    size_t absent = 0;
    size_t present = dx::PropAbsent;
    const dx::PropBinding properties[] = {
        { "NoSuchProperty", &absent, dx::PropType::Float, sizeof(float), 1, nullptr, true },
        { "DrawScale", &present, dx::PropType::Float, sizeof(float), 1, nullptr, true },
    };
    const dx::ClassBinding classes[] = { { "Engine.Actor", properties, std::size(properties) } };
    std::vector<std::string> failures;

    REQUIRE(dx::bindClasses(classes, std::size(classes), failures));

    CHECK(failures.empty());
    CHECK(absent == dx::PropAbsent);
    CHECK(present == dx::offsets_actor.DrawScale);

    // The generated accessor of an optional property returns null while it is absent.
    dx::UObject* levelInfo = dx::objects::findClass("DeusEx.DeusExLevelInfo")->defaultObject;
    REQUIRE(dx::deusexlevelinfo::ConversationPackage(levelInfo) != nullptr);
    size_t saved = dx::offsets_deusexlevelinfo.ConversationPackage;
    dx::offsets_deusexlevelinfo.ConversationPackage = dx::PropAbsent;
    CHECK(dx::deusexlevelinfo::ConversationPackage(levelInfo) == nullptr);
    CHECK(dx::propOr(dx::deusexlevelinfo::ConversationPackage(levelInfo), std::string("fallback")) == "fallback");
    dx::offsets_deusexlevelinfo.ConversationPackage = saved;
}

TEST_CASE("binding every generated table again leaves the offsets unchanged", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::propsBound());
    size_t location = dx::offsets_actor.Location;
    size_t conversations = dx::offsets_deusexlevelinfo.ConversationPackage;
    std::vector<std::string> failures;

    REQUIRE(dx::bindAllProps(failures));

    CHECK(failures.empty());
    CHECK(dx::offsets_actor.Location == location);
    CHECK(dx::offsets_deusexlevelinfo.ConversationPackage == conversations);
}

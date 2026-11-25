#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/url.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    void initEngine()
    {
        static bool initialized = false;
        if (!initialized) {
            dx::engine::initialize(dx::test::gameData());
            initialized = true;
        }
    }

    dx::UClass* testInfoClass()
    {
        dx::UClass* cls = dx::objects::findClass("Engine.TestInfo");
        dx::objects::flushLoads();
        REQUIRE(cls != nullptr);
        return cls;
    }

    dx::UFunction* coreFunction(const char* name)
    {
        dx::UFunction* function = dx::objects::objectClass()->findFunction(dx::Name(name));
        REQUIRE(function != nullptr);
        return function;
    }

    struct Bytes {
        std::vector<uint8_t> data;

        Bytes& op(std::initializer_list<uint8_t> values)
        {
            data.insert(data.end(), values);
            return *this;
        }

        Bytes& u16(uint16_t value)
        {
            data.push_back(static_cast<uint8_t>(value & 0xff));
            data.push_back(static_cast<uint8_t>(value >> 8));
            return *this;
        }

        Bytes& u32(uint32_t value)
        {
            for (int i = 0; i < 4; i++) {
                data.push_back(static_cast<uint8_t>(value >> (8 * i)));
            }
            return *this;
        }

        Bytes& f32(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            return u32(bits);
        }

        Bytes& str(const char* text)
        {
            data.insert(data.end(), text, text + std::strlen(text) + 1);
            return *this;
        }

        Bytes& object(const dx::UObject* value)
        {
            return u32(value->handle);
        }

        Bytes& name(const char* text)
        {
            return u32(dx::Name(text).index());
        }
    };

    dx::UFunction* makeFunction(dx::UClass* owner, const char* name, const Bytes& code, uint32_t flags = 0, dx::UField* params = nullptr)
    {
        dx::UFunction* function = new dx::UFunction();
        function->name = dx::Name(name);
        function->outer = owner;
        function->functionFlags = flags;
        function->children = params;
        function->script = code.data;
        dx::objects::registerObject(function);
        function->link();
        return function;
    }

    dx::UProperty* makeParam(const char* name, uint32_t flags)
    {
        dx::UProperty* property = new dx::UProperty(dx::PropType::Int);
        property->name = dx::Name(name);
        property->propertyFlags = flags;
        dx::objects::registerObject(property);
        return property;
    }

    dx::UState* makeState(dx::UClass* owner, const char* name, const Bytes& code, uint16_t labelTable)
    {
        dx::UState* state = new dx::UState();
        state->name = dx::Name(name);
        state->outer = owner;
        state->script = code.data;
        state->labelTableOffset = labelTable;
        state->ignoreMask = ~0ull;
        dx::objects::registerObject(state);
        owner->states[state->name] = state;
        return state;
    }

    int32_t& xnum(dx::UObject* object)
    {
        uint8_t* address = object->propertyAddress(dx::Name("xnum"));
        REQUIRE(address != nullptr);
        return *reinterpret_cast<int32_t*>(address);
    }

    dx::UProperty* xnumProperty(dx::UClass* cls)
    {
        dx::UProperty* property = cls->findProperty(dx::Name("xnum"));
        REQUIRE(property != nullptr);
        return property;
    }

    Bytes incrementXnum(dx::UClass* cls)
    {
        Bytes code;
        code.op({ 0xA3, 0x01 }).object(xnumProperty(cls)).op({ 0x16 });
        return code;
    }

    template <typename T>
    T evaluate(dx::UObject* self, const Bytes& code)
    {
        dx::UStruct node;
        node.script = code.data;
        dx::Frame frame;
        frame.object = self;
        frame.node = &node;
        frame.code = node.script.data();
        T value {};
        frame.step(self, &value);
        return value;
    }
}

TEST_CASE("vector to string conversion prints six decimals like the original", "[data]")
{
    initEngine();
    Bytes code;
    code.op({ 0x58, 0x23 }).f32(1.5f).f32(-2.0f).f32(0.25f);
    CHECK(evaluate<std::string>(dx::objects::objectClass()->defaultObject, code) == "1.500000,-2.000000,0.250000");
}

TEST_CASE("string switch matches case labels case insensitively", "[data]")
{
    initEngine();
    Bytes code;
    code.op({ 0x05, 0x00, 0x1F }).str("abc");
    code.op({ 0x0A }).u16(20).op({ 0x1F }).str("ABC");
    code.op({ 0x0B, 0x0B, 0x0B, 0x0B, 0x0B });
    code.op({ 0x0A }).u16(0xffff);
    dx::UStruct node;
    node.script = code.data;
    dx::Frame frame;
    frame.object = dx::objects::objectClass()->defaultObject;
    frame.node = &node;
    frame.code = node.script.data();
    frame.step(frame.object, nullptr);
    CHECK(frame.code - node.script.data() == 15);
}

TEST_CASE("Mid clamps the start index as unsigned like FString Mid", "[data]")
{
    initEngine();
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Mid"));
        call.s("abcdef").i(-1).i(3).run();
        CHECK(*reinterpret_cast<std::string*>(call.returnValue()) == "");
    }
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Mid"));
        call.s("abcdef").i(4).i(10).run();
        CHECK(*reinterpret_cast<std::string*>(call.returnValue()) == "ef");
    }
}

TEST_CASE("MirrorVectorByNormal normalizes the normal first", "[data]")
{
    initEngine();
    dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("MirrorVectorByNormal"));
    call.v({ 1, -1, 0 }).v({ 0, 2, 0 }).run();
    dx::Vector value = *reinterpret_cast<dx::Vector*>(call.returnValue());
    CHECK(value.x == 1.0f);
    CHECK(value.y == 1.0f);
    CHECK(value.z == 0.0f);
}

TEST_CASE("script calls to a disabled probe function are skipped", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* object = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    Bytes timerCode = incrementXnum(cls);
    timerCode.op({ 0x04, 0x0B });
    dx::UFunction* timer = makeFunction(cls, "Timer", timerCode);
    Bytes callerCode;
    callerCode.op({ 0x1C }).object(timer).op({ 0x16, 0x04, 0x0B });
    dx::UFunction* caller = makeFunction(cls, "CoreProbeCaller", callerCode);
    xnum(object) = 10;
    dx::vm::disable(object, dx::Name("Timer"));
    {
        dx::vm::Call call(object, caller);
        call.run();
    }
    CHECK(xnum(object) == 10);
    {
        dx::vm::Call call(object, timer);
        call.run();
    }
    CHECK(xnum(object) == 10);
    dx::vm::enable(object, dx::Name("Timer"));
    {
        dx::vm::Call call(object, caller);
        call.run();
    }
    CHECK(xnum(object) == 11);
}

TEST_CASE("singular functions do not reenter on the same object", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* object = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    dx::UFunction* recurse = makeFunction(cls, "CoreSingular", Bytes(), dx::FUNC_Singular);
    Bytes code = incrementXnum(cls);
    code.op({ 0x1C }).object(recurse).op({ 0x16, 0x04, 0x0B });
    recurse->script = code.data;
    std::vector<std::string> criticals;
    dx::vm::CriticalHandler previous = dx::vm::setCriticalHandler([&criticals](const std::string& message) {
        criticals.push_back(message);
    });
    xnum(object) = 0;
    {
        dx::vm::Call call(object, recurse);
        call.run();
    }
    CHECK(xnum(object) == 1);
    CHECK(criticals.empty());
    {
        dx::vm::Call call(object, recurse);
        call.run();
    }
    CHECK(xnum(object) == 2);
    dx::vm::setCriticalHandler(previous);
}

TEST_CASE("out parameters are copied back in reverse order", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* object = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    dx::UProperty* a = makeParam("A", dx::CPF_Parm | dx::CPF_OutParm);
    dx::UProperty* b = makeParam("B", dx::CPF_Parm | dx::CPF_OutParm);
    a->next = b;
    Bytes code;
    code.op({ 0x0F, 0x00 }).object(a).op({ 0x26 });
    code.op({ 0x0F, 0x00 }).object(b).op({ 0x2C, 0x02 });
    code.op({ 0x04, 0x0B });
    dx::UFunction* setBoth = makeFunction(cls, "CoreSetBoth", code, 0, a);
    Bytes callerCode;
    callerCode.op({ 0x1C }).object(setBoth).op({ 0x01 }).object(xnumProperty(cls)).op({ 0x01 }).object(xnumProperty(cls)).op({ 0x16, 0x04, 0x0B });
    dx::UFunction* caller = makeFunction(cls, "CoreSetBothCaller", callerCode);
    xnum(object) = 0;
    dx::vm::Call call(object, caller);
    call.run();
    CHECK(xnum(object) == 1);
}

TEST_CASE("GotoState from EndState preempts the outer GotoState", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* object = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    Bytes begin;
    begin.op({ 0x08 }).name("Begin").u32(0).u32(0).u32(0);
    dx::UState* first = makeState(cls, "CoreFirst", begin, 1);
    makeState(cls, "CoreSecond", begin, 1);
    makeState(cls, "CoreThird", begin, 1);
    Bytes endCode;
    endCode.op({ 0x71, 0x21 }).name("CoreSecond").op({ 0x16, 0x04, 0x0B });
    first->functions[dx::Name("EndState")] = makeFunction(cls, "EndState", endCode);
    std::vector<std::string> criticals;
    dx::vm::CriticalHandler previous = dx::vm::setCriticalHandler([&criticals](const std::string& message) {
        criticals.push_back(message);
    });
    REQUIRE(dx::vm::gotoState(object, dx::Name("CoreFirst")));
    dx::vm::gotoState(object, dx::Name("CoreThird"));
    dx::vm::setCriticalHandler(previous);
    CHECK(criticals.empty());
    CHECK(dx::vm::stateName(object) == dx::Name("CoreSecond"));
}

TEST_CASE("state code stops after four state changes in one update", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* object = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    Bytes ping;
    ping.op({ 0x71, 0x21 }).name("CorePong").op({ 0x16 }).name("Begin").u32(0).u32(0).u32(0);
    Bytes pong;
    pong.op({ 0x71, 0x21 }).name("CorePing").op({ 0x16 }).name("Begin").u32(0).u32(0).u32(0);
    dx::UState* pingState = makeState(cls, "CorePing", ping, 7);
    dx::UState* pongState = makeState(cls, "CorePong", pong, 7);
    Bytes count = incrementXnum(cls);
    count.op({ 0x04, 0x0B });
    dx::UFunction* beginState = makeFunction(cls, "BeginState", count);
    pingState->functions[dx::Name("BeginState")] = beginState;
    pongState->functions[dx::Name("BeginState")] = beginState;
    REQUIRE(dx::vm::gotoState(object, dx::Name("CorePing")));
    xnum(object) = 0;
    dx::vm::runStateCode(object);
    CHECK(xnum(object) == 5);
    CHECK(dx::vm::stateName(object) == dx::Name("CorePong"));
    dx::vm::runStateCode(object);
    CHECK(xnum(object) == 10);
    CHECK(dx::vm::stateName(object) == dx::Name("CorePing"));
}

TEST_CASE("globalconfig properties load from the declaring class section", "[data]")
{
    initEngine();
    dx::UClass* player = dx::objects::findClass("DeusEx.DeusExPlayer");
    dx::objects::flushLoads();
    REQUIRE(player != nullptr);
    uint8_t* address = player->defaultObject->propertyAddress(dx::Name("bSnapToLevel"));
    REQUIRE(address != nullptr);
    CHECK(*reinterpret_cast<uint32_t*>(address) == 0);
}

TEST_CASE("SaveConfig writes globalconfig to the owner section and reloads class defaults", "[data]")
{
    initEngine();
    dx::UClass* gameInfo = dx::objects::findClass("Engine.GameInfo");
    dx::UClass* deusExGameInfo = dx::objects::findClass("DeusEx.DeusExGameInfo");
    dx::objects::flushLoads();
    REQUIRE(gameInfo != nullptr);
    REQUIRE(deusExGameInfo != nullptr);
    dx::UObject* object = dx::objects::newObject(deusExGameInfo, nullptr, dx::Name(), 0);
    int32_t& spectators = *reinterpret_cast<int32_t*>(object->propertyAddress(dx::Name("MaxSpectators")));
    int32_t original = spectators;
    spectators = original + 5;
    {
        dx::vm::Call call(object, coreFunction("SaveConfig"));
        call.run();
    }
    CHECK(dx::config::forName(dx::config::configNameOf(deusExGameInfo))->get("Engine.GameInfo", "MaxSpectators") == std::to_string(original + 5));
    CHECK(*reinterpret_cast<int32_t*>(gameInfo->defaultObject->propertyAddress(dx::Name("MaxSpectators"))) == original + 5);
    CHECK(*reinterpret_cast<int32_t*>(deusExGameInfo->defaultObject->propertyAddress(dx::Name("MaxSpectators"))) == original + 5);
    spectators = original;
    {
        dx::vm::Call call(object, coreFunction("SaveConfig"));
        call.run();
    }
    CHECK(*reinterpret_cast<int32_t*>(deusExGameInfo->defaultObject->propertyAddress(dx::Name("MaxSpectators"))) == original);
}

TEST_CASE("Normal returns zero below the SafeNormal size squared threshold", "[data]")
{
    initEngine();
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Normal"));
        call.v({ 0.00005f, 0, 0 }).run();
        dx::Vector value = *reinterpret_cast<dx::Vector*>(call.returnValue());
        CHECK(value.x == 0.0f);
        CHECK(value.y == 0.0f);
        CHECK(value.z == 0.0f);
    }
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Normal"));
        call.v({ 0, 0.5f, 0 }).run();
        dx::Vector value = *reinterpret_cast<dx::Vector*>(call.returnValue());
        CHECK(value.x == 0.0f);
        CHECK(value.y == 1.0f);
        CHECK(value.z == 0.0f);
    }
}

TEST_CASE("Chr keeps sixteen bits of the code and Caps only uppercases ASCII", "[data]")
{
    initEngine();
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Chr"));
        call.i(321).run();
        CHECK(*reinterpret_cast<std::string*>(call.returnValue()) == "?");
    }
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Chr"));
        call.i(65536).run();
        CHECK(reinterpret_cast<std::string*>(call.returnValue())->empty());
    }
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Chr"));
        call.i(65601).run();
        CHECK(*reinterpret_cast<std::string*>(call.returnValue()) == "A");
    }
    {
        dx::vm::Call call(dx::objects::objectClass()->defaultObject, coreFunction("Caps"));
        call.s("a\xe9z").run();
        CHECK(*reinterpret_cast<std::string*>(call.returnValue()) == "A\xe9Z");
    }
}

TEST_CASE("new defaults the outer to self and masks the object flags", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* self = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    Bytes code;
    code.op({ 0x11, 0x0B, 0x21 }).u32(0).op({ 0x1D }).u32(0xffffffffu).op({ 0x20 }).object(cls);
    dx::UObject* created = evaluate<dx::UObject*>(self, code);
    REQUIRE(created != nullptr);
    CHECK(created->outer == self);
    CHECK(created->flags == 0x704005u);
}

TEST_CASE("a destroyed but not yet cleaned up object still runs calls made through it", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* caller = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    dx::UObject* target = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    Bytes incrementCode = incrementXnum(cls);
    incrementCode.op({ 0x04, 0x0B });
    dx::UFunction* increment = makeFunction(cls, "CoreContextIncrement", incrementCode);
    Bytes callerCode;
    callerCode.op({ 0x19, 0x20 }).object(target).u16(6).op({ 0x00, 0x1C }).object(increment).op({ 0x16, 0x04, 0x0B });
    dx::UFunction* through = makeFunction(cls, "CoreContextCaller", callerCode);
    xnum(target) = 0;
    dx::objects::destroyObject(target);
    dx::vm::Call call(caller, through);
    call.run();
    CHECK(xnum(target) == 1);
}

TEST_CASE("relative and partial travel inherit the current URL options like FURL")
{
    std::string base = "01_NYC_UNATCOIsland?Name=JC?Class=DeusEx.JCDentonMale?load?Difficulty=2?push";
    CHECK(dx::travelUrl(base, "01_NYC_UNATCOHQ?difficulty=3?Quiet", dx::TRAVEL_Relative) == "01_NYC_UNATCOHQ?Name=JC?Class=DeusEx.JCDentonMale?difficulty=3?Quiet");
    CHECK(dx::travelUrl(base, "?Name=Paul", dx::TRAVEL_Relative) == "01_NYC_UNATCOIsland?Name=Paul?Class=DeusEx.JCDentonMale?Difficulty=2");
    CHECK(dx::travelUrl(base, "02_NYC_BatteryPark", dx::TRAVEL_Partial) == "02_NYC_BatteryPark?Name=JC?Class=DeusEx.JCDentonMale?Difficulty=2");
    CHECK(dx::travelUrl(base, "02_NYC_BatteryPark?Name=X", dx::TRAVEL_Absolute) == "02_NYC_BatteryPark?Name=X");
}

TEST_CASE("a subclass created at runtime inherits the parent's defaults, functions and states", "[data]")
{
    initEngine();
    dx::UClass* parent = testInfoClass();
    dx::UClass* sub = dx::objects::createSubclass(parent, dx::Name("RuntimeTestInfo"));
    CHECK(sub->superClass() == parent);
    CHECK(sub->isChildOf(parent));
    CHECK(sub->findFunction(dx::Name("TestStructBools")) == parent->findFunction(dx::Name("TestStructBools")));
    CHECK(sub->findState(dx::Name("AA")) == parent->findState(dx::Name("AA")));
    dx::UObject* object = dx::objects::newObject(sub, nullptr, dx::Name(), 0);
    CHECK(object->cls == sub);
    CHECK(object->isA(parent));
    CHECK(xnum(object) == 777);
    CHECK(*reinterpret_cast<std::string*>(object->propertyAddress(dx::Name("sxx"))) == "Tim");
}

TEST_CASE("findObject skips exports that are not loaded outside the editor", "[data]")
{
    initEngine();
    dx::UClass* textBuffer = dx::objects::intrinsicClass(dx::Name("TextBuffer"));
    REQUIRE(textBuffer != nullptr);
    dx::UObject* found = dx::objects::findObject("Engine.Actor.ScriptText", textBuffer);
    if (dx::objects::editorMode()) {
        CHECK(found != nullptr);
    } else {
        CHECK(found == nullptr);
    }
}

TEST_CASE("Ini save replaces the file only when the write succeeds")
{
    dx::test::TempDir directory("ini-save");
    dx::Ini ini;
    ini.path = directory / "Test.ini";
    ini.set("Section", "Key", "One");
    REQUIRE(ini.save());
    CHECK_FALSE(std::filesystem::exists(directory / "Test.ini.tmp"));
    dx::Ini reloaded;
    REQUIRE(reloaded.load(directory / "Test.ini"));
    CHECK(reloaded.get("Section", "Key") == "One");

    std::filesystem::create_directories(directory / "Test.ini.tmp");
    ini.set("Section", "Key", "Two");
    CHECK_FALSE(ini.save());
    dx::Ini unchanged;
    REQUIRE(unchanged.load(directory / "Test.ini"));
    CHECK(unchanged.get("Section", "Key") == "One");

    dx::Ini missing;
    missing.path = directory / "missing" / "Test.ini";
    missing.set("Section", "Key", "One");
    CHECK_FALSE(missing.save());
}

TEST_CASE("integer division of the minimum by minus one wraps instead of trapping", "[data]")
{
    initEngine();
    Bytes code;
    code.op({ 0x91, 0x1D }).u32(0x80000000u).op({ 0x1D }).u32(0xFFFFFFFFu).op({ 0x16 });
    CHECK(evaluate<int32_t>(dx::objects::objectClass()->defaultObject, code) == INT32_MIN);
    Bytes other;
    other.op({ 0x91, 0x1D }).u32(static_cast<uint32_t>(-7)).op({ 0x1D }).u32(0xFFFFFFFFu).op({ 0x16 });
    CHECK(evaluate<int32_t>(dx::objects::objectClass()->defaultObject, other) == 7);
}

TEST_CASE("runaway script recursion stops at the guard before the native stack runs out", "[data]")
{
    initEngine();
    dx::UClass* cls = testInfoClass();
    dx::UObject* object = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    dx::UFunction* direct = makeFunction(cls, "CoreRecurseDirect", Bytes());
    Bytes directCode;
    directCode.op({ 0x1C }).object(direct).op({ 0x16, 0x04, 0x0B });
    direct->script = directCode.data;
    dx::UFunction* nested = makeFunction(cls, "CoreRecurseNested", Bytes());
    Bytes nestedCode;
    nestedCode.op({ 0x92, 0x92, 0x1C }).object(nested).op({ 0x16, 0x26, 0x16, 0x26, 0x16, 0x04, 0x0B });
    nested->script = nestedCode.data;
    char base = 0;
    size_t deepest = 0;
    int criticals = 0;
    dx::vm::CriticalHandler previous = dx::vm::setCriticalHandler([&](const std::string&) {
        char here = 0;
        deepest = std::max(deepest, static_cast<size_t>(reinterpret_cast<uintptr_t>(&base) - reinterpret_cast<uintptr_t>(&here)));
        criticals++;
    });
    for (dx::UFunction* function : { direct, nested }) {
        deepest = 0;
        criticals = 0;
        {
            dx::vm::Call call(object, function);
            call.run();
        }
        std::printf("%s: guard fired %d time(s) at %zu bytes of native stack (%zu per call)\n", function->name.str().c_str(), criticals, deepest, deepest / 251);
        CHECK(criticals >= 1);
        // The executables link a 16 MB stack. AddressSanitizer's frames are larger, so it gets more room.
#if defined(__SANITIZE_ADDRESS__)
        CHECK(deepest < 12 * 1024 * 1024);
#else
        CHECK(deepest < 4 * 1024 * 1024);
#endif
    }
    dx::vm::setCriticalHandler(previous);
}

TEST_CASE("relinking a struct refreshes findProperty even when the property vector keeps its storage", "[data]")
{
    initEngine();
    dx::UStruct* structType = new dx::UStruct();
    structType->name = dx::Name("CoreRelinkStruct");
    dx::objects::registerObject(structType);
    dx::UProperty* first = makeParam("Relinked", 0);
    dx::UProperty* filler = makeParam("RelinkFiller", 0);
    first->outer = structType;
    filler->outer = structType;
    first->next = filler;
    structType->children = first;
    structType->link();
    REQUIRE(structType->properties.size() == 2);
    REQUIRE(structType->findProperty(dx::Name("Relinked")) == first);
    CHECK(first->offset == 0);
    const void* storage = structType->properties.data();

    // A recompile clears the vector (keeping its capacity) and links new properties of the same count.
    dx::UProperty* second = makeParam("Relinked", 0);
    second->outer = structType;
    filler->next = second;
    structType->children = filler;
    structType->properties.clear();
    structType->size = 0;
    structType->alignment = 1;
    structType->linked = false;
    structType->link();
    REQUIRE(structType->properties.size() == 2);
    CHECK(static_cast<const void*>(structType->properties.data()) == storage);
    CHECK(structType->findProperty(dx::Name("Relinked")) == second);
    CHECK(second->offset == 4);
}

TEST_CASE("the user directory is ambrosia under the XDG data directory")
{
    const char* xdg = std::getenv("XDG_DATA_HOME");
    REQUIRE(xdg != nullptr);
    CHECK(dx::config::userDirectory() == std::filesystem::path(xdg) / "ambrosia");
    CHECK(std::filesystem::is_directory(dx::config::userDirectory()));
}

TEST_CASE("a class that failed to load refuses to create objects", "[data]")
{
    initEngine();
    dx::UClass* cls = dx::objects::createSubclass(testInfoClass(), dx::Name("CoreLoadFailedInfo"));
    REQUIRE(dx::objects::newObject(cls, nullptr, dx::Name(), 0) != nullptr);
    cls->loadFailed = true;
    CHECK(dx::objects::newObject(cls, nullptr, dx::Name(), 0) == nullptr);
    dx::UClass* child = dx::objects::createSubclass(cls, dx::Name("CoreLoadFailedChild"));
    CHECK(dx::objects::newObject(child, nullptr, dx::Name(), 0) == nullptr);
}

TEST_CASE("bytecode naming an unresolved property is a script error, not a crash", "[data]")
{
    initEngine();
    dx::UObject* self = dx::objects::objectClass()->defaultObject;
    for (uint8_t token : { uint8_t(0x00), uint8_t(0x01), uint8_t(0x02), uint8_t(0x36) }) {
        INFO(int(token));
        Bytes code;
        code.op({ token }).u32(0).op({ 0x16 });
        CHECK_THROWS_AS(evaluate<int32_t>(self, code), std::runtime_error);
    }
    Bytes structCompare;
    structCompare.op({ 0x32 }).u32(0).op({ 0x16 });
    CHECK_THROWS_AS(evaluate<int32_t>(self, structCompare), std::runtime_error);
}
#include "core/gc.h"
#include "test_paths.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/csg.h"
#include "fixtures.h"

#include <SDL3/SDL.h>

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
    dx::UObject* makeMesh()
    {
        return dx::objects::newObject(dx::csg::nativeClass("Mesh"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    }

    // Slot reuse on for one test, then back to what the collector decides.
    struct Reuse {
        Reuse()
        {
            dx::engine::initialize(dx::test::gameData());
            dx::objects::forceSlotReuse(true);
        }

        ~Reuse()
        {
            dx::objects::forceSlotReuse(false);
        }

        Reuse(const Reuse&) = delete;
        Reuse& operator=(const Reuse&) = delete;
    };

    // Makes objects until one lands in `slot`, which was freed last; slots freed before it
    // are handed out first. The others are freed again.
    dx::UObject* takeSlot(uint32_t slot)
    {
        std::vector<dx::UObject*> others;
        dx::UObject* found = nullptr;
        for (int i = 0; i < 200000 && !found; i++) {
            dx::UObject* object = makeMesh();
            if (dx::objects::handleSlot(object->handle) == slot) {
                found = object;
            } else {
                others.push_back(object);
            }
        }
        dx::gc::release(others);
        return found;
    }

    // Points stderr at `to`, returning a copy of what it pointed at before.
    int redirectStderr(std::FILE* to)
    {
        std::fflush(stderr);
#ifdef _WIN32
        int saved = _dup(2);
        _dup2(_fileno(to), 2);
#else
        int saved = dup(2);
        dup2(fileno(to), 2);
#endif
        return saved;
    }

    void restoreStderr(int saved)
    {
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, 2);
        _close(saved);
#else
        dup2(saved, 2);
        close(saved);
#endif
    }

    // What `action` writes to stderr, which sees none of it.
    std::string stderrOf(const std::function<void()>& action)
    {
        dx::test::TempDir dir("gc-stderr");
        std::filesystem::path path = dir.path() / "stderr.txt";
        struct CloseFile {
            void operator()(std::FILE* handle) const
            {
                std::fclose(handle);
            }
        };
        std::unique_ptr<std::FILE, CloseFile> file(std::fopen(path.string().c_str(), "wb"));
        REQUIRE(file != nullptr);
        {
            // stderr comes back even when a failed REQUIRE in action throws out of here.
            struct Restore {
                int saved;

                ~Restore()
                {
                    if (saved >= 0) {
                        restoreStderr(saved);
                    }
                }
            } restore { redirectStderr(file.get()) };
            REQUIRE(restore.saved >= 0);
            action();
        }
        file.reset();
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
}

TEST_CASE("a reused slot gets the next generation and the old handle reads None", "[data][gc]")
{
    Reuse reuse;
    dx::UObject* first = makeMesh();
    uint32_t oldHandle = first->handle;
    uint32_t slot = dx::objects::handleSlot(oldHandle);
    dx::gc::release({ first });
    CHECK(dx::objects::atHandle(oldHandle) == nullptr);

    dx::UObject* second = takeSlot(slot);
    REQUIRE(second != nullptr);
    CHECK(dx::objects::handleGeneration(second->handle) == dx::objects::handleGeneration(oldHandle) + 1);
    CHECK(second->handle != oldHandle);
    CHECK(dx::objects::atHandle(second->handle) == second);
    CHECK(dx::objects::atHandle(oldHandle) == nullptr);
    CHECK(dx::objects::all()[slot] == second);
    // Looked up the way bytecode looks up its objects, the old handle warns that it is stale.
    // The warning is expected here, so it goes to a file rather than the test log.
    char expected[32];
    std::snprintf(expected, sizeof(expected), "(handle %08x;", oldHandle);
    dx::UObject* found = second;
    std::string warning = stderrOf([&] { found = dx::objects::fromHandle(oldHandle); });
    CHECK(found == nullptr);
    CHECK(warning.find("names a deleted object") != std::string::npos);
    CHECK(warning.find(expected) != std::string::npos);
    dx::gc::release({ second });
}

TEST_CASE("a slot whose generations run out is never handed out again", "[data][gc]")
{
    Reuse reuse;
    // Take every slot waiting for reuse, so the one freed below is the only one waiting.
    size_t end = dx::objects::all().size();
    std::vector<dx::UObject*> held;
    for (int i = 0; i < 200000; i++) {
        held.push_back(makeMesh());
        if (dx::objects::handleSlot(held.back()->handle) >= end) {
            break;
        }
    }
    dx::UObject* object = makeMesh();
    uint32_t slot = dx::objects::handleSlot(object->handle);
    uint32_t generation = dx::objects::handleGeneration(object->handle);
    // Freed and taken again, the slot counts up its generations until the last one. Nothing
    // refers to the mesh, so the releases skip the search for references.
    while (generation + 1 < dx::objects::HandleGenerations) {
        dx::gc::release({ object }, false);
        object = makeMesh();
        REQUIRE(dx::objects::handleSlot(object->handle) == slot);
        REQUIRE(dx::objects::handleGeneration(object->handle) == generation + 1);
        generation++;
    }
    uint32_t lastHandle = object->handle;
    dx::gc::release({ object }, false);
    CHECK(dx::objects::all()[slot] == nullptr);
    // Nothing made afterwards lands in it, and its last handle stays None.
    std::vector<dx::UObject*> after;
    for (int i = 0; i < 8; i++) {
        after.push_back(makeMesh());
        CHECK(dx::objects::handleSlot(after.back()->handle) != slot);
    }
    CHECK(dx::objects::all()[slot] == nullptr);
    CHECK(dx::objects::atHandle(lastHandle) == nullptr);
    dx::gc::release(after);
    dx::gc::release(held);
}

TEST_CASE("a slot freed while not everything reports is not reused", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    // The game library never reports in unit tests outside a play-freeing test, so nothing
    // here makes everything report.
    dx::gc::forgetLibraryReported("game");
    REQUIRE_FALSE(dx::gc::everythingReported());
    dx::UObject* first = makeMesh();
    uint32_t oldHandle = first->handle;
    uint32_t slot = dx::objects::handleSlot(oldHandle);
    dx::gc::release({ first });
    CHECK(dx::objects::atHandle(oldHandle) == nullptr);
    // Slots earlier tests queued are handed out first; once a new slot is appended the queue is
    // empty, and the slot freed above was never in it.
    size_t end = dx::objects::all().size();
    std::vector<dx::UObject*> made;
    bool reused = false;
    for (int i = 0; i < 200000; i++) {
        dx::UObject* object = makeMesh();
        made.push_back(object);
        uint32_t taken = dx::objects::handleSlot(object->handle);
        reused = reused || taken == slot;
        if (taken >= end) {
            break;
        }
    }
    CHECK_FALSE(reused);
    CHECK(dx::objects::handleSlot(made.back()->handle) >= end);
    dx::gc::release(made);
}

TEST_CASE("an object named only by bytecode keeps its slot through a collection", "[data][gc]")
{
    Reuse reuse;
    // Loaded bytecode records where its object handles are.
    const std::vector<dx::UObject*>& table = dx::objects::all();
    CHECK(std::any_of(table.begin(), table.end(), [](dx::UObject* object) {
        auto* structType = dx::cast<dx::UStruct>(object);
        return structType && structType->linker && structType->scriptObjectsKnown && !structType->scriptObjects.empty();
    }));

    // Objects earlier tests queued for release go first, so the count below is this test's.
    dx::gc::Options none;
    none.candidate = [](const dx::UObject*) { return false; };
    dx::gc::collect(none);

    // A function whose code is `return <target>`, recorded as loading records it. Nothing else
    // refers to the target: it is in no struct's children or properties and no holder has it.
    dx::UObject* named = makeMesh();
    uint32_t namedHandle = named->handle;
    auto* code = new dx::UFunction();
    code->cls = dx::objects::intrinsicClass(dx::Name("Function"));
    code->outer = dx::objects::packageObject(dx::Name("Transient"));
    code->name = dx::Name("GcTestBytecode");
    code->script = { 0x04, 0x20, 0, 0, 0, 0, 0x53 };
    std::memcpy(code->script.data() + 2, &namedHandle, sizeof(namedHandle));
    code->scriptObjects = { 2 };
    code->scriptObjectsKnown = true;
    dx::objects::registerObject(code);
    REQUIRE(dx::gc::reportsReferences(code));

    // Offered as a candidate, the object stays: the bytecode reports it.
    dx::gc::Options options;
    options.candidate = [&](const dx::UObject* object) { return object == named; };
    dx::gc::Result result = dx::gc::collect(options);
    CHECK(result.freed == 0);
    CHECK(dx::objects::atHandle(namedHandle) == named);
    CHECK(named->handle == namedHandle);

    // Without the bytecode, it goes.
    dx::gc::release({ code });
    result = dx::gc::collect(options);
    CHECK(result.freed == 1);
    CHECK(dx::objects::atHandle(namedHandle) == nullptr);
}

// Run by the test below in a process of its own: running out of slots ends the process.
TEST_CASE("slot exhaustion child", "[.slot-exhaustion]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::forceSlotReuse(false);
    dx::objects::setSlotLimit(static_cast<uint32_t>(dx::objects::all().size() + 5));
    for (int i = 0; i < 10; i++) {
        makeMesh();
    }
}

TEST_CASE("running out of object slots stops with a clear error", "[data][gc]")
{
    // ucc (or the child run) needs the data too; this fails saying it is missing, not with an exit code.
    dx::test::gameData();
    std::string exe = std::string(SDL_GetBasePath()) + "unit-tests";
#ifdef _WIN32
    exe += ".exe";
#endif
    dx::test::TempDir dir("gc-slots");
    std::filesystem::path log = dir.path() / "exhaustion.txt";
    std::string command = "\"\"" + exe + "\" \"[.slot-exhaustion]\" > \"" + log.string() + "\" 2>&1\"";
#ifndef _WIN32
    command = "\"" + exe + "\" \"[.slot-exhaustion]\" > \"" + log.string() + "\" 2>&1";
#endif
    int status = std::system(command.c_str());
    CHECK(status != 0);
    std::ifstream in(log);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("fatal: the object table is full") != std::string::npos);
}

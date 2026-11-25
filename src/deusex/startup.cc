#include "deusex/startup.h"

#include "audio/audio.h"
#include "core/config.h"
#include "core/log.h"
#include "core/natives.h"
#include "core/objects.h"
#include "engine/props_bind.h"
#include "engine/mesh.h"
#include "engine/resources.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "engine/world.h"
#include "extension/extension.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace dx::engine {
    void initializeCore(const std::string& gameRoot)
    {
        static bool initialized = false;
        if (initialized) {
            return;
        }
        initialized = true;
        std::filesystem::path root(gameRoot);
        config::initialize(root / "System");
        registerEngineResources();
        registerMeshResources();
        registerObjectNatives();
        registerEngineNatives();
        extension::registerNatives();
        registerDeusExNatives();
        objects::initialize(root);
    }

    void initialize(const std::string& gameRoot)
    {
        static bool initialized = false;
        if (initialized) {
            return;
        }
        initialized = true;
        initializeCore(gameRoot);
        const char* packages[] = { "Engine", "Fire", "Extension", "ConSys", "DeusExText", "DeusEx" };
        for (const char* package : packages) {
            objects::loadPackage(Name(package));
        }
        std::vector<std::string> failures;
        if (!bindAllProps(failures)) {
            for (const std::string& failure : failures) {
                dx::log::error("Init", "property binding: {}", failure);
            }
            throw std::runtime_error(std::to_string(failures.size()) + " script property binding(s) do not match the loaded packages; see the list above. The packages may be from another game version, or src/engine/props_bindings.txt needs regenerating.");
        }
        objects::flushLoads();
        bindEngineClasses();
        console::loadBindings();
    }
}

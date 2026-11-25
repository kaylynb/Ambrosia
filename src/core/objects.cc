#include "core/objects.h"

#include "core/config.h"
#include "core/gc.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/scoped_value.h"
#include "core/text.h"
#include "core/vm.h"
#include "core/reader.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>

namespace dx {
    namespace {
        struct Registry {
            std::filesystem::path root;
            std::map<std::string, std::filesystem::path> paths;
            std::unordered_map<Name, std::unique_ptr<Linker>> linkers;
            std::vector<std::unique_ptr<Linker>> retired;
            std::unordered_map<Name, UObject*> packageObjects;
            std::unordered_map<Name, ObjectFactory> factories;
            std::unordered_map<Name, UClass*> intrinsics;
            std::vector<UObject*> table;
            // The generation each slot's next handle gets, and the freed slots waiting for reuse,
            // oldest first.
            std::vector<uint16_t> generations;
            std::deque<uint32_t> freeSlots;
            bool forceReuse = false;
            uint32_t slotLimit = objects::HandleSlotMask + 1;
            std::deque<UObject*> pending;
            std::unordered_map<Name, uint32_t> nameCounters;
            std::unordered_set<Name> usedNames;
            std::vector<UObject*> deferredData;
            std::vector<std::string> loadMismatches;
            bool bootstrapping = false;
            bool editorMode = false;
            UProperty* nameProperty = nullptr;
            UProperty* classProperty = nullptr;
            UProperty* outerProperty = nullptr;
            UProperty* flagsProperty = nullptr;
            UClass* objectClassPtr = nullptr;
            UClass* classClassPtr = nullptr;
        };

        // Never destroyed, as the objects it holds are not: freeing them at exit would only
        // cost time, and a static destructor here would run before LeakSanitizer looks, leaving
        // every live object unreachable and reported as leaked.
        Registry& registry()
        {
            static Registry* instance = new Registry;
            return *instance;
        }

        const std::unordered_set<std::string>& engineIntrinsics()
        {
            static const std::unordered_set<std::string> names = { "primitive", "mesh", "lodmesh", "model", "polys", "level", "levelbase", "sound", "music", "palette", "font", "viewport", "client", "player", "netconnection", "netdriver", "engine", "renderbase", "renderdevice", "audiosubsystem", "eventmanager", "aieventtype", "aievent", "aisenderevent", "aireceiverevent" };
            return names;
        }

        const std::unordered_set<std::string>& editorIntrinsics()
        {
            static const std::unordered_set<std::string> names = { "transbuffer" };
            return names;
        }

        const std::unordered_map<std::string, std::string>& intrinsicSupers()
        {
            static const std::unordered_map<std::string, std::string> supers = {
                { "field", "Object" },
                { "struct", "Field" },
                { "function", "Struct" },
                { "state", "Struct" },
                { "class", "State" },
                { "const", "Field" },
                { "enum", "Field" },
                { "property", "Field" },
                { "byteproperty", "Property" },
                { "intproperty", "Property" },
                { "boolproperty", "Property" },
                { "floatproperty", "Property" },
                { "objectproperty", "Property" },
                { "classproperty", "ObjectProperty" },
                { "nameproperty", "Property" },
                { "strproperty", "Property" },
                { "stringproperty", "Property" },
                { "structproperty", "Property" },
                { "arrayproperty", "Property" },
                { "fixedarrayproperty", "Property" },
                { "mapproperty", "Property" },
                { "pointerproperty", "Property" },
                { "textbuffer", "Object" },
                { "package", "Object" },
                { "linker", "Object" },
                { "linkerload", "Linker" },
                { "linkersave", "Linker" },
                { "system", "Subsystem" },
                { "language", "Object" },
                { "viewport", "Player" },
                { "lodmesh", "Mesh" },
                { "client", "Object" },
                { "aievent", "Object" },
                { "aisenderevent", "AIEvent" },
                { "aireceiverevent", "AIEvent" },
                { "netconnection", "Player" },
                { "netdriver", "Subsystem" },
                { "audiosubsystem", "Subsystem" },
                { "renderbase", "Subsystem" },
                { "renderdevice", "Subsystem" },
                { "mesh", "Primitive" },
                { "model", "Primitive" },
                { "level", "LevelBase" },
            };
            return supers;
        }

        UObject* makeIntrinsicInstance(Name typeName)
        {
            std::string key = lowered(typeName.str());
            if (key == "function") {
                return new UFunction();
            }
            if (key == "state") {
                return new UState();
            }
            if (key == "struct") {
                return new UStruct();
            }
            if (key == "class") {
                return new UClass();
            }
            if (key == "const") {
                return new UConst();
            }
            if (key == "enum") {
                return new UEnum();
            }
            if (key == "textbuffer") {
                return new UTextBuffer();
            }
            if (key == "byteproperty") {
                return new UProperty(PropType::Byte);
            }
            if (key == "intproperty") {
                return new UProperty(PropType::Int);
            }
            if (key == "boolproperty") {
                return new UProperty(PropType::Bool);
            }
            if (key == "floatproperty") {
                return new UProperty(PropType::Float);
            }
            if (key == "objectproperty") {
                return new UProperty(PropType::Object);
            }
            if (key == "classproperty") {
                return new UProperty(PropType::Class);
            }
            if (key == "nameproperty") {
                return new UProperty(PropType::Name);
            }
            if (key == "strproperty") {
                return new UProperty(PropType::Str);
            }
            if (key == "stringproperty") {
                return new UProperty(PropType::String);
            }
            if (key == "structproperty") {
                return new UProperty(PropType::Struct);
            }
            if (key == "arrayproperty") {
                return new UProperty(PropType::Array);
            }
            if (key == "fixedarrayproperty") {
                return new UProperty(PropType::FixedArray);
            }
            if (key == "mapproperty") {
                return new UProperty(PropType::Map);
            }
            if (key == "pointerproperty") {
                return new UProperty(PropType::Pointer);
            }
            return nullptr;
        }

        UObject* makeInstance(Name typeName)
        {
            if (UObject* object = makeIntrinsicInstance(typeName)) {
                return object;
            }
            auto it = registry().factories.find(typeName);
            if (it != registry().factories.end()) {
                return it->second();
            }
            return new UObject();
        }

        bool isKnownType(Name typeName)
        {
            if (intrinsicSupers().count(lowered(typeName.str()))) {
                return true;
            }
            return registry().factories.count(typeName) != 0;
        }

        Name nativeTypeOf(Linker* linker, int32_t classRef)
        {
            for (int guard = 0; guard < 128 && classRef != 0; guard++) {
                const Package& file = linker->file();
                Name className(file.objectName(classRef));
                if (isKnownType(className)) {
                    return className;
                }
                if (classRef > 0) {
                    classRef = file.exportAt(classRef).superRef;
                    continue;
                }
                int32_t root = classRef;
                while (file.importAt(root).outer != 0) {
                    root = file.importAt(root).outer;
                }
                Linker* other = objects::loadPackage(Name(file.objectName(root)));
                if (!other) {
                    return Name("Object");
                }
                int32_t found = other->findExport(className, Name("Class"), file.groupOf(classRef));
                if (found == 0) {
                    return className;
                }
                linker = other;
                classRef = found;
            }
            return Name("Object");
        }
    }

    Linker::Linker(Name name, std::unique_ptr<Package> file)
        : packageName(name)
        , package(std::move(file))
    {
        static uint64_t created = 0;
        loadOrder = ++created;
        names.reserve(package->names().size());
        for (const NameEntry& entry : package->names()) {
            names.emplace_back(entry.name);
        }
        exports.resize(package->exports().size(), nullptr);
        imports.resize(package->imports().size(), nullptr);
        importResolved.resize(package->imports().size(), false);
    }

    const Package& Linker::file() const
    {
        return *package;
    }

    Name Linker::name(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= names.size()) {
            throw std::runtime_error("name index out of range in " + packageName.str());
        }
        return names[static_cast<size_t>(index)];
    }

    UObject* Linker::readObject(Reader& reader)
    {
        return object(reader.index());
    }

    Name Linker::readName(Reader& reader)
    {
        return name(reader.index());
    }

    int32_t Linker::findExport(Name objectName, Name className, std::string_view group) const
    {
        const auto& table = package->exports();
        for (size_t i = 0; i < table.size(); i++) {
            if (name(table[i].name) != objectName) {
                continue;
            }
            int32_t ref = static_cast<int32_t>(i + 1);
            if (!className.none()) {
                Name actualClass = table[i].classRef == 0 ? Name("Class") : Name(package->objectName(table[i].classRef));
                if (actualClass != className) {
                    continue;
                }
            }
            if (group != "*" && !iequals(package->groupOf(ref), group)) {
                continue;
            }
            return ref;
        }
        return 0;
    }

    void Linker::forget(const std::unordered_set<const UObject*>& dead)
    {
        for (UObject*& slot : exports) {
            if (slot && dead.count(slot)) {
                slot = nullptr;
            }
        }
        for (size_t i = 0; i < imports.size(); i++) {
            if (imports[i] && dead.count(imports[i])) {
                imports[i] = nullptr;
                importResolved[i] = false;
            }
        }
        if (packageObject && dead.count(packageObject)) {
            packageObject = nullptr;
        }
    }

    bool Linker::forgetExport(const UObject* object)
    {
        size_t index = static_cast<size_t>(object->exportRef - 1);
        if (object->exportRef <= 0 || index >= exports.size() || exports[index] != object) {
            return false;
        }
        exports[index] = nullptr;
        return true;
    }

    void Linker::rebind(int32_t ref, UObject* object)
    {
        exports.at(static_cast<size_t>(ref - 1)) = object;
        if (object) {
            object->inLinkerTable = true;
            object->linkerShared = true;
        }
    }

    UObject* Linker::object(int32_t ref)
    {
        if (ref == 0) {
            return nullptr;
        }
        if (ref > 0) {
            size_t index = static_cast<size_t>(ref - 1);
            if (index >= exports.size()) {
                throw std::runtime_error("export reference out of range in " + packageName.str());
            }
            if (!exports[index]) {
                uint32_t exportFlags = package->exportAt(ref).flags;
                if (!registry().editorMode && (exportFlags & (RF_NotForClient | RF_NotForServer))) {
                    return nullptr;
                }
                return createExport(ref);
            }
            return exports[index];
        }
        size_t index = static_cast<size_t>(-ref - 1);
        if (index >= imports.size()) {
            throw std::runtime_error("import reference out of range in " + packageName.str());
        }
        if (!importResolved[index]) {
            importResolved[index] = true;
            imports[index] = resolveImport(ref);
            if (imports[index]) {
                imports[index]->inLinkerTable = true;
                imports[index]->linkerShared = true;
            }
        }
        return imports[index];
    }

    UObject* Linker::createExport(int32_t ref)
    {
        const ExportEntry& entry = package->exportAt(ref);
        // Read before the object is made: a bad name index throws, and nothing would own it yet.
        Name exportName = name(entry.name);
        // A save may contain instances of mod classes that are no longer installed.
        // Leave every reference to such an instance None, including the level's actor
        // slot. A plain Object has neither its Actor layout nor its script methods.
        if (savedGame && entry.classRef < 0 && !object(entry.classRef)) {
            return nullptr;
        }
        UObject* result;
        if (entry.classRef == 0) {
            result = new UClass();
        } else {
            result = makeInstance(nativeTypeOf(this, entry.classRef));
        }
        result->name = exportName;
        registry().usedNames.insert(result->name);
        result->flags = entry.flags;
        result->linker = this;
        result->exportRef = ref;
        result->loaded = false;
        exports[static_cast<size_t>(ref - 1)] = result;
        result->inLinkerTable = true;
        objects::registerObject(result);
        result->outer = entry.outer != 0 ? object(entry.outer) : packageObject;
        if (entry.classRef != 0) {
            result->cls = cast<UClass>(object(entry.classRef));
            if (!result->cls) {
                log::warn("DevLoad", "missing class for {}", result->pathName());
                result->cls = objects::objectClass();
            }
        } else {
            result->cls = objects::classClass();
        }
        objects::queueLoad(result);
        return result;
    }

    UObject* Linker::resolveImport(int32_t ref)
    {
        const ImportEntry& entry = package->importAt(ref);
        Name objectName = name(entry.name);
        Name className = name(entry.className);
        if (entry.outer == 0) {
            objects::loadPackage(objectName);
            return objects::packageObject(objectName);
        }
        int32_t root = ref;
        while (package->importAt(root).outer != 0) {
            root = package->importAt(root).outer;
        }
        Name rootName(package->objectName(root));
        Linker* other = objects::loadPackage(rootName);
        if (!other) {
            log::warn("DevLoad", "package {} not found for {}", rootName.str(), objectName.str());
            return nullptr;
        }
        std::string group = package->groupOf(ref);
        int32_t found = other->findExport(objectName, className, group);
        if (found == 0 && className == Name("Class")) {
            if (savedGame && !objects::isIntrinsicClassName(objectName.str())) {
                UClass* live = objects::findClass(rootName.str() + "." + objectName.str());
                if (live && live->outermost()->name == rootName) {
                    return live;
                }
                log::warn("DevLoad", "saved class {}.{} is not installed; skipping its instances", rootName.str(), objectName.str());
                return nullptr;
            }
            return objects::intrinsicClass(objectName);
        }
        if (found == 0) {
            found = other->findExport(objectName, className, "*");
        }
        if (found == 0) {
            log::warn("DevLoad", "cannot resolve {} {}.{}.{}", className.str(), rootName.str(), group, objectName.str());
            return nullptr;
        }
        return other->object(found);
    }

    namespace objects {
        void allocateData(UObject* object);
        void setData(UObject* object, UClass* layout, const UObject* defaults);
        void finishBootstrap();

        namespace {
            // For log lines: path::string() throws on Windows for names outside the narrow code page.
            std::string displayPath(const std::filesystem::path& path)
            {
                std::u8string text = path.u8string();
                return std::string(text.begin(), text.end());
            }
        }

        void initialize(const std::filesystem::path& gameRoot)
        {
            Registry& reg = registry();
            reg.root = gameRoot;
            if (reg.table.empty()) {
                reg.table.push_back(nullptr);
                reg.generations.push_back(0);
            }
            const char* folders[] = { "System", "Textures", "Sounds", "Music", "Maps" };
            const char* extensions[] = { ".u", ".utx", ".uax", ".umx", ".dx", ".unr" };
            std::vector<std::filesystem::path> directories;
            for (const char* folder : folders) {
                directories.push_back(gameRoot / folder);
            }
            std::filesystem::path user = config::userDirectory();
            for (const char* folder : { "System", "Textures", "Sounds", "Music" }) {
                directories.push_back(user / folder);
            }
            for (const std::filesystem::path& directory : directories) {
                std::error_code error;
                if (!std::filesystem::is_directory(directory, error)) {
                    continue;
                }
                std::filesystem::directory_iterator items(directory, error);
                if (error) {
                    log::warn("DevLoad", "cannot list {}: {}", displayPath(directory), error.message());
                    continue;
                }
                for (auto end = std::filesystem::directory_iterator(); items != end; items.increment(error)) {
                    if (error) {
                        log::warn("DevLoad", "stopped listing {}: {}", displayPath(directory), error.message());
                        break;
                    }
                    const std::filesystem::directory_entry& item = *items;
                    std::string extension;
                    std::string stem;
                    try {
                        extension = lowered(item.path().extension().string());
                        stem = lowered(item.path().stem().string());
                    } catch (const std::exception&) {
                        // path::string() throws for a name the narrow encoding cannot represent.
                        log::warn("DevLoad", "skipping a file in {} whose name cannot be converted", displayPath(directory));
                        continue;
                    }
                    bool known = false;
                    for (const char* candidate : extensions) {
                        if (extension == candidate) {
                            known = true;
                        }
                    }
                    if (known && directory.parent_path() == user) {
                        reg.paths[stem] = item.path();
                    } else if (known) {
                        reg.paths.emplace(stem, item.path());
                    }
                }
            }
            objectClass();
        }

        const std::filesystem::path& gameRoot()
        {
            return registry().root;
        }

        UObject* packageObject(Name packageName)
        {
            Registry& reg = registry();
            auto it = reg.packageObjects.find(packageName);
            if (it != reg.packageObjects.end()) {
                return it->second;
            }
            UObject* object = new UObject();
            object->name = packageName;
            object->flags = RF_Public;
            reg.packageObjects.emplace(packageName, object);
            registerObject(object);
            auto linker = reg.linkers.find(packageName);
            if (linker != reg.linkers.end()) {
                linker->second->packageObject = object;
                object->inLinkerTable = true;
                object->linkerShared = true;
            }
            object->cls = intrinsicClass(Name("Package"));
            if (reg.bootstrapping) {
                reg.deferredData.push_back(object);
            } else {
                allocateData(object);
            }
            return object;
        }

        UObject* findPackageObject(Name packageName)
        {
            Registry& reg = registry();
            auto it = reg.packageObjects.find(packageName);
            return it != reg.packageObjects.end() ? it->second : nullptr;
        }

        void unloadPackage(Name packageName)
        {
            Registry& reg = registry();
            auto it = reg.linkers.find(packageName);
            if (it == reg.linkers.end()) {
                return;
            }
            reg.retired.push_back(std::move(it->second));
            reg.linkers.erase(it);
            reg.packageObjects.erase(packageName);
        }

        std::vector<UObject*> unloadedPackages(bool levelsOnly)
        {
            std::vector<UObject*> result;
            for (const std::unique_ptr<Linker>& linker : registry().retired) {
                if (linker->packageObject && (!levelsOnly || linker->findExport(Name("MyLevel"), Name("Level"), "") != 0)) {
                    result.push_back(linker->packageObject);
                }
            }
            return result;
        }

        bool isLoadedLinker(const Linker* linker)
        {
            auto it = registry().linkers.find(linker->packageName);
            return it != registry().linkers.end() && it->second.get() == linker;
        }

        void releaseObjects(const std::vector<UObject*>& given, const std::vector<UObject*>& packages, bool reuseSlots)
        {
            Registry& reg = registry();
            std::vector<UObject*> objects;
            std::unordered_set<const UObject*> dead;
            for (UObject* object : given) {
                if (object && dead.insert(object).second) {
                    objects.push_back(object);
                }
            }
            // A class deletes its default object, so the default object goes with it here.
            for (size_t i = 0; i < objects.size(); i++) {
                if (UClass* cls = cast<UClass>(objects[i])) {
                    if (cls->defaultObject && dead.insert(cls->defaultObject).second) {
                        objects.push_back(cls->defaultObject);
                    }
                    cls->defaultObject = nullptr;
                }
            }
            std::unordered_set<const UObject*> released(packages.begin(), packages.end());
            std::unordered_set<const Linker*> linkers;
            for (auto it = reg.retired.begin(); it != reg.retired.end();) {
                if (released.count((*it)->packageObject)) {
                    linkers.insert(it->get());
                }
                ++it;
            }
            for (const auto& [name, linker] : reg.linkers) {
                if (released.count(linker->packageObject)) {
                    linkers.insert(linker.get());
                }
            }
            std::erase_if(reg.pending, [&](UObject* object) { return dead.count(object) != 0; });
            std::erase_if(reg.deferredData, [&](UObject* object) { return dead.count(object) != 0; });
            for (UObject* object : objects) {
                uint32_t slot = handleSlot(object->handle);
                if (slot < reg.table.size() && reg.table[slot] == object) {
                    reg.table[slot] = nullptr;
                    // Reused with the next generation, unless that would wrap around.
                    if ((reuseSlots || reg.forceReuse) && !reg.editorMode && reg.generations[slot] + 1u < HandleGenerations) {
                        reg.generations[slot]++;
                        reg.freeSlots.push_back(slot);
                    }
                }
            }
            if (!linkers.empty()) {
                for (UObject* object : reg.table) {
                    if (object && object->linker && linkers.count(object->linker)) {
                        object->linker = nullptr;
                        object->exportRef = 0;
                    }
                }
            }
            // Linkers that stay may have loaded or imported a dead object. Every object placed in
            // a linker's tables is marked, so the others need no search. One that is only its own
            // linker's export, such as an actor of the map, is cleared from that slot alone;
            // anything imported or shared means looking through every linker.
            bool inTables = false;
            for (UObject* object : objects) {
                if (!object->inLinkerTable) {
                    continue;
                }
                if (object->linkerShared || !object->linker || object->exportRef <= 0 || !object->linker->forgetExport(object)) {
                    inTables = true;
                    break;
                }
            }
            if (inTables) {
                for (const auto& [name, linker] : reg.linkers) {
                    if (!linkers.count(linker.get())) {
                        linker->forget(dead);
                    }
                }
                for (const std::unique_ptr<Linker>& linker : reg.retired) {
                    if (!linkers.count(linker.get())) {
                        linker->forget(dead);
                    }
                }
            }
            if (std::any_of(objects.begin(), objects.end(), [](const UObject* object) { return cast<UClass>(object) != nullptr; })) {
                std::erase_if(reg.intrinsics, [&](const auto& entry) { return dead.count(entry.second) != 0; });
            }
            // Property values are torn down while every class and property they need is still
            // there; then the objects themselves go.
            for (UObject* object : objects) {
                if (object->data && object->cls) {
                    object->cls->destroyValue(object->data);
                }
                freeObjectData(object->data);
                object->data = nullptr;
            }
            for (UObject* object : objects) {
                delete object;
            }
            gc::objectsReleased();
            std::erase_if(reg.packageObjects, [&](const auto& entry) { return released.count(entry.second) != 0 || dead.count(entry.second) != 0; });
            std::erase_if(reg.linkers, [&](const auto& entry) { return linkers.count(entry.second.get()) != 0; });
            std::erase_if(reg.retired, [&](const std::unique_ptr<Linker>& linker) { return linkers.count(linker.get()) != 0; });
        }

        Linker* findLinker(Name packageName)
        {
            auto it = registry().linkers.find(packageName);
            if (it == registry().linkers.end()) {
                return nullptr;
            }
            return it->second.get();
        }

        Linker* loadPackage(Name packageName)
        {
            Registry& reg = registry();
            auto it = reg.linkers.find(packageName);
            if (it != reg.linkers.end()) {
                return it->second.get();
            }
            auto path = reg.paths.find(lowered(packageName.str()));
            if (path == reg.paths.end()) {
                return nullptr;
            }
            auto linker = std::make_unique<Linker>(packageName, std::make_unique<Package>(path->second));
            Linker* result = linker.get();
            reg.linkers.emplace(packageName, std::move(linker));
            result->packageObject = packageObject(packageName);
            result->packageObject->inLinkerTable = true;
            result->packageObject->linkerShared = true;
            return result;
        }

        void setPackageFile(Name packageName, const std::filesystem::path& path)
        {
            Registry& reg = registry();
            if (path.empty()) {
                reg.paths.erase(lowered(packageName.str()));
            } else {
                reg.paths[lowered(packageName.str())] = path;
            }
        }

        std::filesystem::path packageFile(Name packageName)
        {
            Registry& reg = registry();
            auto path = reg.paths.find(lowered(packageName.str()));
            return path == reg.paths.end() ? std::filesystem::path() : path->second;
        }

        Linker* loadPackageFile(Name packageName, const std::filesystem::path& path, bool savedGame)
        {
            Registry& reg = registry();
            unloadPackage(packageName);
            auto linker = std::make_unique<Linker>(packageName, std::make_unique<Package>(path));
            Linker* result = linker.get();
            result->savedGame = savedGame;
            reg.linkers.emplace(packageName, std::move(linker));
            result->packageObject = packageObject(packageName);
            result->packageObject->inLinkerTable = true;
            result->packageObject->linkerShared = true;
            return result;
        }

        const std::vector<std::string>& loadMismatches()
        {
            return registry().loadMismatches;
        }

        void setEditorMode(bool enabled)
        {
            registry().editorMode = enabled;
        }

        bool editorMode()
        {
            return registry().editorMode;
        }

        void registerFactory(Name nativeClass, ObjectFactory factory)
        {
            registry().factories[nativeClass] = std::move(factory);
        }

        void registerObject(UObject* object)
        {
            Registry& reg = registry();
            if (reg.table.empty()) {
                reg.table.push_back(nullptr);
                reg.generations.push_back(0);
            }
            uint32_t slot = 0;
            // A collection walks the table in order and expects what it loads to come after.
            if (!reg.editorMode && !reg.freeSlots.empty() && !gc::isCollecting()) {
                slot = reg.freeSlots.front();
                reg.freeSlots.pop_front();
            } else {
                if (reg.table.size() >= reg.slotLimit) {
                    log::error("Critical", "fatal: the object table is full: {} objects. A handle has room for {}.", reg.table.size(), reg.slotLimit);
                    log::flush();
                    std::abort();
                }
                slot = static_cast<uint32_t>(reg.table.size());
                reg.table.push_back(nullptr);
                reg.generations.push_back(0);
            }
            object->handle = slot | (static_cast<uint32_t>(reg.generations[slot]) << HandleSlotBits);
            reg.table[slot] = object;
            gc::objectRegistered(object);
        }

        UObject* atHandle(uint32_t handle)
        {
            Registry& reg = registry();
            uint32_t slot = handleSlot(handle);
            if (slot == 0 || slot >= reg.table.size()) {
                return nullptr;
            }
            UObject* object = reg.table[slot];
            return object && object->handle == handle ? object : nullptr;
        }

        UObject* fromHandle(uint32_t handle, const UObject* from)
        {
            UObject* object = atHandle(handle);
            if (!object) {
                uint32_t slot = handleSlot(handle);
                Registry& reg = registry();
                if (slot != 0 && slot < reg.table.size() && reg.table[slot]) {
                    log::warn("DevGarbage", "{} names a deleted object (handle {:08x}; slot {} now holds {})", from ? from->pathName() : std::string("a handle"), handle, slot, reg.table[slot]->pathName());
                }
                return nullptr;
            }
            if (!object->loaded) {
                ensureLoaded(object);
            }
            return object;
        }

        void forceSlotReuse(bool force)
        {
            registry().forceReuse = force;
        }

        void setSlotLimit(uint32_t slots)
        {
            registry().slotLimit = std::min(slots, HandleSlotMask + 1);
        }

        const std::vector<UObject*>& all()
        {
            return registry().table;
        }

        void queueLoad(UObject* object)
        {
            registry().pending.push_back(object);
        }

        void flushLoads()
        {
            DX_ZONE_NAMED("Load queued objects");
            Registry& reg = registry();
            while (!reg.pending.empty()) {
                UObject* object = reg.pending.front();
                reg.pending.pop_front();
                ensureLoaded(object);
            }
        }

        void loadAllExports(Linker* linker)
        {
            for (size_t i = 0; i < linker->file().exports().size(); i++) {
                ensureLoaded(linker->object(static_cast<int32_t>(i + 1)));
            }
            flushLoads();
        }

        void ensureLoaded(UObject* object)
        {
            if (!object || object->loaded) {
                return;
            }
            DX_TIME("Object loading");
            object->loaded = true;
            try {
                Linker& linker = *object->linker;
                const ExportEntry& entry = linker.file().exportAt(object->exportRef);
                UClass* asClass = cast<UClass>(object);
                UClass* cls = object->cls;
                if (cls && !registry().bootstrapping) {
                    ensureLoaded(cls);
                    cls->linkClass();
                    setData(object, cls, cls->defaultObject);
                } else if (!asClass) {
                    registry().deferredData.push_back(object);
                }
                if (entry.size > 0) {
                    Reader reader(linker.file().data(), static_cast<size_t>(entry.offset));
                    int32_t stackNode = 0;
                    int32_t stackState = 0;
                    int32_t stackOffset = -1;
                    uint64_t probeMask = 0;
                    int32_t latentAction = 0;
                    if (entry.flags & RF_HasStackFlag) {
                        stackNode = reader.index();
                        stackState = reader.index();
                        probeMask = reader.u64();
                        latentAction = reader.i32();
                        if (stackNode != 0) {
                            stackOffset = reader.index();
                        }
                    }
                    if (!asClass) {
                        readTaggedProperties(object->data ? cls : nullptr, object->data, reader, linker);
                    }
                    object->serialize(reader, linker);
                    size_t end = static_cast<size_t>(entry.offset) + static_cast<size_t>(entry.size);
                    if (reader.tell() != end && typeid(*object) != typeid(UObject)) {
                        registry().loadMismatches.push_back(object->fullName() + " consumed " + std::to_string(reader.tell() - static_cast<size_t>(entry.offset)) + " of " + std::to_string(entry.size));
                    }
                    if (stackState != 0) {
                        UState* state = cast<UState>(linker.object(stackState));
                        UStruct* node = cast<UStruct>(linker.object(stackNode));
                        vm::restoreState(object, state, node, stackOffset, probeMask);
                        if (linker.savedGame && object->stateFrame) {
                            object->stateFrame->latentAction = latentAction;
                            UState* frameState = object->stateFrame->state;
                            uint64_t base = frameState ? (frameState->probeMask | cls->probeMask) & frameState->ignoreMask : cls->probeMask & cls->ignoreMask;
                            object->stateFrame->disabledProbes = base & ~probeMask;
                        }
                    }
                }
                syncObjectProperties(object);
                object->postLoad();
            } catch (const std::exception& error) {
                object->loadFailed = true;
                log::warn("DevLoad", "failed loading {}: {}", object->fullName(), error.what());
            }
        }

        // A fresh property block for the object, laid out by layout and copied from defaults
        // unless the object is those defaults. A block it already had is torn down first, by the
        // class that built it.
        void setData(UObject* object, UClass* layout, const UObject* defaults)
        {
            if (object->data && object->cls) {
                object->cls->destroyValue(object->data);
            }
            freeObjectData(object->data);
            object->data = allocateObjectData(layout->size);
            layout->initValue(object->data);
            if (defaults && defaults != object && defaults->data) {
                layout->copyValue(object->data, defaults->data);
            }
        }

        void allocateData(UObject* object)
        {
            UClass* cls = object->cls;
            if (!cls) {
                return;
            }
            if (!cls->defaultObject) {
                ensureLoaded(cls);
                cls->linkClass();
            }
            setData(object, cls, cls->defaultObject);
            syncObjectProperties(object);
        }

        void finishBootstrap()
        {
            Registry& reg = registry();
            reg.classClassPtr = intrinsicClass(Name("Class"));
            std::vector<UClass*> intrinsics;
            for (auto& [className, cls] : reg.intrinsics) {
                intrinsics.push_back(cls);
            }
            for (UClass* cls : intrinsics) {
                cls->cls = reg.classClassPtr;
                cls->linkClass();
            }
            for (UObject* object : std::vector<UObject*>(reg.table)) {
                if (object && cast<UClass>(object) && (!object->cls || !object->data)) {
                    if (!object->cls) {
                        object->cls = reg.classClassPtr;
                    }
                    allocateData(object);
                }
            }
            std::vector<UObject*> deferred = std::move(reg.deferredData);
            reg.deferredData.clear();
            for (UObject* object : deferred) {
                if (!object->cls) {
                    object->cls = reg.classClassPtr;
                }
                allocateData(object);
            }
        }

        UClass* objectClass()
        {
            Registry& reg = registry();
            if (reg.objectClassPtr) {
                return reg.objectClassPtr;
            }
            if (reg.bootstrapping) {
                Linker* core = findLinker(Name("Core"));
                if (!core) {
                    return nullptr;
                }
                return cast<UClass>(core->object(core->findExport(Name("Object"), Name("Class"), "")));
            }
            UClass* object = nullptr;
            {
                // Cleared however it ends, so a missing Core is tried again (and reported again)
                // rather than leaving every later call to take the bootstrapping path.
                ScopedValue<bool> bootstrapping(reg.bootstrapping, true);
                Linker* core = loadPackage(Name("Core"));
                if (!core) {
                    throw std::runtime_error("Core package not found");
                }
                object = cast<UClass>(core->object(core->findExport(Name("Object"), Name("Class"), "")));
                ensureLoaded(object);
                reg.objectClassPtr = object;
                reg.nameProperty = object->findProperty(Name("Name"));
                reg.classProperty = object->findProperty(Name("Class"));
                reg.outerProperty = object->findProperty(Name("Outer"));
                reg.flagsProperty = object->findProperty(Name("ObjectFlags"));
            }
            finishBootstrap();
            return object;
        }

        UClass* classClass()
        {
            Registry& reg = registry();
            if (reg.classClassPtr) {
                return reg.classClassPtr;
            }
            return intrinsicClass(Name("Class"));
        }

        bool isIntrinsicClassName(std::string_view className)
        {
            std::string key = lowered(className);
            return engineIntrinsics().count(key) || editorIntrinsics().count(key) || intrinsicSupers().count(key);
        }

        bool isIntrinsic(const UObject* object)
        {
            auto it = registry().intrinsics.find(object->name);
            return it != registry().intrinsics.end() && it->second == object;
        }

        UClass* intrinsicClass(Name className)
        {
            Registry& reg = registry();
            auto it = reg.intrinsics.find(className);
            if (it != reg.intrinsics.end()) {
                return it->second;
            }
            if (className == Name("Object")) {
                return objectClass();
            }
            UClass* cls = new UClass();
            cls->name = className;
            cls->flags = RF_Public | RF_Native;
            cls->loaded = true;
            reg.intrinsics.emplace(className, cls);
            registerObject(cls);
            if (engineIntrinsics().count(lowered(className.str()))) {
                cls->outer = packageObject(Name("Engine"));
            } else if (editorIntrinsics().count(lowered(className.str()))) {
                cls->outer = packageObject(Name("Editor"));
            } else {
                Linker* core = findLinker(Name("Core"));
                cls->outer = core ? core->packageObject : nullptr;
            }
            auto super = intrinsicSupers().find(lowered(className.str()));
            UClass* parent = nullptr;
            if (super != intrinsicSupers().end()) {
                parent = findClass(super->second);
                Name parentName(super->second);
                for (size_t i = 0; !parent && i < reg.table.size(); i++) {
                    UClass* candidate = cast<UClass>(reg.table[i]);
                    if (candidate && !candidate->destroyed && candidate->name == parentName && candidate->outer && !candidate->outer->outer) {
                        parent = candidate;
                    }
                }
                if (!parent && isIntrinsicClassName(super->second)) {
                    parent = intrinsicClass(parentName);
                }
            } else {
                parent = objectClass();
            }
            cls->superField = parent;
            cls->cls = className == Name("Class") ? cls : classClass();
            if (!reg.bootstrapping) {
                cls->linkClass();
                allocateData(cls);
            }
            return cls;
        }

        UClass* findClass(std::string_view path)
        {
            std::string text(path);
            size_t dot = text.rfind('.');
            if (dot != std::string::npos) {
                std::string packageName = text.substr(0, text.find('.'));
                Name className(text.substr(dot + 1));
                Linker* linker = loadPackage(Name(packageName));
                int32_t ref = linker ? linker->findExport(className, Name("Class"), "*") : 0;
                if (ref == 0) {
                    auto it = registry().intrinsics.find(className);
                    if (linker && it != registry().intrinsics.end()) {
                        return it->second;
                    }
                    for (UObject* object : registry().table) {
                        UClass* live = object && !object->destroyed && object->name == className ? cast<UClass>(object) : nullptr;
                        if (live && live->outermost()->name == Name(packageName)) {
                            return live;
                        }
                    }
                    return nullptr;
                }
                UClass* cls = cast<UClass>(linker->object(ref));
                ensureLoaded(cls);
                return cls;
            }
            Name className(text);
            auto intrinsic = registry().intrinsics.find(className);
            if (intrinsic != registry().intrinsics.end()) {
                return intrinsic->second;
            }
            if (intrinsicSupers().count(lowered(text))) {
                return intrinsicClass(className);
            }
            for (auto& [packageName, linker] : registry().linkers) {
                int32_t ref = linker->findExport(className, Name("Class"), "*");
                if (ref != 0) {
                    UClass* cls = cast<UClass>(linker->object(ref));
                    ensureLoaded(cls);
                    return cls;
                }
            }
            return nullptr;
        }

        static UObject* findLiveObject(const std::string& text, UClass* cls)
        {
            Name packageName(text.substr(0, text.find('.')));
            Name objectName(text.substr(text.rfind('.') + 1));
            for (UObject* object : registry().table) {
                if (object && !object->destroyed && object->outer && object->name == objectName && object->outermost()->name == packageName && (!cls || object->isA(cls))) {
                    return object;
                }
            }
            return nullptr;
        }

        UObject* findObject(std::string_view path, UClass* cls)
        {
            std::string text(path);
            size_t dot = text.find('.');
            if (dot == std::string::npos) {
                Name name(text);
                for (UObject* object : registry().table) {
                    if (object && object->name == name && (!cls || object->isA(cls))) {
                        return object;
                    }
                }
                return nullptr;
            }
            Linker* linker = findLinker(Name(text.substr(0, dot)));
            if (!linker) {
                return findLiveObject(text, cls);
            }
            size_t last = text.rfind('.');
            std::string group = last > dot ? text.substr(dot + 1, last - dot - 1) : std::string();
            Name objectName(text.substr(last + 1));
            Name className = cls ? cls->name : Name();
            int32_t ref = linker->findExport(objectName, Name(), group);
            if (ref == 0) {
                ref = linker->findExport(objectName, Name(), "*");
            }
            if (ref == 0) {
                return nullptr;
            }
            UObject* object = linker->object(ref);
            ensureLoaded(object);
            if (cls && (!object || !object->isA(cls)) && !className.none()) {
                const Package& file = linker->file();
                for (size_t i = 0; i < file.exports().size(); i++) {
                    int32_t other = static_cast<int32_t>(i + 1);
                    if (Name(file.nameAt(file.exports()[i].name)) != objectName || !iequals(file.groupOf(other), group)) {
                        continue;
                    }
                    UObject* candidate = linker->object(other);
                    ensureLoaded(candidate);
                    if (candidate && candidate->isA(cls)) {
                        return candidate;
                    }
                }
                return nullptr;
            }
            return object;
        }

        UObject* loadObject(std::string_view path, UClass* cls)
        {
            std::string text(path);
            size_t dot = text.find('.');
            if (dot != std::string::npos) {
                loadPackage(Name(text.substr(0, dot)));
            }
            if (cls == classClass()) {
                return findClass(text);
            }
            UObject* object = findObject(text, cls);
            flushLoads();
            return object;
        }

        UObject* createNative(UClass* cls)
        {
            for (UClass* current = cls; current; current = current->superClass()) {
                auto it = registry().factories.find(current->name);
                if (it != registry().factories.end()) {
                    return it->second();
                }
            }
            return new UObject();
        }

        Name uniqueName(UClass* cls)
        {
            Registry& reg = registry();
            uint32_t& counter = reg.nameCounters[cls->name];
            while (true) {
                Name candidate(cls->name.str() + std::to_string(counter++));
                if (reg.usedNames.insert(candidate).second) {
                    return candidate;
                }
            }
        }

        UObject* newObject(UClass* cls, UObject* outer, Name objectName, uint32_t flags)
        {
            ensureLoaded(cls);
            for (const UClass* current = cls; current; current = current->superClass()) {
                if (current->loadFailed) {
                    log::error("DevLoad", "cannot create an object of class {}: {} failed to load", cls->pathName(), current->pathName());
                    return nullptr;
                }
            }
            cls->linkClass();
            // Owned here until the table takes it, as making its data can throw.
            std::unique_ptr<UObject> made(createNative(cls));
            UObject* object = made.get();
            object->cls = cls;
            object->outer = outer;
            object->name = objectName.none() ? uniqueName(cls) : objectName;
            object->flags = flags;
            setData(object, cls, cls->defaultObject);
            registerObject(made.release());
            syncObjectProperties(object);
            return object;
        }

        UClass* createSubclass(UClass* parent, Name className)
        {
            ensureLoaded(parent);
            parent->linkClass();
            UClass* cls = new UClass();
            cls->name = className;
            registry().usedNames.insert(className);
            cls->cls = classClass();
            cls->outer = parent->outer;
            cls->flags = parent->flags;
            cls->superField = parent;
            cls->classFlags = parent->classFlags;
            cls->within = parent->within;
            cls->configName = parent->configName;
            setData(cls, cls->cls, nullptr);
            registerObject(cls);
            syncObjectProperties(cls);
            cls->linkClass();
            return cls;
        }

        uint64_t destroyedObjects = 0;

        void destroyObject(UObject* object)
        {
            if (object) {
                object->destroyed = true;
                destroyedObjects++;
            }
        }

        uint64_t destroyCount()
        {
            return destroyedObjects;
        }

        namespace {
            // The size is kept in front of the data, a whole alignment unit so the data stays as
            // aligned as new[] makes it.
            constexpr size_t DataHeader = __STDCPP_DEFAULT_NEW_ALIGNMENT__ > sizeof(size_t) ? __STDCPP_DEFAULT_NEW_ALIGNMENT__ : sizeof(size_t);
        }

        uint8_t* allocateObjectData(size_t size)
        {
            size = std::max<size_t>(size, 1);
            uint8_t* block = new uint8_t[DataHeader + size];
            std::memcpy(block, &size, sizeof(size));
            gc::dataAllocated(size);
            return block + DataHeader;
        }

        void freeObjectData(uint8_t* data)
        {
            if (!data) {
                return;
            }
            uint8_t* block = data - DataHeader;
            size_t size = 0;
            std::memcpy(&size, block, sizeof(size));
            gc::dataFreed(size);
            delete[] block;
        }

        LinkerStats linkerStats()
        {
            Registry& reg = registry();
            LinkerStats stats;
            for (const auto& [name, linker] : reg.linkers) {
                stats.loaded++;
                stats.loadedBytes += linker->file().data().size();
            }
            for (const std::unique_ptr<Linker>& linker : reg.retired) {
                stats.retired++;
                stats.retiredBytes += linker->file().data().size();
            }
            return stats;
        }

        void syncObjectProperties(UObject* object)
        {
            Registry& reg = registry();
            if (!object->data || !reg.nameProperty || !object->cls || object->cls->size < reg.nameProperty->offset + 4) {
                return;
            }
            *reinterpret_cast<uint32_t*>(object->data + reg.nameProperty->offset) = object->name.index();
            if (reg.classProperty) {
                *reinterpret_cast<UObject**>(object->data + reg.classProperty->offset) = object->cls;
            }
            if (reg.outerProperty) {
                *reinterpret_cast<UObject**>(object->data + reg.outerProperty->offset) = object->outer;
            }
            if (reg.flagsProperty) {
                *reinterpret_cast<uint32_t*>(object->data + reg.flagsProperty->offset) = object->flags;
            }
        }

        namespace {
            void readStructBinary(UStruct* structType, uint8_t* memory, Reader& reader, Linker& linker);

            void readBinaryValue(UProperty* property, uint8_t* memory, Reader& reader, Linker& linker)
            {
                switch (property->type) {
                case PropType::Byte:
                    *memory = reader.u8();
                    break;
                case PropType::Int:
                    *reinterpret_cast<int32_t*>(memory) = reader.i32();
                    break;
                case PropType::Bool:
                    *reinterpret_cast<uint32_t*>(memory) = reader.u8() != 0 ? 1 : 0;
                    break;
                case PropType::Float:
                    *reinterpret_cast<float*>(memory) = reader.f32();
                    break;
                case PropType::Object:
                case PropType::Class:
                    *reinterpret_cast<UObject**>(memory) = linker.readObject(reader);
                    break;
                case PropType::Name:
                    *reinterpret_cast<uint32_t*>(memory) = linker.readName(reader).index();
                    break;
                case PropType::Str:
                case PropType::String:
                    *reinterpret_cast<std::string*>(memory) = reader.string();
                    break;
                case PropType::Struct:
                    readStructBinary(property->structType, memory, reader, linker);
                    break;
                case PropType::Array:
                case PropType::Map:
                case PropType::FixedArray:
                case PropType::Pointer:
                    throw std::runtime_error("unsupported binary property type");
                }
            }

            void readStructBinary(UStruct* structType, uint8_t* memory, Reader& reader, Linker& linker)
            {
                for (UProperty* member : structType->fieldOrder()) {
                    if (member->propertyFlags & (CPF_Native | CPF_Transient)) {
                        continue;
                    }
                    for (int32_t i = 0; i < member->arrayDim; i++) {
                        readBinaryValue(member, memory + member->offset + static_cast<size_t>(i) * member->elementSize(), reader, linker);
                    }
                }
            }
        }

        void readTaggedProperties(UStruct* structType, uint8_t* data, Reader& reader, Linker& linker)
        {
            while (true) {
                Name propertyName = linker.readName(reader);
                if (propertyName.none()) {
                    break;
                }
                uint8_t info = reader.u8();
                PropertyType tagType = static_cast<PropertyType>(info & 0x0f);
                Name structName;
                if (tagType == PropertyType::Struct) {
                    structName = linker.readName(reader);
                }
                size_t size = readTagSize(reader, info);
                int32_t arrayIndex = 0;
                bool infoBit = (info & 0x80) != 0;
                if (tagType == PropertyType::Bool) {
                    size = 0;
                } else if (infoBit) {
                    arrayIndex = readTagArrayIndex(reader);
                }
                size_t start = reader.tell();

                UProperty* property = nullptr;
                if (!structType || !data) {
                    reader.seek(start + size);
                    continue;
                }
                for (auto it = structType->properties.rbegin(); it != structType->properties.rend(); ++it) {
                    if ((*it)->name == propertyName) {
                        if (tagType != PropertyType::Struct || (*it)->type == PropType::Struct) {
                            property = *it;
                            break;
                        }
                    }
                }
                if (!property || arrayIndex < 0 || arrayIndex >= property->arrayDim) {
                    reader.seek(start + size);
                    continue;
                }
                uint8_t* memory = data + property->offset + static_cast<size_t>(arrayIndex) * property->elementSize();
                try {
                    switch (tagType) {
                    case PropertyType::Byte:
                        if (property->type == PropType::Byte) {
                            *memory = reader.u8();
                        }
                        break;
                    case PropertyType::Int:
                        if (property->type == PropType::Int) {
                            *reinterpret_cast<int32_t*>(memory) = reader.i32();
                        }
                        break;
                    case PropertyType::Bool:
                        if (property->type == PropType::Bool) {
                            *reinterpret_cast<uint32_t*>(memory) = infoBit ? 1 : 0;
                        }
                        break;
                    case PropertyType::Float:
                        if (property->type == PropType::Float) {
                            *reinterpret_cast<float*>(memory) = reader.f32();
                        }
                        break;
                    case PropertyType::Object:
                    case PropertyType::Class:
                        if (property->type == PropType::Object || property->type == PropType::Class) {
                            *reinterpret_cast<UObject**>(memory) = linker.readObject(reader);
                        }
                        break;
                    case PropertyType::Name:
                        if (property->type == PropType::Name) {
                            *reinterpret_cast<uint32_t*>(memory) = linker.readName(reader).index();
                        }
                        break;
                    case PropertyType::Str:
                        if (property->type == PropType::Str || property->type == PropType::String) {
                            *reinterpret_cast<std::string*>(memory) = reader.string();
                        }
                        break;
                    case PropertyType::String:
                        if (property->type == PropType::Str || property->type == PropType::String) {
                            if (size > reader.remaining()) {
                                throw std::runtime_error("string property past end of package data");
                            }
                            std::string text(size, '\0');
                            reader.bytes(text.data(), size);
                            text.resize(std::strlen(text.c_str()));
                            *reinterpret_cast<std::string*>(memory) = text;
                        }
                        break;
                    case PropertyType::Struct:
                        if (property->type == PropType::Struct && property->structType) {
                            readStructBinary(property->structType, memory, reader, linker);
                        }
                        break;
                    case PropertyType::Vector:
                    case PropertyType::Rotator:
                    case PropertyType::Array:
                    case PropertyType::Map:
                    case PropertyType::FixedArray:
                        break;
                    }
                } catch (const std::exception& error) {
                    log::warn("DevLoad", "failed reading property {} of {}: {}", propertyName.str(), structType->pathName(), error.what());
                }
                reader.seek(start + size);
            }
        }
    }
}

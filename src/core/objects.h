#pragma once

#include "core/object.h"
#include "core/package.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dx {
    class Linker {
    public:
        Linker(Name name, std::unique_ptr<Package> file);

        UObject* object(int32_t ref);
        void rebind(int32_t ref, UObject* object);
        Name name(int32_t index) const;
        UObject* readObject(Reader& reader);
        Name readName(Reader& reader);
        int32_t findExport(Name objectName, Name className, std::string_view group) const;
        const Package& file() const;
        // Drops the linker's pointers to objects about to be deleted. An export is made again if
        // it is asked for later; an import is looked up again.
        void forget(const std::unordered_set<const UObject*>& dead);
        // Clears the export slot an object was made in, if it is still there. False when it is
        // not, so the caller looks through every linker instead.
        bool forgetExport(const UObject* object);

        Name packageName;
        UObject* packageObject = nullptr;
        bool savedGame = false;
        uint64_t loadOrder = 0;

    private:
        UObject* createExport(int32_t ref);
        UObject* resolveImport(int32_t ref);

        std::unique_ptr<Package> package;
        std::vector<Name> names;
        std::vector<UObject*> exports;
        std::vector<UObject*> imports;
        std::vector<bool> importResolved;
    };

    using ObjectFactory = std::function<UObject*()>;

    namespace objects {
        // A handle is an object's slot in the table in its low bits and the slot's generation in
        // its high bits. A slot freed while reuse is on is handed out again, oldest first, with
        // the next generation, so a handle kept past its object's deletion no longer matches
        // and reads as None. A slot whose generations run out is never reused.
        constexpr uint32_t HandleSlotBits = 22;
        constexpr uint32_t HandleSlotMask = (1u << HandleSlotBits) - 1;
        constexpr uint32_t HandleGenerations = 1u << (32 - HandleSlotBits);

        constexpr uint32_t handleSlot(uint32_t handle)
        {
            return handle & HandleSlotMask;
        }

        constexpr uint32_t handleGeneration(uint32_t handle)
        {
            return handle >> HandleSlotBits;
        }

        void initialize(const std::filesystem::path& gameRoot);
        const std::filesystem::path& gameRoot();
        Linker* loadPackage(Name packageName);
        Linker* loadPackageFile(Name packageName, const std::filesystem::path& path, bool savedGame);
        void setPackageFile(Name packageName, const std::filesystem::path& path);
        // The file loadPackage would open for a package, or an empty path.
        std::filesystem::path packageFile(Name packageName);
        Linker* findLinker(Name packageName);
        void unloadPackage(Name packageName);
        // The package object of that name, made and registered if there is none yet.
        UObject* packageObject(Name packageName);
        // The package object of that name, or None; never makes one.
        UObject* findPackageObject(Name packageName);
        // The package objects of packages unloaded with unloadPackage and not yet released;
        // with levelsOnly, just those that hold a level.
        std::vector<UObject*> unloadedPackages(bool levelsOnly = false);
        // Deletes the objects and the linkers of the packages (unloaded or still loaded). Only the
        // garbage collector (core/gc.h) calls this, once nothing refers to the objects. Their
        // table slots become null, other linkers forget them, and a kept object that came from
        // one of the released linkers is cut loose from it. A class's default object goes with
        // the class.
        // With reuseSlots, the freed slots are handed out again (never in the editor).
        void releaseObjects(const std::vector<UObject*>& objects, const std::vector<UObject*>& packages, bool reuseSlots = false);
        // True for a linker of a package that is loaded, not unloaded and waiting for release.
        bool isLoadedLinker(const Linker* linker);
        const std::vector<std::string>& loadMismatches();
        void setEditorMode(bool enabled);
        bool editorMode();
        void registerFactory(Name nativeClass, ObjectFactory factory);
        void registerObject(UObject* object);
        // The object a handle names, loading it if needed; None for a handle whose object was
        // deleted, with a warning naming `from` when its slot now holds another object.
        UObject* fromHandle(uint32_t handle, const UObject* from = nullptr);
        // The object a handle names, without loading it or warning; None when it is gone.
        UObject* atHandle(uint32_t handle);
        // For tests: reuse the slots of every release, as if everything reported its references.
        void forceSlotReuse(bool force);
        // The number of slots the table may grow to; for tests of running out.
        void setSlotLimit(uint32_t slots);
        // The table, by slot. Slots of deleted objects hold None.
        const std::vector<UObject*>& all();
        void ensureLoaded(UObject* object);
        void flushLoads();
        // Loads every export of a package, then whatever loading them queued.
        void loadAllExports(Linker* linker);
        void queueLoad(UObject* object);
        UClass* findClass(std::string_view path);
        UClass* intrinsicClass(Name className);
        bool isIntrinsic(const UObject* object);
        bool isIntrinsicClassName(std::string_view className);
        UObject* findObject(std::string_view path, UClass* cls);
        UObject* loadObject(std::string_view path, UClass* cls);
        UObject* newObject(UClass* cls, UObject* outer, Name objectName, uint32_t flags);
        UClass* createSubclass(UClass* parent, Name className);
        UObject* createNative(UClass* cls);
        void destroyObject(UObject* object);
        uint64_t destroyCount();
        // An object's script property data: at least one byte, its size kept beside it so that
        // what is made and deleted can be counted (gc::dataAllocated, gc::dataFreed). Every
        // UObject::data comes from allocateObjectData and goes through freeObjectData.
        uint8_t* allocateObjectData(size_t size);
        void freeObjectData(uint8_t* data);

        // Package file images held by loaded linkers and by unloaded ones kept in `retired`.
        struct LinkerStats {
            size_t loaded = 0;
            size_t retired = 0;
            uint64_t loadedBytes = 0;
            uint64_t retiredBytes = 0;
        };

        LinkerStats linkerStats();
        Name uniqueName(UClass* cls);
        void readTaggedProperties(UStruct* structType, uint8_t* data, Reader& reader, Linker& linker);
        void syncObjectProperties(UObject* object);
        UClass* classClass();
        UClass* objectClass();
    }
}

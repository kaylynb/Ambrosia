#include "engine/level_release.h"

#include "core/gc.h"
#include "core/log.h"
#include "core/object.h"
#include "core/objects.h"
#include "engine/actor.h"
#include "engine/ai.h"
#include "engine/mesh.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dx::levelrelease {
    namespace {
        struct State {
            // Packages that hold classes, structs, functions, meshes or fonts. While some
            // library does not report its references, such a package is never released.
            std::unordered_set<const UObject*> pinned;
            // A package object deleted by a collection leaves the set, so another object made
            // at its address is not taken for it.
            gc::ReleaseHook forgetPinned { [this](const gc::ReleasedSet& released) {
                std::erase_if(pinned, [&](const UObject* package) { return released.count(package) != 0; });
            } };
        };

        State& state()
        {
            static State instance;
            return instance;
        }

        constexpr std::string_view SavePrefix = "SaveGame_";

        bool sameText(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) {
                return false;
            }
            for (size_t i = 0; i < a.size(); i++) {
                char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
                char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
                if (x != y) {
                    return false;
                }
            }
            return true;
        }

        // The map a package belongs to: SaveGame_X and X are the same level.
        std::string_view mapOf(std::string_view packageName)
        {
            if (packageName.size() > SavePrefix.size() && sameText(packageName.substr(0, SavePrefix.size()), SavePrefix)) {
                return packageName.substr(SavePrefix.size());
            }
            return packageName;
        }

        const UObject* topOf(const UObject* object)
        {
            int guard = 0;
            const UObject* top = object;
            while (top->outer && guard++ < 64) {
                top = top->outer;
            }
            return top;
        }

        bool isActor(const UObject* object)
        {
            return engineClasses().actor && object->cls && object->isA(engineClasses().actor);
        }

        // An actor of a level, rather than the default object of an Actor class, which shares
        // the class's outer but belongs to the class.
        bool isLevelActor(const UObject* object)
        {
            return isActor(object) && object->cls->defaultObject != object;
        }

        // Objects that only mean something inside their level. A reference to one is set to
        // None rather than keeping it, as travel already does for actors.
        bool levelBound(const UObject* object, const std::unordered_set<const UObject*>& packages)
        {
            if (packages.count(object) || isLevelActor(object)) {
                return true;
            }
            return cast<ULevel>(object) || cast<UModel>(object) || cast<UPolys>(object) || cast<UAIEvent>(object) || cast<UAIEventType>(object) || cast<UEventManager>(object);
        }
    }

    void setEnabled(bool enabled)
    {
        gc::setEnabled(enabled);
    }

    bool enabled()
    {
        return gc::enabled();
    }

    Result collect(World* current)
    {
        State& s = state();
        Result result;
        if (!gc::enabled()) {
            return result;
        }
        objects::flushLoads();

        // A level restored from a save pulls in the map's own package for its textures and
        // sounds. That package goes with the save unless the level now loaded is the same map.
        UObject* currentPackage = current && current->level ? current->level->outermost() : nullptr;
        std::string currentMap = currentPackage ? std::string(mapOf(currentPackage->name.str())) : std::string();
        for (UObject* package : objects::unloadedPackages(true)) {
            std::string name = package->name.str();
            std::string_view map = mapOf(name);
            if (map.size() == name.size() || sameText(map, currentMap)) {
                continue;
            }
            Linker* base = objects::findLinker(Name(std::string(map)));
            if (base && base->packageObject != currentPackage) {
                objects::unloadPackage(base->packageName);
            }
        }

        bool reported = gc::everythingReported();
        std::unordered_set<const UObject*> packages;
        for (UObject* package : objects::unloadedPackages(true)) {
            // Once everything reports its references, pinned packages go like any other.
            if (package != currentPackage && (!s.pinned.count(package) || reported)) {
                packages.insert(package);
            }
        }
        // Until everything reports its references, a package holding classes, meshes or fonts
        // is left whole: their pointers into it are not all known.
        if (!packages.empty() && !reported) {
            for (UObject* object : objects::all()) {
                if (!object || !(cast<UField>(object) || cast<UMesh>(object) || cast<UFont>(object))) {
                    continue;
                }
                const UObject* top = topOf(object);
                if (packages.erase(top)) {
                    s.pinned.insert(top);
                    result.pinned++;
                }
            }
        }

        // LoadMap sets RF_EliminateObject on the new level's actors that are not in its actor
        // list, so references to them become None. Ambrosia does it for the destroyed ones;
        // World::cleanupDestroyed has already cleared most of those references.
        std::unordered_set<const UObject*> listed;
        if (current) {
            listed.insert(current->actors.begin(), current->actors.end());
        }

        gc::Options options;
        // Once everything reports, the collection is LoadMap's whole one: anything unreached that
        // is not native and not part of a package still loaded goes too.
        options.candidate = [&](const UObject* object) {
            return packages.count(topOf(object)) != 0 || (reported && gc::isDefaultCandidate(object));
        };
        options.eliminate = [&](const UObject* object) {
            if (packages.count(topOf(object))) {
                return levelBound(object, packages);
            }
            return currentPackage && object->destroyed && topOf(object) == currentPackage && isLevelActor(object) && !listed.count(object);
        };
        options.keepInners = true;
        options.loadKept = true;
        // What travel carried was already moved under the new player (World::rehomeCarried); any
        // other kept object whose outer is going moves to the transient package. It is logged:
        // something still reached an object of the old level that travel did not carry.
        size_t orphaned = 0;
        options.adoptOuter = [&](UObject* outer) -> UObject* {
            if (orphaned++ < 32) {
                log::warn("DevGarbage", "levelrelease: an object inside {} outlives the level it came from; moved to Transient", outer->fullName());
            }
            return nullptr;
        };
        for (const UObject* package : packages) {
            options.packages.push_back(const_cast<UObject*>(package));
        }
        gc::Result collected = gc::collect(options);
        result.packages = options.packages.size();
        result.freed = collected.freed;
        result.kept = collected.kept;
        result.cleared = collected.cleared;
        return result;
    }
}

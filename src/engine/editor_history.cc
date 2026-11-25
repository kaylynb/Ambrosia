#include "engine/editor_history.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace dx {
    namespace {
        template <typename T>
        size_t bytesOf(const std::vector<T>& values)
        {
            return values.size() * sizeof(T);
        }

        size_t polyBytes(const std::vector<Poly>& polys)
        {
            size_t bytes = bytesOf(polys);
            for (const Poly& poly : polys) {
                bytes += bytesOf(poly.vertices);
            }
            return bytes;
        }
    }

    std::shared_ptr<uint8_t> EditorHistory::capture(UObject* object, size_t& bytes)
    {
        if (!object || !object->cls || !object->data) {
            return {};
        }
        UClass* cls = object->cls;
        bytes += cls->size;
        std::unique_ptr<uint8_t[]> block(new uint8_t[std::max<size_t>(cls->size, 1)]);
        cls->initValue(block.get());
        // Owned by the snapshot once initialised, so a throwing copy still tears it down.
        std::shared_ptr<uint8_t> copy(block.release(), [cls](uint8_t* memory) {
            cls->destroyValue(memory);
            delete[] memory;
        });
        cls->copyValue(copy.get(), object->data);
        return copy;
    }

    void EditorHistory::restore(UObject* object, const std::shared_ptr<uint8_t>& data)
    {
        if (!object || !object->cls || !object->data || !data) {
            return;
        }
        object->cls->copyValue(object->data, data.get());
    }

    std::shared_ptr<const EditorHistory::LightState> EditorHistory::captureLight(const UModel& model, size_t& bytes)
    {
        bool same = lastLight && lastLight->lightMaps.size() == model.lightMaps.size() && lastLight->lightBits == model.lightBits
            && (model.lightMaps.empty() || std::memcmp(lastLight->lightMaps.data(), model.lightMaps.data(), bytesOf(model.lightMaps)) == 0);
        if (!same) {
            auto state = std::make_shared<LightState>();
            state->lightMaps = model.lightMaps;
            state->lightBits = model.lightBits;
            bytes += bytesOf(state->lightMaps) + bytesOf(state->lightBits);
            lastLight = state;
        }
        return lastLight;
    }

    std::shared_ptr<EditorHistory::NativeState> EditorHistory::captureNative(UObject* object, size_t& bytes)
    {
        if (UModel* model = cast<UModel>(object)) {
            auto state = std::make_shared<NativeState>();
            state->vectors = model->vectors;
            state->points = model->points;
            state->nodes = model->nodes;
            state->surfs = model->surfs;
            state->verts = model->verts;
            state->numSharedSides = model->numSharedSides;
            state->zones = model->zones;
            state->polys = model->polys;
            state->light = captureLight(*model, bytes);
            state->nodeBounds = model->nodeBounds;
            state->leafHulls = model->leafHulls;
            state->leaves = model->leaves;
            state->lights = model->lights;
            state->rootOutside = model->rootOutside;
            state->linked = model->linked;
            state->bounds = model->bounds;
            state->sphere = model->sphere;
            bytes += sizeof(NativeState) + bytesOf(state->vectors) + bytesOf(state->points) + bytesOf(state->nodes) + bytesOf(state->surfs) + bytesOf(state->verts) + bytesOf(state->zones)
                + bytesOf(state->nodeBounds) + bytesOf(state->leafHulls) + bytesOf(state->leaves) + bytesOf(state->lights);
            return state;
        }
        if (UPolys* polys = cast<UPolys>(object)) {
            auto state = std::make_shared<NativeState>();
            state->polyList = polys->polys;
            bytes += sizeof(NativeState) + polyBytes(state->polyList);
            return state;
        }
        return {};
    }

    void EditorHistory::restoreNative(UObject* object, const std::shared_ptr<NativeState>& state)
    {
        if (!state) {
            return;
        }
        if (UModel* model = cast<UModel>(object)) {
            model->vectors = state->vectors;
            model->points = state->points;
            model->nodes = state->nodes;
            model->surfs = state->surfs;
            model->verts = state->verts;
            model->numSharedSides = state->numSharedSides;
            model->zones = state->zones;
            model->polys = state->polys;
            if (state->light) {
                model->lightMaps = state->light->lightMaps;
                model->lightBits = state->light->lightBits;
            }
            model->nodeBounds = state->nodeBounds;
            model->leafHulls = state->leafHulls;
            model->leaves = state->leaves;
            model->lights = state->lights;
            model->rootOutside = state->rootOutside;
            model->linked = state->linked;
            model->bounds = state->bounds;
            model->sphere = state->sphere;
            model->revision++;
        } else if (UPolys* polys = cast<UPolys>(object)) {
            polys->polys = state->polyList;
        }
    }

    void EditorHistory::addNativeReferences(NativeState& state, ReferenceVisitor& references)
    {
        for (BspSurf& surf : state.surfs) {
            references(surf.texture);
            references(surf.actor);
        }
        for (ZoneProperties& zone : state.zones) {
            references(zone.zoneActor);
        }
        references(state.polys);
        references(state.lights);
        for (Poly& poly : state.polyList) {
            references(poly.actor);
            references(poly.texture);
        }
    }

    void EditorHistory::addTransactionReferences(Transaction& transaction, ReferenceVisitor& references)
    {
        references(transaction.level);
        references(transaction.actorsBefore);
        references(transaction.actorsAfter);
        for (Snapshot& snapshot : transaction.snapshots) {
            references(snapshot.object);
            references(snapshot.cls);
            // The copies are made by the class they are laid out as, which frees them too.
            for (const std::shared_ptr<uint8_t>& copy : { snapshot.before, snapshot.after }) {
                if (copy && snapshot.cls) {
                    gc::addPropertyReferences(snapshot.cls, copy.get(), references);
                }
            }
            for (const std::shared_ptr<NativeState>& state : { snapshot.nativeBefore, snapshot.nativeAfter }) {
                if (state) {
                    addNativeReferences(*state, references);
                }
            }
        }
    }

    void EditorHistory::addReferences(ReferenceVisitor& references)
    {
        for (Transaction& transaction : done) {
            addTransactionReferences(transaction, references);
        }
        for (Transaction& transaction : undone) {
            addTransactionReferences(transaction, references);
        }
        if (pending) {
            addTransactionReferences(*pending, references);
        }
    }

    void EditorHistory::begin(ULevel* level, const std::vector<UObject*>& objects, const std::string& label)
    {
        pending = std::make_unique<Transaction>();
        pending->label = label;
        pending->level = level;
        if (level) {
            pending->actorsBefore = level->actors;
            pending->bytes += bytesOf(pending->actorsBefore);
        }
        std::unordered_set<UObject*> seen;
        for (UObject* object : objects) {
            if (!object || !seen.insert(object).second) {
                continue;
            }
            Snapshot snapshot;
            snapshot.object = object;
            snapshot.cls = object->cls;
            snapshot.before = capture(object, pending->bytes);
            snapshot.nativeBefore = captureNative(object, pending->bytes);
            snapshot.destroyedBefore = object->destroyed;
            pending->snapshots.push_back(snapshot);
        }
    }

    void EditorHistory::commit()
    {
        if (!pending) {
            return;
        }
        if (pending->level) {
            pending->actorsAfter = pending->level->actors;
            pending->bytes += bytesOf(pending->actorsAfter);
        }
        for (Snapshot& snapshot : pending->snapshots) {
            // A collection sets a deleted object's entry to None.
            if (!snapshot.object) {
                continue;
            }
            snapshot.after = capture(snapshot.object, pending->bytes);
            snapshot.nativeAfter = captureNative(snapshot.object, pending->bytes);
            snapshot.destroyedAfter = snapshot.object->destroyed;
        }
        pending->bytes += sizeof(Transaction) + pending->label.size() + bytesOf(pending->snapshots);
        for (const Transaction& transaction : undone) {
            retained -= transaction.bytes;
        }
        undone.clear();
        retained += pending->bytes;
        done.push_back(std::move(*pending));
        pending.reset();
        changes++;
        trim();
    }

    void EditorHistory::trim()
    {
        size_t drop = 0;
        while (done.size() - drop > 1 && (done.size() - drop > maxEntries || retained > maxBytes)) {
            retained -= done[drop].bytes;
            drop++;
        }
        done.erase(done.begin(), done.begin() + static_cast<std::ptrdiff_t>(drop));
    }

    void EditorHistory::setLimits(size_t entries, size_t bytes)
    {
        maxEntries = std::max<size_t>(entries, 1);
        maxBytes = bytes;
        trim();
    }

    size_t EditorHistory::retainedBytes() const
    {
        return retained;
    }

    void EditorHistory::cancel()
    {
        pending.reset();
    }

    void EditorHistory::apply(const Transaction& transaction, bool forward)
    {
        if (transaction.level) {
            const std::vector<UObject*>& target = forward ? transaction.actorsAfter : transaction.actorsBefore;
            const std::vector<UObject*>& other = forward ? transaction.actorsBefore : transaction.actorsAfter;
            std::unordered_set<UObject*> kept(target.begin(), target.end());
            for (UObject* object : other) {
                if (object && !kept.count(object)) {
                    object->destroyed = true;
                }
            }
            for (UObject* object : target) {
                if (object) {
                    object->destroyed = false;
                }
            }
            transaction.level->actors = target;
        }
        for (const Snapshot& snapshot : transaction.snapshots) {
            if (!snapshot.object) {
                continue;
            }
            restore(snapshot.object, forward ? snapshot.after : snapshot.before);
            restoreNative(snapshot.object, forward ? snapshot.nativeAfter : snapshot.nativeBefore);
            snapshot.object->destroyed = forward ? snapshot.destroyedAfter : snapshot.destroyedBefore;
        }
    }

    bool EditorHistory::undo()
    {
        if (done.empty()) {
            return false;
        }
        Transaction transaction = std::move(done.back());
        done.pop_back();
        apply(transaction, false);
        undone.push_back(std::move(transaction));
        changes++;
        return true;
    }

    bool EditorHistory::redo()
    {
        if (undone.empty()) {
            return false;
        }
        Transaction transaction = std::move(undone.back());
        undone.pop_back();
        apply(transaction, true);
        done.push_back(std::move(transaction));
        changes++;
        return true;
    }

    void EditorHistory::clear()
    {
        done.clear();
        undone.clear();
        pending.reset();
        lastLight.reset();
        retained = 0;
    }

    size_t EditorHistory::undoCount() const
    {
        return done.size();
    }

    size_t EditorHistory::redoCount() const
    {
        return undone.size();
    }

    bool EditorHistory::recording() const
    {
        return pending != nullptr;
    }

    uint64_t EditorHistory::revision() const
    {
        return changes;
    }

    std::string EditorHistory::nextUndo() const
    {
        return done.empty() ? std::string() : done.back().label;
    }

    std::string EditorHistory::nextRedo() const
    {
        return undone.empty() ? std::string() : undone.back().label;
    }
}

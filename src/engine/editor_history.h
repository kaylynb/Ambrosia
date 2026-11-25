#pragma once

#include "core/gc.h"
#include "engine/resources.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dx {
    // Undo and redo keep alive everything they could put back: the level, its actor lists, each
    // object recorded and what its saved copies refer to.
    class EditorHistory final : public gc::Holder {
    public:
        EditorHistory() = default;
        void addReferences(ReferenceVisitor& references) override;

        void begin(ULevel* level, const std::vector<UObject*>& objects, const std::string& label);
        void commit();
        void cancel();
        bool undo();
        bool redo();
        void clear();
        size_t undoCount() const;
        size_t redoCount() const;
        bool recording() const;
        uint64_t revision() const;
        std::string nextUndo() const;
        std::string nextRedo() const;
        // Oldest transactions are dropped once either limit is passed; the newest is always kept.
        void setLimits(size_t entries, size_t bytes);
        size_t retainedBytes() const;

        static constexpr size_t DefaultMaxEntries = 1000;
        static constexpr size_t DefaultMaxBytes = size_t(1) << 30;

    private:
        // Light maps are shared between snapshots while they are unchanged, so surface and CSG
        // edits do not copy the light data twice per transaction.
        struct LightState {
            std::vector<LightMapIndex> lightMaps;
            std::vector<uint8_t> lightBits;
        };

        struct NativeState {
            std::vector<Vector> vectors;
            std::vector<Vector> points;
            std::vector<BspNode> nodes;
            std::vector<BspSurf> surfs;
            std::vector<BspVert> verts;
            int32_t numSharedSides = 0;
            std::vector<ZoneProperties> zones;
            UPolys* polys = nullptr;
            std::shared_ptr<const LightState> light;
            std::vector<BoundingBox> nodeBounds;
            std::vector<int32_t> leafHulls;
            std::vector<ConvexLeaf> leaves;
            std::vector<UObject*> lights;
            int32_t rootOutside = 0;
            int32_t linked = 0;
            BoundingBox bounds;
            Plane sphere;
            std::vector<Poly> polyList;
        };

        struct Snapshot {
            UObject* object = nullptr;
            // The class `before` and `after` are laid out as.
            UClass* cls = nullptr;
            std::shared_ptr<uint8_t> before;
            std::shared_ptr<uint8_t> after;
            std::shared_ptr<NativeState> nativeBefore;
            std::shared_ptr<NativeState> nativeAfter;
            bool destroyedBefore = false;
            bool destroyedAfter = false;
        };

        struct Transaction {
            std::string label;
            ULevel* level = nullptr;
            std::vector<UObject*> actorsBefore;
            std::vector<UObject*> actorsAfter;
            std::vector<Snapshot> snapshots;
            size_t bytes = 0;
        };

        static std::shared_ptr<uint8_t> capture(UObject* object, size_t& bytes);
        static void restore(UObject* object, const std::shared_ptr<uint8_t>& data);
        std::shared_ptr<NativeState> captureNative(UObject* object, size_t& bytes);
        std::shared_ptr<const LightState> captureLight(const UModel& model, size_t& bytes);
        static void restoreNative(UObject* object, const std::shared_ptr<NativeState>& state);
        static void addTransactionReferences(Transaction& transaction, ReferenceVisitor& references);
        static void addNativeReferences(NativeState& state, ReferenceVisitor& references);
        void apply(const Transaction& transaction, bool forward);
        void trim();

        std::vector<Transaction> done;
        std::vector<Transaction> undone;
        std::unique_ptr<Transaction> pending;
        std::shared_ptr<const LightState> lastLight;
        uint64_t changes = 0;
        size_t retained = 0;
        size_t maxEntries = DefaultMaxEntries;
        size_t maxBytes = DefaultMaxBytes;
    };
}

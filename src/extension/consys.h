#pragma once

#include "core/gc.h"
#include "core/objects.h"
#include "core/reader.h"

#include <string>
#include <vector>

namespace dx::extension {
    class UConAudioList : public UObject {
    public:
        void serialize(Reader& reader, Linker& source) override
        {
            int32_t count = reader.index();
            for (int32_t i = 0; i < count; i++) {
                sounds.push_back(source.readObject(reader));
            }
        }

        // The speech sounds of a conversation package, found by index while it plays.
        void addReferences(ReferenceVisitor& references) override
        {
            UObject::addReferences(references);
            references(sounds);
        }

        std::vector<UObject*> sounds;
    };

    // Holds only label names.
    class UConEventRandomLabel : public UObject {
    public:
        void serialize(Reader& reader, Linker&) override
        {
            int32_t count = reader.index();
            for (int32_t i = 0; i < count; i++) {
                labels.push_back(reader.string());
            }
        }

        std::vector<std::string> labels;
    };
}

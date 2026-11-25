#include "engine/props_bind.h"

#include "core/objects.h"
#include "engine/props_consys.h"
#include "engine/props_deusex.h"
#include "engine/props_engine.h"
#include "engine/props_extension.h"
#include "engine/props_fire.h"

#include <span>
#include <string_view>
#include <utility>

namespace dx {
    namespace {
        bool bound = false;

        std::string describe(PropType type, size_t elementSize, int32_t arrayDim, const char* structName)
        {
            std::string text = propTypeName(type);
            if (structName) {
                text += std::string(" ") + structName;
            }
            if (elementSize) {
                text += ", " + std::to_string(elementSize) + (elementSize == 1 ? " byte" : " bytes");
            }
            text += ", " + std::to_string(arrayDim) + (arrayDim == 1 ? " element" : " elements");
            return text;
        }
    }

    const char* propTypeName(PropType type)
    {
        switch (type) {
        case PropType::Byte:
            return "byte";
        case PropType::Int:
            return "int";
        case PropType::Bool:
            return "bool";
        case PropType::Float:
            return "float";
        case PropType::Object:
            return "object";
        case PropType::Class:
            return "class";
        case PropType::Name:
            return "name";
        case PropType::Str:
            return "string";
        case PropType::String:
            return "fixed string";
        case PropType::Struct:
            return "struct";
        case PropType::Array:
            return "array";
        case PropType::Map:
            return "map";
        case PropType::FixedArray:
            return "fixed array";
        case PropType::Pointer:
            return "pointer";
        }
        return "unknown";
    }

    bool bindClasses(const ClassBinding* classes, size_t count, std::vector<std::string>& failures)
    {
        size_t before = failures.size();
        std::vector<std::pair<size_t*, size_t>> found;
        for (size_t c = 0; c < count; c++) {
            const ClassBinding& binding = classes[c];
            UClass* cls = objects::findClass(binding.path);
            for (size_t p = 0; p < binding.count; p++) {
                const PropBinding& entry = binding.properties[p];
                std::string where = std::string(binding.path) + "." + entry.name;
                std::string expected = describe(entry.type, entry.elementSize, entry.arrayDim, entry.structName);
                UProperty* property = cls ? cls->findProperty(Name(entry.name)) : nullptr;
                if (!property) {
                    if (entry.optional) {
                        found.emplace_back(entry.offset, PropAbsent);
                    } else {
                        failures.push_back(where + ": expected " + expected + ", found " + (cls ? "no such property" : "no such class"));
                    }
                    continue;
                }
                const char* structName = property->type == PropType::Struct && property->structType ? property->structType->name.c_str() : nullptr;
                bool sameStruct = (structName == nullptr) == (entry.structName == nullptr) && (!structName || std::string_view(structName) == entry.structName);
                if (property->type != entry.type || property->arrayDim != entry.arrayDim || (entry.elementSize && property->elementSize() != entry.elementSize) || !sameStruct) {
                    failures.push_back(where + ": expected " + expected + ", found " + describe(property->type, entry.elementSize ? property->elementSize() : 0, property->arrayDim, structName));
                    continue;
                }
                found.emplace_back(entry.offset, property->offset);
            }
        }
        if (failures.size() != before) {
            return false;
        }
        for (const auto& [slot, offset] : found) {
            *slot = offset;
        }
        return true;
    }

    bool bindAllProps(std::vector<std::string>& failures)
    {
        std::vector<ClassBinding> all;
        for (std::span<const ClassBinding> part : { propClasses_engine(), propClasses_fire(), propClasses_extension(), propClasses_consys(), propClasses_deusex() }) {
            all.insert(all.end(), part.begin(), part.end());
        }
        if (!bindClasses(all.data(), all.size(), failures)) {
            return false;
        }
        bound = true;
        return true;
    }

    bool propsBound()
    {
        return bound;
    }
}

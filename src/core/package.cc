#include "core/package.h"

#include "core/profile.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dx {
    size_t readTagSize(Reader& reader, uint8_t info)
    {
        switch ((info >> 4) & 0x07) {
        case 0:
            return 1;
        case 1:
            return 2;
        case 2:
            return 4;
        case 3:
            return 12;
        case 4:
            return 16;
        case 5:
            return reader.u8();
        case 6:
            return static_cast<uint16_t>(reader.i16());
        default:
            return reader.u32();
        }
    }

    int32_t readTagArrayIndex(Reader& reader)
    {
        uint8_t first = reader.u8();
        if ((first & 0xc0) == 0xc0) {
            int32_t value = (first & 0x3f) << 24;
            value |= reader.u8() << 16;
            value |= reader.u8() << 8;
            return value | reader.u8();
        }
        if (first & 0x80) {
            return ((first & 0x7f) << 8) | reader.u8();
        }
        return first;
    }

    Object::Object(const Package& package, std::vector<Property> properties, size_t nativeOffset)
        : owner(package)
        , props(std::move(properties))
        , nativeStart(nativeOffset)
    {
    }

    const std::vector<Property>& Object::properties() const
    {
        return props;
    }

    const Property* Object::find(std::string_view name, int32_t arrayIndex) const
    {
        for (const Property& property : props) {
            if (property.arrayIndex == arrayIndex && iequals(property.name, name)) {
                return &property;
            }
        }
        return nullptr;
    }

    Reader Object::valueReader(const Property& property) const
    {
        return Reader(owner.data(), property.offset);
    }

    std::optional<uint8_t> Object::byteValue(std::string_view name, int32_t arrayIndex) const
    {
        const Property* property = find(name, arrayIndex);
        if (!property || property->type != PropertyType::Byte) {
            return std::nullopt;
        }
        return valueReader(*property).u8();
    }

    std::optional<int32_t> Object::intValue(std::string_view name, int32_t arrayIndex) const
    {
        const Property* property = find(name, arrayIndex);
        if (!property || property->type != PropertyType::Int) {
            return std::nullopt;
        }
        return valueReader(*property).i32();
    }

    std::optional<float> Object::floatValue(std::string_view name, int32_t arrayIndex) const
    {
        const Property* property = find(name, arrayIndex);
        if (!property || property->type != PropertyType::Float) {
            return std::nullopt;
        }
        return valueReader(*property).f32();
    }

    std::optional<bool> Object::boolValue(std::string_view name, int32_t arrayIndex) const
    {
        const Property* property = find(name, arrayIndex);
        if (!property || property->type != PropertyType::Bool) {
            return std::nullopt;
        }
        return property->boolValue;
    }

    std::optional<int32_t> Object::objectValue(std::string_view name, int32_t arrayIndex) const
    {
        const Property* property = find(name, arrayIndex);
        if (!property || (property->type != PropertyType::Object && property->type != PropertyType::Class)) {
            return std::nullopt;
        }
        return valueReader(*property).index();
    }

    std::optional<std::string> Object::nameValue(std::string_view name, int32_t arrayIndex) const
    {
        const Property* property = find(name, arrayIndex);
        if (!property || property->type != PropertyType::Name) {
            return std::nullopt;
        }
        return owner.nameAt(valueReader(*property).index());
    }

    std::optional<std::array<float, 3>> Object::vectorValue(std::string_view name, int32_t arrayIndex) const
    {
        const Property* property = find(name, arrayIndex);
        if (!property || property->type != PropertyType::Struct || property->size != 12) {
            return std::nullopt;
        }
        Reader reader = valueReader(*property);
        std::array<float, 3> value {};
        value[0] = reader.f32();
        value[1] = reader.f32();
        value[2] = reader.f32();
        return value;
    }

    Reader Object::native() const
    {
        return Reader(owner.data(), nativeStart);
    }

    namespace {
        // path::string() throws on Windows for names outside the narrow code page; UTF-8 never does.
        std::string utf8(const std::filesystem::path& path)
        {
            std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }
    }

    Package::Package(const std::filesystem::path& path)
        : packageName(utf8(path.stem()))
    {
        DX_ZONE_NAMED("Read package");
        DX_MESSAGE(packageName);
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("cannot open package " + utf8(path));
        }
        std::error_code error;
        uintmax_t fileSize = std::filesystem::file_size(path, error);
        if (error) {
            throw std::runtime_error("cannot read package " + utf8(path) + ": " + error.message());
        }
        bytes.resize(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            throw std::runtime_error("cannot read package " + utf8(path));
        }

        Reader reader(bytes, 0);
        if (reader.u32() != 0x9E2A83C1) {
            throw std::runtime_error("not an unreal package: " + utf8(path));
        }
        fileVersion = static_cast<uint16_t>(reader.i16());
        licenseeMode = static_cast<uint16_t>(reader.i16());
        if (fileVersion < 61 || fileVersion > 69) {
            throw std::runtime_error("unsupported package version in " + utf8(path));
        }
        reader.u32();
        uint32_t nameCount = reader.u32();
        uint32_t nameOffset = reader.u32();
        uint32_t exportCount = reader.u32();
        uint32_t exportOffset = reader.u32();
        uint32_t importCount = reader.u32();
        uint32_t importOffset = reader.u32();

        reader.seek(nameOffset);
        // Each entry takes at least a byte, so a count beyond the data is not reserved for.
        nameTable.reserve(std::min<size_t>(nameCount, reader.remaining()));
        for (uint32_t i = 0; i < nameCount; i++) {
            NameEntry entry;
            if (fileVersion < 64) {
                for (uint8_t c = reader.u8(); c != 0; c = reader.u8()) {
                    entry.name.push_back(static_cast<char>(c));
                }
            } else {
                entry.name = reader.string();
            }
            entry.flags = reader.u32();
            nameTable.push_back(std::move(entry));
        }

        reader.seek(exportOffset);
        exportTable.reserve(std::min<size_t>(exportCount, reader.remaining()));
        for (uint32_t i = 0; i < exportCount; i++) {
            ExportEntry entry;
            entry.classRef = reader.index();
            entry.superRef = reader.index();
            entry.outer = reader.i32();
            entry.name = reader.index();
            entry.flags = reader.u32();
            entry.size = reader.index();
            if (entry.size > 0) {
                entry.offset = reader.index();
            }
            exportTable.push_back(entry);
        }

        reader.seek(importOffset);
        importTable.reserve(std::min<size_t>(importCount, reader.remaining()));
        for (uint32_t i = 0; i < importCount; i++) {
            ImportEntry entry;
            entry.classPackage = reader.index();
            entry.className = reader.index();
            entry.outer = reader.i32();
            entry.name = reader.index();
            importTable.push_back(entry);
        }
    }

    const std::string& Package::name() const
    {
        return packageName;
    }

    uint16_t Package::version() const
    {
        return fileVersion;
    }

    uint16_t Package::licensee() const
    {
        return licenseeMode;
    }

    const std::vector<NameEntry>& Package::names() const
    {
        return nameTable;
    }

    const std::vector<ImportEntry>& Package::imports() const
    {
        return importTable;
    }

    const std::vector<ExportEntry>& Package::exports() const
    {
        return exportTable;
    }

    std::span<const uint8_t> Package::data() const
    {
        return bytes;
    }

    const std::string& Package::nameAt(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= nameTable.size()) {
            throw std::runtime_error("name index out of range in " + packageName);
        }
        return nameTable[static_cast<size_t>(index)].name;
    }

    const ImportEntry& Package::importAt(int32_t ref) const
    {
        if (ref >= 0 || static_cast<size_t>(-ref) > importTable.size()) {
            throw std::runtime_error("import reference out of range in " + packageName);
        }
        return importTable[static_cast<size_t>(-ref - 1)];
    }

    const ExportEntry& Package::exportAt(int32_t ref) const
    {
        if (ref <= 0 || static_cast<size_t>(ref) > exportTable.size()) {
            throw std::runtime_error("export reference out of range in " + packageName);
        }
        return exportTable[static_cast<size_t>(ref - 1)];
    }

    std::string Package::objectName(int32_t ref) const
    {
        if (ref == 0) {
            return "None";
        }
        if (ref < 0) {
            return nameAt(importAt(ref).name);
        }
        return nameAt(exportAt(ref).name);
    }

    std::string Package::classOf(int32_t ref) const
    {
        if (ref < 0) {
            return nameAt(importAt(ref).className);
        }
        const ExportEntry& entry = exportAt(ref);
        if (entry.classRef == 0) {
            return "Class";
        }
        return objectName(entry.classRef);
    }

    std::string Package::groupOf(int32_t ref) const
    {
        std::vector<std::string> parts;
        int32_t outer = ref < 0 ? importAt(ref).outer : exportAt(ref).outer;
        while (outer != 0) {
            parts.push_back(objectName(outer));
            outer = outer < 0 ? importAt(outer).outer : exportAt(outer).outer;
        }
        if (ref < 0 && !parts.empty()) {
            parts.pop_back();
        }
        std::string group;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            if (!group.empty()) {
                group += '.';
            }
            group += *it;
        }
        return group;
    }

    int32_t Package::findExport(std::string_view className, std::string_view objectName) const
    {
        for (size_t i = 0; i < exportTable.size(); i++) {
            int32_t ref = static_cast<int32_t>(i + 1);
            if (iequals(nameAt(exportTable[i].name), objectName) && iequals(classOf(ref), className)) {
                return ref;
            }
        }
        return 0;
    }

    Object Package::load(int32_t ref) const
    {
        const ExportEntry& entry = exportAt(ref);
        if (entry.size <= 0) {
            return Object(*this, {}, static_cast<size_t>(entry.offset));
        }

        Reader reader(bytes, static_cast<size_t>(entry.offset));
        if (entry.flags & RF_HasStack) {
            int32_t node = reader.index();
            reader.index();
            reader.u64();
            reader.i32();
            if (node != 0) {
                reader.index();
            }
        }

        std::vector<Property> properties;
        if (entry.classRef != 0) {
            while (true) {
                std::string propertyName = nameAt(reader.index());
                if (iequals(propertyName, "None")) {
                    break;
                }

                Property property;
                property.name = std::move(propertyName);
                uint8_t info = reader.u8();
                property.type = static_cast<PropertyType>(info & 0x0f);
                if (property.type == PropertyType::Struct) {
                    property.structName = nameAt(reader.index());
                }

                property.size = readTagSize(reader, info);

                bool infoBit = (info & 0x80) != 0;
                if (property.type == PropertyType::Bool) {
                    property.boolValue = infoBit;
                    property.size = 0;
                } else if (infoBit) {
                    property.arrayIndex = readTagArrayIndex(reader);
                }

                property.offset = reader.tell();
                reader.skip(property.size);
                properties.push_back(std::move(property));
            }
        }

        return Object(*this, std::move(properties), reader.tell());
    }
}

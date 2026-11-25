#include "compiler/props_generator.h"

#include "core/files.h"
#include "core/object.h"
#include "core/objects.h"
#include "core/text.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace dx::compiler {
    namespace {
        struct ListProperty {
            std::string name;
            bool optional = false;
            int line = 0;
        };

        struct ListClass {
            std::string path;
            std::string space;
            std::vector<ListProperty> properties;
        };

        struct ListFile {
            std::string name;
            std::vector<ListClass> classes;
        };

        struct List {
            std::unordered_map<std::string, std::string> mirrors;
            std::vector<ListFile> files;
        };

        std::vector<std::string> words(const std::string& line)
        {
            std::vector<std::string> result;
            std::istringstream in(line.substr(0, line.find('#')));
            std::string word;
            while (in >> word) {
                result.push_back(word);
            }
            return result;
        }

        bool parseList(const std::string& text, List& list, std::vector<std::string>& errors)
        {
            std::istringstream in(text);
            std::string line;
            int number = 0;
            auto fail = [&](const std::string& message) {
                errors.push_back("line " + std::to_string(number) + ": " + message);
            };
            while (std::getline(in, line)) {
                number++;
                std::vector<std::string> w = words(line);
                if (w.empty()) {
                    continue;
                }
                bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
                if (!indented && w[0] == "mirror") {
                    if (w.size() != 3) {
                        fail("expected: mirror <script struct> <C++ type>");
                        continue;
                    }
                    list.mirrors[w[1]] = w[2];
                } else if (!indented && w[0] == "file") {
                    if (w.size() != 2) {
                        fail("expected: file <name>");
                        continue;
                    }
                    list.files.push_back({ w[1], {} });
                } else if (!indented && w[0] == "class") {
                    if (list.files.empty()) {
                        fail("class before any file");
                        continue;
                    }
                    ListClass cls;
                    cls.path = w.size() > 1 ? w[1] : "";
                    size_t dot = cls.path.find('.');
                    if (dot == std::string::npos || (w.size() != 2 && !(w.size() == 4 && w[2] == "as"))) {
                        fail("expected: class <Package.Class> [as <namespace>]");
                        continue;
                    }
                    cls.space = w.size() == 4 ? w[3] : lowered(cls.path.substr(dot + 1));
                    list.files.back().classes.push_back(std::move(cls));
                } else if (indented) {
                    if (list.files.empty() || list.files.back().classes.empty()) {
                        fail("property outside a class");
                        continue;
                    }
                    ListProperty property;
                    property.name = w[0];
                    property.line = number;
                    for (size_t i = 1; i < w.size(); i++) {
                        if (w[i] == "optional") {
                            property.optional = true;
                        } else {
                            fail("unknown annotation '" + w[i] + "'");
                        }
                    }
                    list.files.back().classes.back().properties.push_back(std::move(property));
                } else {
                    fail("unknown directive '" + w[0] + "'");
                }
            }
            return errors.empty();
        }

        struct Accessor {
            std::string name;
            std::string type; // C++ type; empty for a raw uint8_t* accessor
            std::string kind; // PropType enumerator
            std::string structName;
            int32_t arrayDim = 1;
            bool optional = false;

            bool raw() const
            {
                return type.empty();
            }

            bool indexed() const
            {
                return !raw() && arrayDim > 1;
            }
        };

        const char* kindName(PropType type)
        {
            switch (type) {
            case PropType::Byte:
                return "Byte";
            case PropType::Int:
                return "Int";
            case PropType::Bool:
                return "Bool";
            case PropType::Float:
                return "Float";
            case PropType::Object:
                return "Object";
            case PropType::Class:
                return "Class";
            case PropType::Name:
                return "Name";
            case PropType::Str:
                return "Str";
            case PropType::String:
                return "String";
            case PropType::Struct:
                return "Struct";
            case PropType::Array:
                return "Array";
            case PropType::Map:
                return "Map";
            case PropType::FixedArray:
                return "FixedArray";
            case PropType::Pointer:
                return "Pointer";
            }
            return nullptr;
        }

        bool deriveAccessor(const List& list, const ListClass& cls, const ListProperty& entry, const UProperty* property, Accessor& accessor, std::vector<std::string>& errors)
        {
            accessor.name = entry.name;
            accessor.optional = entry.optional;
            accessor.arrayDim = property->arrayDim;
            const char* kind = kindName(property->type);
            if (!kind) {
                errors.push_back(cls.path + "." + entry.name + " (line " + std::to_string(entry.line) + "): no accessor for this property type");
                return false;
            }
            accessor.kind = kind;
            switch (property->type) {
            case PropType::Bool:
                accessor.type = "uint32_t";
                break;
            case PropType::Byte:
                accessor.type = "uint8_t";
                break;
            case PropType::Int:
                accessor.type = "int32_t";
                break;
            case PropType::Float:
                accessor.type = "float";
                break;
            case PropType::Object:
            case PropType::Class:
                accessor.type = "UObject*";
                break;
            case PropType::Name:
                accessor.type = "Name";
                break;
            case PropType::Str:
            case PropType::String:
                accessor.type = "std::string";
                break;
            case PropType::Struct: {
                if (!property->structType) {
                    errors.push_back(cls.path + "." + entry.name + " (line " + std::to_string(entry.line) + "): struct property without a struct");
                    return false;
                }
                accessor.structName = property->structType->name.str();
                auto mirror = list.mirrors.find(accessor.structName);
                if (mirror != list.mirrors.end()) {
                    accessor.type = mirror->second;
                }
                break;
            }
            case PropType::Array:
            case PropType::Map:
            case PropType::FixedArray:
            case PropType::Pointer:
            default:
                // Dynamic arrays, maps, fixed arrays and pointers get raw access.
                break;
            }
            return true;
        }

        void emitAccessor(std::ostringstream& h, const std::string& space, const Accessor& a)
        {
            std::string offset = "offsets_" + space + "." + a.name;
            std::string type = a.raw() ? "uint8_t" : a.type;
            std::string params = a.indexed() ? "(UObject* o, int index)" : "(UObject* o)";
            std::string address = a.raw() ? "o->data + " + offset : "reinterpret_cast<" + type + "*>(o->data + " + offset + ")";
            if (a.indexed()) {
                address = a.optional ? "&" + address + "[index]" : address + "[index]";
            }
            if (a.optional) {
                // An optional property may be absent: the accessor returns nullptr then.
                h << "        inline " << type << "* " << a.name << params << "\n";
                h << "        {\n";
                h << "            if (" << offset << " == PropAbsent) {\n";
                h << "                return nullptr;\n";
                h << "            }\n";
                h << "            return " << address << ";\n";
                h << "        }\n\n";
            } else if (a.raw()) {
                h << "        inline uint8_t* " << a.name << params << "\n";
                h << "        {\n";
                h << "            return " << address << ";\n";
                h << "        }\n\n";
            } else {
                h << "        inline " << type << "& " << a.name << params << "\n";
                h << "        {\n";
                h << "            return " << (a.indexed() ? "" : "*") << address << ";\n";
                h << "        }\n\n";
            }
        }

        void emitBinding(std::ostringstream& c, const std::string& space, const Accessor& a)
        {
            // The element size is written as sizeof so the checked-in text is the
            // same on every platform; raw accessors do not depend on it.
            std::string size = a.raw() ? "0" : "sizeof(" + a.type + ")";
            std::string structName = a.structName.empty() ? "nullptr" : "\"" + a.structName + "\"";
            c << "            { \"" << a.name << "\", &offsets_" << space << "." << a.name << ", PropType::" << a.kind << ", " << size << ", " << a.arrayDim << ", " << structName << ", " << (a.optional ? "true" : "false") << " },\n";
        }

        void loadPackages(const List& list)
        {
            std::vector<std::string> loaded;
            for (const ListFile& file : list.files) {
                for (const ListClass& cls : file.classes) {
                    std::string package = cls.path.substr(0, cls.path.find('.'));
                    if (std::find(loaded.begin(), loaded.end(), package) == loaded.end()) {
                        loaded.push_back(package);
                        objects::loadPackage(Name(package));
                    }
                }
            }
            objects::flushLoads();
        }
    }

    std::map<std::string, std::string> generateProps(const std::string& listText, std::vector<std::string>& errors)
    {
        List list;
        if (!parseList(listText, list, errors)) {
            return {};
        }
        loadPackages(list);
        std::map<std::string, std::string> result;
        for (const ListFile& file : list.files) {
            std::ostringstream h;
            std::ostringstream c;
            h << "// Generated by ucc genprops from src/engine/props_bindings.txt. Do not edit.\n";
            h << "#pragma once\n\n";
            h << "#include \"core/math.h\"\n";
            h << "#include \"core/object.h\"\n";
            h << "#include \"engine/props_bind.h\"\n\n";
            h << "#include <span>\n";
            h << "#include <string>\n\n";
            h << "namespace dx {\n";
            c << "// Generated by ucc genprops from src/engine/props_bindings.txt. Do not edit.\n";
            c << "#include \"engine/props_" << file.name << ".h\"\n\n";
            c << "#include <iterator>\n\n";
            c << "namespace dx {\n";
            std::ostringstream tables;
            std::ostringstream classes;
            for (const ListClass& cls : file.classes) {
                UClass* uclass = objects::findClass(cls.path);
                if (!uclass) {
                    errors.push_back("missing class " + cls.path);
                    continue;
                }
                std::vector<Accessor> accessors;
                for (const ListProperty& entry : cls.properties) {
                    UProperty* property = uclass->findProperty(Name(entry.name));
                    if (!property) {
                        std::string why = entry.optional ? "optional property absent from the loaded packages, so its type cannot be derived; generate with packages that have it" : "property not found";
                        errors.push_back(cls.path + "." + entry.name + " (line " + std::to_string(entry.line) + "): " + why);
                        continue;
                    }
                    Accessor accessor;
                    if (deriveAccessor(list, cls, entry, property, accessor, errors)) {
                        accessors.push_back(std::move(accessor));
                    }
                }
                h << "    namespace " << cls.space << " {\n";
                h << "        struct Offsets {\n";
                for (const Accessor& a : accessors) {
                    h << "            size_t " << a.name << " = PropAbsent;\n";
                }
                h << "        };\n";
                h << "    }\n\n";
                h << "    extern " << cls.space << "::Offsets offsets_" << cls.space << ";\n\n";
                h << "    namespace " << cls.space << " {\n";
                for (const Accessor& a : accessors) {
                    emitAccessor(h, cls.space, a);
                }
                if (accessors.empty()) {
                    h << "\n";
                }
                h << "    }\n\n";

                c << "    " << cls.space << "::Offsets offsets_" << cls.space << ";\n";
                if (accessors.empty()) {
                    classes << "            { \"" << cls.path << "\", nullptr, 0 },\n";
                    continue;
                }
                tables << "\n        const PropBinding " << cls.space << "Bindings[] = {\n";
                for (const Accessor& a : accessors) {
                    emitBinding(tables, cls.space, a);
                }
                tables << "        };\n";
                classes << "            { \"" << cls.path << "\", " << cls.space << "Bindings, std::size(" << cls.space << "Bindings) },\n";
            }
            h << "    // The classes and properties bindAllProps() binds for this file.\n";
            h << "    std::span<const ClassBinding> propClasses_" << file.name << "();\n";
            h << "}\n";
            c << "\n";
            c << "    namespace {";
            c << tables.str();
            c << "\n";
            c << "        const ClassBinding classes[] = {\n";
            c << classes.str();
            c << "        };\n";
            c << "    }\n\n";
            c << "    std::span<const ClassBinding> propClasses_" << file.name << "()\n";
            c << "    {\n";
            c << "        return classes;\n";
            c << "    }\n";
            c << "}\n";
            result["props_" + file.name + ".h"] = h.str();
            result["props_" + file.name + ".cc"] = c.str();
        }
        if (!errors.empty()) {
            return {};
        }
        return result;
    }

    bool writeProps(const std::filesystem::path& listFile, const std::filesystem::path& outputDirectory, std::vector<std::string>& errors)
    {
        std::optional<std::string> text = files::readFile(listFile);
        if (!text) {
            errors.push_back("cannot read " + listFile.string());
            return false;
        }
        std::map<std::string, std::string> files = generateProps(*text, errors);
        if (files.empty()) {
            return false;
        }
        std::filesystem::create_directories(outputDirectory);
        for (const auto& [name, content] : files) {
            std::ofstream out(outputDirectory / name, std::ios::binary);
            out << content;
            if (!out) {
                errors.push_back("cannot write " + (outputDirectory / name).string());
                return false;
            }
        }
        return true;
    }
}

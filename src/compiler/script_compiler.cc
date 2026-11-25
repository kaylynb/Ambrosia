#include "compiler/script_compiler.h"

#include "compiler/assets.h"
#include "compiler/exec_parse.h"
#include "compiler/script_parser.h"
#include "core/objects.h"
#include "core/package.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dx::compiler {
    namespace {
        std::vector<std::string> splitLines(const std::string& text)
        {
            std::vector<std::string> lines;
            size_t at = 0;
            while (at < text.size()) {
                size_t end = text.find_first_of("\r\n", at);
                lines.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
                at = end == std::string::npos ? text.size() : end;
                if (at < text.size() && text[at] == '\r') {
                    at++;
                }
                if (at < text.size() && text[at] == '\n') {
                    at++;
                }
            }
            return lines;
        }

        std::string parseToken(const std::string& line, size_t at)
        {
            while (at < line.size() && (line[at] == ' ' || line[at] == 9)) {
                at++;
            }
            std::string token;
            while (at < line.size() && line[at] != ' ' && line[at] != 9) {
                token += line[at++];
            }
            return token;
        }

        void resetDefaults(UClass* cls)
        {
            uint8_t* data = cls->defaultObject->data;
            cls->destroyValue(data);
            cls->initValue(data);
            if (UClass* parent = cls->superClass()) {
                parent->copyValue(data, parent->defaultObject->data);
            }
            objects::syncObjectProperties(cls->defaultObject);
        }
    }

    namespace {
        void objLoad(PackageCompiler::Impl& context, std::string_view command)
        {
            std::string file;
            if (!parseString(command, "FILE=", file)) {
                context.log("Warning: OBJ LOAD: missing FILE=");
                return;
            }
            std::replace(file.begin(), file.end(), '\\', '/');
            std::string packageName = std::filesystem::path(file).stem().string();
            Linker* linker = objects::loadPackage(Name(packageName));
            if (!linker) {
                context.log("Warning: OBJ LOAD could not load " + file);
                return;
            }
            objects::loadAllExports(linker);
        }

        void runExecs(PackageCompiler::Impl& context)
        {
            for (const auto& [cls, command] : context.pendingExecs) {
                std::string_view text = command;
                std::string_view objText = text;
                if (dx::compiler::parseCommand(objText, "OBJ") && dx::compiler::parseCommand(objText, "LOAD")) {
                    objLoad(context, objText);
                    continue;
                }
                ExecContext exec;
                exec.package = context.package;
                exec.directory = context.files.count(cls) ? context.files[cls].parent_path().parent_path() : std::filesystem::path();
                exec.log = context.log;
                if (!importAsset(exec, command)) {
                    context.log("Warning: unsupported #exec " + command);
                }
                context.errorCount += exec.errors;
            }
            context.pendingExecs.clear();
        }
    }

    PackageCompiler::PackageCompiler(UObject* package, LogSink log)
        : impl(std::make_unique<Impl>())
    {
        impl->package = package;
        impl->log = std::move(log);
        impl->lookup.refresh();
    }

    PackageCompiler::~PackageCompiler() = default;

    void PackageCompiler::setNativeClasses(std::vector<Name> names)
    {
        impl->nativeClasses = std::move(names);
    }

    int PackageCompiler::errors() const
    {
        return impl->errorCount;
    }

    UClass* PackageCompiler::importClass(const std::filesystem::path& file, const std::string& text)
    {
        std::string scriptText;
        std::string defaultText;
        std::string className;
        std::string baseClassName;
        std::vector<std::string> lines = splitLines(text);
        for (size_t i = 0; i < lines.size(); i++) {
            std::string line = lines[i];
            if (std::string_view command = line; parseCommand(command, "defaultproperties")) {
                for (i++; i < lines.size(); i++) {
                    size_t first = lines[i].find_first_not_of(" \t");
                    if (first != std::string::npos && lines[i][first] == '}') {
                        break;
                    }
                    defaultText += lines[i] + "\r\n";
                }
                continue;
            }
            scriptText += line + "\r\n";
            size_t comment = line.find("//");
            if (comment != std::string::npos) {
                line = line.substr(0, comment);
            }
            size_t at = std::string::npos;
            if (className.empty() && (at = findKey(line, "class")) != std::string::npos) {
                className = parseToken(line, at + 6);
            }
            if (baseClassName.empty() && ((at = findKey(line, "expands")) != std::string::npos || (at = findKey(line, "extends")) != std::string::npos)) {
                baseClassName = parseToken(line, at + 7);
                while (!baseClassName.empty() && baseClassName.back() == ';') {
                    baseClassName.pop_back();
                }
            }
        }
        std::string expected = file.stem().string();
        Name fileName(expected);
        if (className.empty() || (baseClassName.empty() && className != "Object")) {
            impl->log("Bad class definition '" + className + "'/'" + baseClassName + "' in " + file.string());
            impl->errorCount++;
            return nullptr;
        }
        if (!iequals(className, expected)) {
            impl->log("Script vs. class name mismatch (" + expected + "/" + className + ")");
        }
        Lookup& lookup = impl->lookup;
        auto inPackage = [&](const std::string& name) -> UClass* {
            Name found = Name::find(name);
            if (found.none()) {
                return nullptr;
            }
            for (UObject* object : objects::all()) {
                UClass* cls = cast<UClass>(object);
                if (cls && !cls->destroyed && cls->outer == impl->package && cls->name == found) {
                    return cls;
                }
            }
            return nullptr;
        };
        UClass* result = inPackage(className);
        bool updateNative = result && (result->flags & RF_Native);
        if (result && result->defaultObject) {
            result->defaultObject->destroyed = true;
            result->defaultObject = nullptr;
            result->linked = false;
            result->properties.clear();
            result->invalidatePropertyIndex();
            result->size = 0;
            result->alignment = 1;
        }
        if (!result) {
            Name classObjectName = iequals(className, expected) ? fileName : Name(className);
            result = detail::createField(std::make_unique<UClass>(), impl->package, classObjectName, nullptr, RF_Public | RF_Standalone);
            lookup.add(result);
        }
        if (std::find(impl->nativeClasses.begin(), impl->nativeClasses.end(), result->name) != impl->nativeClasses.end()) {
            result->flags |= RF_Native;
        }
        if (!updateNative) {
            UClass* base = inPackage(baseClassName);
            if (!base) {
                base = lookup.findClass(baseClassName);
            }
            if (!base && !baseClassName.empty()) {
                base = detail::createField(std::make_unique<UClass>(), impl->package, Name(baseClassName), nullptr, RF_Public | RF_Standalone);
                base->superField = result;
                lookup.add(base);
            }
            result->superField = base;
        }
        UTextBuffer* buffer = detail::createField(std::make_unique<UTextBuffer>(), result, Name("ScriptText"), "TextBuffer", RF_NotForClient | RF_NotForServer);
        buffer->text = scriptText;
        result->scriptText = buffer;
        result->classFlags &= ~(CLASS_Parsed | CLASS_Compiled);
        impl->defaultProperties[result] = defaultText;
        impl->files[result] = file;
        impl->imported.push_back(result);
        return result;
    }

    bool PackageCompiler::compile()
    {
        Impl& context = *impl;
        bool missingSuper = false;
        for (UClass* cls : context.imported) {
            std::unordered_set<UClass*> seen;
            UClass* current = cls;
            while (current && seen.insert(current).second) {
                current = current->superClass();
            }
            UClass* super = cls->superClass();
            if (current && super && !super->scriptText) {
                // The original compiler stopped here. Count the error and keep its message
                // format (no filename prefix).
                context.log("Superclass " + super->name.str() + " of class " + cls->name.str() + " not found");
                context.errorCount++;
                missingSuper = true;
            }
        }
        if (missingSuper) {
            return false;
        }
        std::vector<UClass*> allClasses;
        for (UObject* object : objects::all()) {
            if (UClass* cls = cast<UClass>(object)) {
                if (!cls->destroyed) {
                    allClasses.push_back(cls);
                }
            }
        }
        std::unordered_map<UClass*, std::vector<UClass*>> subclasses;
        for (UClass* cls : allClasses) {
            subclasses[cls->superClass()].push_back(cls);
        }
        detail::ScriptCompiler compiler(context);
        std::function<bool(UClass*)> parse = [&](UClass* cls) {
            if (!cls->scriptText) {
                return true;
            }
            if (!(cls->classFlags & CLASS_Parsed)) {
                if (!compiler.compileScript(cls, 0)) {
                    return false;
                }
                cls->linkClass();
            }
            for (UClass* child : subclasses[cls]) {
                if (!parse(child)) {
                    return false;
                }
            }
            return true;
        };
        std::function<bool(UClass*)> compileTree = [&](UClass* cls) {
            if (cls->scriptText && !(cls->classFlags & CLASS_Compiled)) {
                if (!compiler.compileScript(cls, 1)) {
                    return false;
                }
                resetDefaults(cls);
                detail::importDefaults(context, cls, context.defaultProperties[cls]);
            }
            for (UClass* child : subclasses[cls]) {
                if (!compileTree(child)) {
                    return false;
                }
            }
            return true;
        };
        UClass* root = objects::objectClass();
        if (!parse(root)) {
            return false;
        }
        runExecs(context);
        bool success = compileTree(root);
        ExecContext finish;
        finish.package = context.package;
        finish.log = context.log;
        finishAssets(finish);
        context.errorCount += finish.errors;
        return success && context.errorCount == 0;
    }
}

#include "compiler/assets.h"

#include "core/log.h"
#include "core/objects.h"
#include "core/text.h"

#include <algorithm>
#include <exception>

namespace dx::compiler {
    const std::vector<AssetHandler>& assetHandlers()
    {
        static const std::vector<AssetHandler> handlers = { textureAssets(), audioAssets(), meshAssets(), conversationAssets(), textAssets() };
        return handlers;
    }

    namespace {
        // A handler that throws (out of memory, a number out of range, a filesystem error) fails only its own
        // command: it is logged as an error and counted, and the command counts as handled.
        void handlerFailed(ExecContext& context, const std::string& command, const std::exception& error)
        {
            context.errors++;
            if (context.log) {
                context.log("Error: " + command + ": " + error.what());
            }
        }
    }

    bool importAsset(ExecContext& context, const std::string& command)
    {
        for (const AssetHandler& handler : assetHandlers()) {
            try {
                if (handler.import && handler.import(context, command)) {
                    return true;
                }
            } catch (const std::exception& error) {
                handlerFailed(context, command, error);
                return true;
            }
        }
        return false;
    }

    bool exportAssetSource(ExecContext& context, const std::string& command)
    {
        for (const AssetHandler& handler : assetHandlers()) {
            try {
                if (handler.exportSource && handler.exportSource(context, command)) {
                    return true;
                }
            } catch (const std::exception& error) {
                handlerFailed(context, command, error);
                return false;
            }
        }
        return false;
    }

    bool exportAssetObject(UObject* object, const std::string& extension, const std::filesystem::path& file)
    {
        for (const AssetHandler& handler : assetHandlers()) {
            try {
                if (handler.exportObject && handler.exportObject(object, extension, file)) {
                    return true;
                }
            } catch (const std::exception& error) {
                log::error("DevCompile", "exporting {}: {}", object ? object->pathName() : std::string("None"), error.what());
                return false;
            }
        }
        return false;
    }

    bool serializeAsset(PackageWriter& package, UObject* object, Writer& out)
    {
        for (const AssetHandler& handler : assetHandlers()) {
            if (handler.serialize && handler.serialize(package, object, out)) {
                return true;
            }
        }
        return false;
    }

    void finishAssets(ExecContext& context)
    {
        for (const AssetHandler& handler : assetHandlers()) {
            if (handler.finish) {
                try {
                    handler.finish(context);
                } catch (const std::exception& error) {
                    handlerFailed(context, "finishing imported assets", error);
                }
            }
        }
    }

    bool exportPath(ExecContext& context, const std::filesystem::path& root, std::string file, std::filesystem::path& result)
    {
        std::replace(file.begin(), file.end(), '\\', '/');
        std::filesystem::path relative = std::filesystem::path(file).lexically_normal();
        if (relative.empty() || relative.has_root_path() || *relative.begin() == "..") {
            context.log("Warning: export path " + file + " is outside " + root.string());
            return false;
        }
        result = root / relative;
        return true;
    }

    UObject* findChild(UObject* outer, Name name, UClass* cls)
    {
        for (UObject* object : objects::all()) {
            if (object && !object->destroyed && object->outer == outer && object->name == name && (!cls || object->cls == cls)) {
                return object;
            }
        }
        return nullptr;
    }

    std::filesystem::path resolveFile(const std::filesystem::path& directory, std::string text)
    {
        std::replace(text.begin(), text.end(), '\\', '/');
        std::filesystem::path result = directory;
        for (const std::filesystem::path& part : std::filesystem::path(text)) {
            std::filesystem::path exact = result / part;
            if (std::filesystem::exists(exact) || !std::filesystem::is_directory(result)) {
                result = exact;
                continue;
            }
            std::filesystem::path match = exact;
            for (const auto& entry : std::filesystem::directory_iterator(result)) {
                if (iequals(entry.path().filename().string(), part.string())) {
                    match = entry.path();
                    break;
                }
            }
            result = match;
        }
        return result;
    }
}

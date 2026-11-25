#include "compiler/assets.h"
#include "compiler/exec_parse.h"
#include "compiler/exporter.h"
#include "compiler/props_generator.h"
#include "compiler/script_compiler.h"
#include "compiler/script_package.h"
#include "core/config.h"
#include "core/files.h"
#include "core/objects.h"
#include "core/random.h"
#include "core/text.h"
#include "core/version.h"
#include "core/workers.h"
#include "audio/acoustic_baker.h"
#include "audio/acoustic_scene.h"
#include "deusex/startup.h"
#include "engine/world.h"

#include <CLI11.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// MinGW's C++ library defines NOMINMAX itself.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
    int genProps(const std::string& list, const std::string& directory)
    {
        std::vector<std::string> errors;
        bool written = dx::compiler::writeProps(list, directory, errors);
        for (const std::string& error : errors) {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
        if (!written) {
            std::fprintf(stderr, "Failure - %zu error(s)\n", errors.size());
            return 1;
        }
        std::printf("Wrote property bindings to %s\n", directory.c_str());
        return 0;
    }

    dx::Linker* loadForExport(const std::string& file)
    {
        std::string packageName = std::filesystem::path(file).stem().string();
        dx::Linker* linker = dx::objects::loadPackage(dx::Name(packageName));
        if (!linker) {
            std::fprintf(stderr, "cannot load package %s\n", packageName.c_str());
            return nullptr;
        }
        dx::objects::loadAllExports(linker);
        return linker;
    }

    int exportClasses(dx::Linker* linker, const std::filesystem::path& directory)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            std::fprintf(stderr, "cannot create %s: %s\n", directory.string().c_str(), error.message().c_str());
            return 1;
        }
        const dx::Package& file = linker->file();
        int exported = 0;
        int skipped = 0;
        int failed = 0;
        for (size_t i = 0; i < file.exports().size(); i++) {
            int32_t ref = static_cast<int32_t>(i + 1);
            if (file.classOf(ref) != "Class") {
                continue;
            }
            dx::UClass* cls = dx::cast<dx::UClass>(linker->object(ref));
            if (!cls) {
                continue;
            }
            if (!cls->scriptText) {
                std::printf("No script text for %s\n", cls->name.str().c_str());
                skipped++;
                continue;
            }
            dx::objects::ensureLoaded(cls->scriptText);
            std::filesystem::path path = directory / (cls->name.str() + ".uc");
            std::ofstream out(path, std::ios::binary);
            out << dx::compiler::exportClass(cls);
            out.close();
            if (!out) {
                std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
                failed++;
                continue;
            }
            exported++;
        }
        std::printf("Exported %d classes, %d without script text\n", exported, skipped);
        return failed == 0 ? 0 : 1;
    }

    struct BatchExport {
        std::string package;
        std::string className;
        std::string extension;
        std::string directory;
    };

    int batchExport(const BatchExport& args)
    {
        dx::Linker* linker = loadForExport(args.package);
        if (!linker) {
            return 1;
        }
        std::filesystem::path directory(args.directory);
        if (dx::lowered(args.className) == "class" && dx::lowered(args.extension) == "uc") {
            return exportClasses(linker, directory);
        }
        dx::UClass* cls = dx::objects::findClass(args.className);
        if (!cls) {
            std::fprintf(stderr, "class %s not found\n", args.className.c_str());
            return 1;
        }
        std::filesystem::create_directories(directory);
        int exported = 0;
        for (size_t i = 0; i < linker->file().exports().size(); i++) {
            dx::UObject* object = linker->object(static_cast<int32_t>(i + 1));
            if (!object || !object->cls || !object->isA(cls)) {
                continue;
            }
            std::filesystem::path file = directory / (object->name.str() + "." + args.extension);
            if (dx::compiler::exportAssetObject(object, dx::lowered(args.extension), file)) {
                std::printf("Exported %s to %s\n", object->pathName().c_str(), file.string().c_str());
                exported++;
            } else {
                std::printf("Can't export %s to %s\n", object->pathName().c_str(), args.extension.c_str());
            }
        }
        std::printf("Exported %d objects\n", exported);
        return 0;
    }

    int exportSources(const std::string& package, const std::string& directory)
    {
        dx::Linker* linker = loadForExport(package);
        if (!linker) {
            return 1;
        }
        std::filesystem::path root = std::filesystem::path(directory) / linker->packageName.str();
        if (exportClasses(linker, root / "Classes") != 0) {
            return 1;
        }
        dx::compiler::ExecContext context;
        context.package = linker->packageObject;
        context.directory = root;
        context.log = [](const std::string& line) {
            std::printf("%s\n", line.c_str());
        };
        int written = 0;
        int failed = 0;
        for (size_t i = 0; i < linker->file().exports().size(); i++) {
            dx::UClass* cls = dx::cast<dx::UClass>(linker->object(static_cast<int32_t>(i + 1)));
            if (!cls || !cls->scriptText) {
                continue;
            }
            std::istringstream lines(cls->scriptText->text);
            std::string line;
            while (std::getline(lines, line)) {
                std::string_view text = line;
                while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
                    text.remove_suffix(1);
                }
                if (!dx::compiler::parseCommand(text, "#exec")) {
                    continue;
                }
                std::string_view objText = text;
                if (dx::compiler::parseCommand(objText, "OBJ") && dx::compiler::parseCommand(objText, "LOAD")) {
                    continue;
                }
                if (dx::compiler::exportAssetSource(context, std::string(text))) {
                    written++;
                } else {
                    std::printf("Can't export source for #exec %s\n", std::string(text).c_str());
                    failed++;
                }
            }
        }
        std::printf("Exported sources for %d #exec directives, %d failed\n", written, failed);
        return failed == 0 ? 0 : 1;
    }

    std::vector<std::filesystem::path> classFiles(const std::filesystem::path& directory)
    {
        std::vector<std::filesystem::path> files;
        for (const auto& item : std::filesystem::directory_iterator(directory)) {
            if (item.is_regular_file() && dx::lowered(item.path().extension().string()) == ".uc") {
                files.push_back(item.path());
            }
        }
        std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return dx::lowered(a.filename().string()) < dx::lowered(b.filename().string());
        });
        return files;
    }

    void rebindResident(dx::Linker* linker, dx::UObject* package)
    {
        std::unordered_map<std::string, dx::UObject*> live;
        for (dx::UObject* object : dx::compiler::packageContents(package)) {
            live.emplace(dx::lowered(object->pathName()), object);
        }
        dx::compiler::discardStaleFields(package);
        for (size_t i = 0; i < linker->file().exports().size(); i++) {
            int32_t ref = static_cast<int32_t>(i + 1);
            dx::UObject* cached = linker->object(ref);
            if (cached && cached->destroyed) {
                auto found = live.find(dx::lowered(cached->pathName()));
                linker->rebind(ref, found != live.end() ? found->second : nullptr);
            }
        }
    }

    std::string exportPath(const dx::Package& file, int32_t ref)
    {
        const dx::ExportEntry& entry = file.exportAt(ref);
        std::string name = file.nameAt(entry.name);
        return entry.outer > 0 ? exportPath(file, entry.outer) + "." + name : file.name() + "." + name;
    }

    bool buildPackage(const std::filesystem::path& root, const std::string& name, const std::filesystem::path& sources, const std::filesystem::path& output)
    {
        std::printf("Building %s from %s\n", name.c_str(), sources.string().c_str());
        std::vector<std::filesystem::path> files = classFiles(sources);
        if (files.empty()) {
            std::printf("No files for package: %s\n", sources.string().c_str());
            return false;
        }
        dx::Linker* residentLinker = dx::objects::findLinker(dx::Name(name));
        bool resident = residentLinker != nullptr;
        if (resident) {
            dx::objects::loadAllExports(residentLinker);
        }
        dx::objects::setPackageFile(dx::Name(name), {});
        dx::UObject* package = dx::objects::packageObject(dx::Name(name));
        dx::compiler::PackageCompiler compiler(package, [](const std::string& line) {
            std::printf("%s\n", line.c_str());
        });
        std::filesystem::path shipped = root / "System" / (name + ".u");
        std::unordered_map<std::string, size_t> conform;
        if (std::filesystem::exists(shipped)) {
            dx::Package original(shipped);
            for (size_t i = 0; i < original.exports().size(); i++) {
                conform.emplace(dx::lowered(exportPath(original, static_cast<int32_t>(i + 1))), i);
            }
            std::vector<dx::Name> natives;
            for (size_t i = 0; i < original.exports().size(); i++) {
                const dx::ExportEntry& entry = original.exports()[i];
                if (entry.classRef == 0 && (entry.flags & dx::RF_Native)) {
                    natives.emplace_back(original.nameAt(entry.name));
                }
            }
            compiler.setNativeClasses(std::move(natives));
            std::unordered_map<std::string, size_t> created;
            for (size_t i = 0; i < original.exports().size(); i++) {
                if (original.exports()[i].classRef == 0) {
                    created.emplace(dx::lowered(original.nameAt(original.exports()[i].name)), i);
                }
            }
            std::stable_sort(files.begin(), files.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
                auto first = created.find(dx::lowered(a.stem().string()));
                auto second = created.find(dx::lowered(b.stem().string()));
                size_t left = first != created.end() ? first->second : created.size();
                size_t right = second != created.end() ? second->second : created.size();
                return left < right;
            });
        }
        for (const std::filesystem::path& file : files) {
            std::optional<std::string> text = dx::files::readFile(file);
            if (!text) {
                std::printf("Failure - cannot read %s\n", file.string().c_str());
                return false;
            }
            compiler.importClass(file, *text);
        }
        if (!compiler.compile()) {
            std::printf("Failure - %d error(s)\n", compiler.errors());
            return false;
        }
        if (resident) {
            rebindResident(residentLinker, package);
        }
        std::vector<dx::UObject*> contents = dx::compiler::packageContents(package);
        std::stable_sort(contents.begin(), contents.end(), [&](dx::UObject* a, dx::UObject* b) {
            auto first = conform.find(dx::lowered(a->pathName()));
            auto second = conform.find(dx::lowered(b->pathName()));
            size_t left = first != conform.end() ? first->second : conform.size();
            size_t right = second != conform.end() ? second->second : conform.size();
            return left < right;
        });
        std::string error;
        if (!dx::compiler::saveScriptPackage(package, contents, output, &error)) {
            std::printf("Failed to save %s: %s\n", output.string().c_str(), error.c_str());
            return false;
        }
        std::printf("Saved %s\n", output.string().c_str());
        dx::objects::setPackageFile(dx::Name(name), output);
        if (resident) {
            return true;
        }
        for (dx::UObject* object : dx::compiler::packageContents(package)) {
            if (dx::UClass* cls = dx::cast<dx::UClass>(object)) {
                cls->defaultObject->destroyed = true;
            }
            object->destroyed = true;
        }
        dx::Linker* linker = dx::objects::loadPackageFile(dx::Name(name), output, false);
        if (!linker) {
            std::printf("Failed to reload %s\n", output.string().c_str());
            return false;
        }
        dx::objects::loadAllExports(linker);
        for (dx::UObject* object : std::vector<dx::UObject*>(dx::objects::all())) {
            dx::UClass* cls = dx::cast<dx::UClass>(object);
            if (cls && !cls->destroyed && cls->superField && cls->superField->destroyed) {
                cls->superField = dx::objects::findClass(cls->superField->pathName());
            }
        }
        return true;
    }

    int make(const std::filesystem::path& root)
    {
        std::filesystem::path user = dx::config::userDirectory();
        std::vector<std::string> packages = dx::config::system().getAll("Editor.EditorEngine", "EditPackages");
        for (const std::string& name : packages) {
            std::printf("--------------------%s--------------------\n", name.c_str());
            std::filesystem::path output = user / "System" / (name + ".u");
            std::filesystem::path userSources = user / name / "Classes";
            std::filesystem::path gameSources = root / name / "Classes";
            if (std::filesystem::exists(output)) {
                dx::Linker* linker = dx::objects::loadPackage(dx::Name(name));
                if (!linker) {
                    std::printf("Failed to load %s\n", output.string().c_str());
                    return 1;
                }
                dx::objects::loadAllExports(linker);
                continue;
            }
            std::filesystem::path sources;
            if (std::filesystem::is_directory(userSources)) {
                sources = userSources;
            } else if (dx::Linker* linker = dx::objects::loadPackage(dx::Name(name))) {
                dx::objects::loadAllExports(linker);
                continue;
            } else if (std::filesystem::is_directory(gameSources)) {
                sources = gameSources;
            } else {
                std::printf("No files for package: %s\n", name.c_str());
                return 1;
            }
            if (!buildPackage(root, name, sources, output)) {
                return 1;
            }
        }
        std::printf("Success\n");
        return 0;
    }

    // Bakes the raytraced sound mode's acoustics for maps ahead of play, into the same cache
    // the game uses, skipping maps whose cache is already valid unless force is set.
    int bakeAcoustics(const std::filesystem::path& root, const std::vector<std::string>& names, bool force)
    {
        std::vector<std::string> maps;
        for (const std::string& arg : names) {
            if (dx::lowered(arg) == "all") {
                std::error_code error;
                for (const auto& entry : std::filesystem::directory_iterator(root / "Maps", error)) {
                    if (dx::lowered(entry.path().extension().string()) == ".dx") {
                        maps.push_back(entry.path().stem().string());
                    }
                }
            } else {
                std::filesystem::path path(arg);
                maps.push_back(path.has_extension() ? path.stem().string() : arg);
            }
        }
        if (maps.empty()) {
            std::fprintf(stderr, "ucc bakeacoustics: no maps in %s\n", (root / "Maps").string().c_str());
            return 1;
        }
        if (!dx::audio::acoustics::bakingAvailable()) {
            std::fprintf(stderr, "ucc bakeacoustics: Steam Audio's baking is not available\n");
            return 1;
        }
        std::sort(maps.begin(), maps.end());
        // A batch bake can run for a while: it keeps to half the background threads (half the
        // machine's cores unless AmbrosiaBackgroundThreads says otherwise), below normal priority,
        // so the machine stays usable meanwhile.
#ifdef _WIN32
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
        [[maybe_unused]] int niceness = nice(10);
#endif
        dx::engine::initialize(root.string());
        dx::audio::acoustics::BakeSettings settings;
        settings.threads = std::max(1, (dx::workers::threadCap() + 1) / 2);
        std::printf("Baking into %s\n", dx::audio::acoustics::cacheDirectory().string().c_str());
        int failed = 0;
        for (const std::string& map : maps) {
            dx::audio::acoustics::BatchResult result;
            {
                dx::World world;
                if (!world.loadMap(map)) {
                    std::printf("%s: could not be loaded\n", map.c_str());
                    failed++;
                    dx::objects::unloadPackage(dx::Name(map));
                    continue;
                }
                dx::audio::acoustics::Geometry geometry = dx::audio::acoustics::levelGeometry(world.level);
                result = dx::audio::acoustics::bakeToCache(map, geometry, dx::audio::acoustics::defaultMaterials(), settings, force, {});
            }
            dx::objects::unloadPackage(dx::Name(map));
            if (!result.ok) {
                std::printf("%s: failed: %s\n", map.c_str(), result.message.c_str());
                failed++;
            } else if (result.skipped && result.probes == 0) {
                std::printf("%s: %s\n", map.c_str(), result.message.c_str());
            } else if (result.skipped) {
                std::printf("%s: %zu probes %.1f m apart, %.1f MB, already baked\n", map.c_str(), result.probes, static_cast<double>(result.spacing), static_cast<double>(result.bytes) / 1048576.0);
            } else {
                std::printf("%s: %zu probes %.1f m apart, %.1f MB, %.1f s: %s\n", map.c_str(), result.probes, static_cast<double>(result.spacing), static_cast<double>(result.bytes) / 1048576.0, result.seconds, result.message.c_str());
            }
            std::fflush(stdout);
        }
        std::printf(failed ? "%d failed\n" : "Success\n", failed);
        return failed ? 1 : 0;
    }
}

int main(int argc, char** argv)
{
    dx::random::seed(dx::random::clockSeed());
    CLI::App app("Ambrosia's UCC: the original's commands for building and exporting UnrealScript packages, and Ambrosia's own.", "ucc");
    app.require_subcommand(0, 1);
    app.set_version_flag("--version", dx::version::build());
    // Command names are case-insensitive: ucc Make and ucc make run the same command.
    auto command = [&app](const char* name, const char* description) {
        return app.add_subcommand(name, description)->ignore_case();
    };

    BatchExport batch;
    CLI::App* batchCommand = command("batchexport", "Export a package's objects of a class to files (class uc exports the scripts)");
    batchCommand->add_option("package", batch.package, "Package, such as DeusEx.u")->required();
    batchCommand->add_option("class", batch.className, "Class of the objects to export, such as Texture")->required();
    batchCommand->add_option("extension", batch.extension, "File type to export as, such as bmp")->required();
    batchCommand->add_option("directory", batch.directory, "Directory to write into")->required();

    std::string sourcesPackage;
    std::string sourcesDirectory;
    CLI::App* sourcesCommand = command("exportsources", "Export a package's scripts and its #exec directives' source files");
    sourcesCommand->add_option("package", sourcesPackage, "Package, such as DeusEx.u")->required();
    sourcesCommand->add_option("directory", sourcesDirectory, "Directory to write into")->required();

    CLI::App* makeCommand = command("make", "Rebuild the UnrealScript packages from their sources");

    std::string propsList;
    std::string propsDirectory;
    CLI::App* propsCommand = command("genprops", "Generate the engine's property bindings");
    propsCommand->add_option("list", propsList, "The list of bindings, src/engine/props_bindings.txt")->required();
    propsCommand->add_option("directory", propsDirectory, "Directory to write the sources into")->required();

    std::vector<std::string> bakeMaps;
    bool bakeForce = false;
    CLI::App* bakeCommand = command("bakeacoustics", "Bake maps' acoustics for the raytraced sound mode");
    bakeCommand->add_option("maps", bakeMaps, "Maps to bake, or all")->required();
    bakeCommand->add_flag("--force", bakeForce, "Bake maps whose baked file is already valid too");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        // 0 after --help; 2 after saying what is wrong, as the game's own options.
        return app.exit(error) == 0 ? 0 : 2;
    }
    if (app.get_subcommands().empty()) {
        std::printf("%s", app.help().c_str());
        return 0;
    }
    std::filesystem::path root = dx::config::findGameRoot();
    if (bakeCommand->parsed()) {
        try {
            return bakeAcoustics(root, bakeMaps, bakeForce);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "ucc bakeacoustics failed: %s\n", error.what());
            return 1;
        }
    }
    dx::objects::setEditorMode(true);
    dx::config::setApplyClassDefaults(false);
    std::string name = app.get_subcommands().front()->get_name();
    try {
        dx::engine::initializeCore(root.string());
        if (batchCommand->parsed()) {
            return batchExport(batch);
        }
        if (sourcesCommand->parsed()) {
            return exportSources(sourcesPackage, sourcesDirectory);
        }
        if (makeCommand->parsed()) {
            return make(root);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ucc %s failed: %s\n", name.c_str(), error.what());
        return 1;
    }
    return genProps(propsList, propsDirectory);
}

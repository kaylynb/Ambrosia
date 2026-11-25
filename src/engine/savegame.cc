#include "engine/savegame.h"

#include "core/config.h"
#include "core/files.h"
#include "core/gc.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/package_writer.h"
#include "core/text.h"
#include "core/vm.h"
#include "engine/actor.h"
#include "engine/ai.h"
#include "engine/map_writer.h"
#include "engine/mesh.h"
#include "engine/props_deusex.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <deque>
#include <functional>
#include <unordered_set>

namespace dx {
    namespace savegame {
        namespace {
            struct State {
                std::filesystem::path root;
                std::filesystem::path pending;
                std::function<std::string()> saveExtras;
                std::function<void(const std::string&)> restoreExtras;
                std::function<void()> progressPainter;
                std::function<UObject*()> snapshotSource;
            };

            State& state()
            {
                static State instance;
                return instance;
            }

            std::filesystem::path defaultRoot()
            {
                return config::userDirectory() / "Save";
            }

            bool classNamed(const UClass* cls, std::initializer_list<const char*> names)
            {
                for (const UClass* current = cls; current; current = current->superClass()) {
                    for (const char* candidate : names) {
                        if (current->name == Name(candidate)) {
                            return true;
                        }
                    }
                }
                return false;
            }

            bool transientObject(UObject* object)
            {
                for (const UClass* current = object->cls; current; current = current->superClass()) {
                    if (current->classFlags & CLASS_Transient) {
                        return true;
                    }
                }
                return classNamed(object->cls, { "Window", "GC", "Player", "Canvas", "Console", "Client", "GameDirectory" });
            }

            bool nativeResource(UObject* object)
            {
                return cast<UField>(object) || cast<UTexture>(object) || cast<UPalette>(object) || cast<USound>(object) || cast<UMusic>(object) || cast<UPolys>(object) || cast<UPrimitive>(object) || cast<UFont>(object) || cast<UMesh>(object) || cast<ULevel>(object);
            }

            bool isActor(UObject* object)
            {
                return object->cls && engineClasses().actor && object->isA(engineClasses().actor);
            }

            void forEachReference(UStruct* structType, uint8_t* data, const std::function<void(UObject*)>& visit)
            {
                if (!structType || !data) {
                    return;
                }
                for (UProperty* property : structType->properties) {
                    if (property->propertyFlags & (CPF_Transient | CPF_Native)) {
                        continue;
                    }
                    for (int32_t index = 0; index < property->arrayDim; index++) {
                        uint8_t* memory = data + property->offset + static_cast<size_t>(index) * property->elementSize();
                        if (property->type == PropType::Object || property->type == PropType::Class) {
                            visit(*reinterpret_cast<UObject**>(memory));
                        } else if (property->type == PropType::Struct) {
                            forEachReference(property->structType, memory, visit);
                        }
                    }
                }
            }

            UObject* findLevelInfo(World& world, const char* className)
            {
                for (UObject* a : world.actors) {
                    if (a && !a->destroyed && a->isA(Name(className))) {
                        return a;
                    }
                }
                return nullptr;
            }

            std::string twoDigitPrefix(const std::string& map)
            {
                if (map.size() >= 3 && std::isdigit(static_cast<unsigned char>(map[0])) && std::isdigit(static_cast<unsigned char>(map[1])) && map[2] == '_') {
                    return map.substr(0, 2);
                }
                return {};
            }

            std::string slotName(int32_t index)
            {
                if (index == QuickSaveIndex) {
                    return "QuickSave";
                }
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "Save%04d", index);
                return buffer;
            }

            bool copyLevels(const std::filesystem::path& from, const std::filesystem::path& to)
            {
                std::error_code error;
                std::filesystem::create_directories(to, error);
                if (error) {
                    return false;
                }
                if (!std::filesystem::is_directory(from, error)) {
                    return true;
                }
                bool copied = true;
                for (std::filesystem::directory_iterator it(from, error), end; !error && it != end; it.increment(error)) {
                    const std::filesystem::path& path = it->path();
                    if (path.extension() == ".dxs" && path.filename() != "SaveInfo.dxs") {
                        if (!files::copy(path, to / path.filename())) {
                            copied = false;
                        }
                    }
                }
                return copied && !error;
            }

            // Ambrosia's state rides on the end of SaveInfo.dxs: the text, its length as four
            // little-endian bytes, then a tag. A package is read through the offsets in its
            // header, so the game never looks at bytes past its last export.
            constexpr char extrasTag[8] = { 'A', 'M', 'B', 'R', 'O', 'S', '0', '1' };

            bool appendExtras(const std::filesystem::path& file, const std::string& text)
            {
                std::ofstream out(file, std::ios::binary | std::ios::app);
                uint32_t length = static_cast<uint32_t>(text.size());
                const unsigned char size[4] = { static_cast<unsigned char>(length), static_cast<unsigned char>(length >> 8), static_cast<unsigned char>(length >> 16), static_cast<unsigned char>(length >> 24) };
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
                out.write(reinterpret_cast<const char*>(size), 4);
                out.write(extrasTag, sizeof(extrasTag));
                out.close();
                return !out.fail();
            }

            std::string readExtras(const std::filesystem::path& file)
            {
                std::ifstream in(file, std::ios::binary | std::ios::ate);
                std::streamoff end = in ? static_cast<std::streamoff>(in.tellg()) : 0;
                if (end < 12) {
                    return {};
                }
                char tail[12] = {};
                in.seekg(end - 12);
                in.read(tail, 12);
                if (std::memcmp(tail + 4, extrasTag, sizeof(extrasTag)) != 0) {
                    return {};
                }
                const auto* size = reinterpret_cast<const unsigned char*>(tail);
                std::streamoff length = size[0] | (size[1] << 8) | (size[2] << 16) | (static_cast<std::streamoff>(size[3]) << 24);
                if (length > end - 12) {
                    return {};
                }
                std::string text(static_cast<size_t>(length), '\0');
                in.seekg(end - 12 - length);
                in.read(text.data(), length);
                return text;
            }

            void clearDirectory(const std::filesystem::path& directory)
            {
                std::error_code error;
                if (!std::filesystem::is_directory(directory, error)) {
                    return;
                }
                for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
                    std::error_code removeError;
                    std::filesystem::remove_all(it->path(), removeError);
                }
            }
        }

        void setSaveRoot(const std::filesystem::path& root)
        {
            state().root = root;
        }

        std::filesystem::path saveRoot()
        {
            if (state().root.empty()) {
                state().root = defaultRoot();
            }
            std::error_code error;
            std::filesystem::create_directories(state().root, error);
            return state().root;
        }

        std::filesystem::path currentDirectory()
        {
            return saveRoot() / "Current";
        }

        std::filesystem::path slotDirectory(int32_t index)
        {
            return saveRoot() / slotName(index);
        }

        std::vector<int32_t> slotIndices()
        {
            std::vector<int32_t> result;
            std::error_code error;
            for (std::filesystem::directory_iterator it(saveRoot(), error), end; !error && it != end; it.increment(error)) {
                std::string name = it->path().filename().string();
                std::error_code typeError;
                if (!it->is_directory(typeError) || name.size() != 8 || name.compare(0, 4, "Save") != 0) {
                    continue;
                }
                bool digits = std::all_of(name.begin() + 4, name.end(), [](char c) {
                    return std::isdigit(static_cast<unsigned char>(c)) != 0;
                });
                if (digits) {
                    result.push_back(std::atoi(name.c_str() + 4));
                }
            }
            std::sort(result.begin(), result.end());
            return result;
        }

        int32_t newSlotIndex()
        {
            std::vector<int32_t> used = slotIndices();
            return used.empty() ? 1 : used.back() + 1;
        }

        uint64_t slotSize(int32_t index)
        {
            uint64_t total = 0;
            std::filesystem::path directory = slotDirectory(index);
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error)) {
                return 0;
            }
            for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
                std::error_code fileError;
                if (it->is_regular_file(fileError)) {
                    uintmax_t size = it->file_size(fileError);
                    if (!fileError) {
                        total += static_cast<uint64_t>(size);
                    }
                }
            }
            return total;
        }

        std::string mapBaseName(const std::string& url)
        {
            std::string map = url.substr(0, url.find('?'));
            size_t hash = map.find('#');
            if (hash != std::string::npos) {
                map = map.substr(0, hash);
            }
            map = std::filesystem::path(map).filename().string();
            size_t dot = map.rfind('.');
            if (dot != std::string::npos) {
                map = map.substr(0, dot);
            }
            return map;
        }

        bool saveLevel(World& world, const std::filesystem::path& file, bool includePlayer, std::string* error)
        {
            ULevel* level = world.level;
            if (!level) {
                if (error) {
                    *error = "no level loaded";
                }
                return false;
            }
            UObject* player = world.player();
            std::unordered_set<UObject*> excluded;
            if (!includePlayer && player) {
                for (UObject* a : world.actors) {
                    if (!a || a->destroyed) {
                        continue;
                    }
                    UObject* owner = a;
                    int guard = 0;
                    while (owner && owner != player && guard++ < 32) {
                        owner = actor::Owner(owner);
                    }
                    if (owner == player) {
                        excluded.insert(a);
                    }
                }
            }

            PackageWriter writer(Name("SaveGame"));
            UObject* mapPackage = level->outermost();
            writer.addExport(level, level->outer);
            std::deque<UObject*> queue;
            for (UObject* a : world.actors) {
                if (a && !a->destroyed && !excluded.count(a) && !writer.isExport(a) && !transientObject(a)) {
                    writer.addExport(a, a->outer);
                    queue.push_back(a);
                }
            }

            auto exportable = [&](UObject* object) {
                // A reference left at an object freed during play is written as None.
                if (!object || gc::wasFreed(object) || object->destroyed || writer.isExport(object) || !object->cls || !object->outer) {
                    return false;
                }
                if (isActor(object) || transientObject(object) || nativeResource(object)) {
                    return false;
                }
                return !object->linker || object->linker->savedGame || object->outermost() == mapPackage;
            };
            auto visit = [&](UObject* object) {
                if (exportable(object)) {
                    writer.addExport(object, object->outer);
                    queue.push_back(object);
                }
            };
            auto addModel = [&](UModel* model) {
                if (!model || writer.isExport(model) || model->outermost() != mapPackage) {
                    return;
                }
                objects::ensureLoaded(model);
                writer.addExport(model, model->outer);
                if (model->polys && !writer.isExport(model->polys) && model->polys->outermost() == mapPackage) {
                    objects::ensureLoaded(model->polys);
                    writer.addExport(model->polys, model->polys->outer);
                }
            };
            addModel(level->model);
            for (UObject* a : world.actors) {
                if (a && !a->destroyed && writer.isExport(a)) {
                    addModel(cast<UModel>(actor::Brush(a)));
                }
            }
            while (!queue.empty()) {
                UObject* object = queue.front();
                queue.pop_front();
                if (object->outer && exportable(object->outer)) {
                    visit(object->outer);
                }
                forEachReference(object->cls, object->data, visit);
                aiReferences(object, visit);
            }
            objects::flushLoads();

            writer.setFlagSelector([&](UObject* object) {
                constexpr uint32_t loadFlags = 0x00070000;
                if (isActor(object)) {
                    return loadFlags | RF_Transactional;
                }
                if (object->outermost() == mapPackage) {
                    return loadFlags | (object->flags & (RF_Transactional | RF_Public | RF_Standalone));
                }
                return loadFlags;
            });

            writer.setFilter([&](UObject* object) {
                if (!object->outer) {
                    return true;
                }
                if (cast<UClass>(object)) {
                    return true;
                }
                if (isActor(object) || transientObject(object)) {
                    return false;
                }
                return object->linker && !object->linker->savedGame;
            });

            std::string map = mapBaseName(world.url);
            writer.setNativeSerializer([&](PackageWriter& out, UObject* object, Writer& data) {
                if (aiWriteNative(out, object, data)) {
                    return;
                }
                if (object == level) {
                    int32_t count = static_cast<int32_t>(world.actors.size());
                    data.i32(count);
                    data.i32(count);
                    for (UObject* a : world.actors) {
                        out.writeObject(data, a && !a->destroyed && out.isExport(a) ? a : nullptr);
                    }
                    std::vector<std::string> options;
                    size_t mark = world.url.find('?');
                    while (mark != std::string::npos) {
                        size_t next = world.url.find('?', mark + 1);
                        std::string option = world.url.substr(mark + 1, next == std::string::npos ? std::string::npos : next - mark - 1);
                        if (!option.empty()) {
                            options.push_back(option);
                        }
                        mark = next;
                    }
                    if (options.empty() && player) {
                        options.push_back("Name=Player");
                        options.push_back("Class=" + std::string(player->cls->outermost()->name.c_str()) + "." + player->cls->name.c_str());
                    }
                    data.string("deusex");
                    data.string("");
                    data.string(map);
                    data.string("");
                    data.index(static_cast<int32_t>(options.size()));
                    for (const std::string& option : options) {
                        data.string(option);
                    }
                    data.i32(7790);
                    data.u32(1);
                    out.writeObject(data, level->model);
                    data.index(static_cast<int32_t>(level->reachSpecs.size()));
                    for (const ReachSpec& spec : level->reachSpecs) {
                        data.i32(spec.distance);
                        out.writeObject(data, spec.start);
                        out.writeObject(data, spec.end);
                        data.i32(spec.collisionRadius);
                        data.i32(spec.collisionHeight);
                        data.i32(spec.reachFlags);
                        data.u8(spec.pruned);
                    }
                    data.f32(world.timeSeconds);
                    out.writeObject(data, nullptr);
                    for (UObject* block : level->textBlocks) {
                        out.writeObject(data, block);
                    }
                    data.index(static_cast<int32_t>(level->travelInfo.size()));
                    for (const auto& [key, value] : level->travelInfo) {
                        data.string(key);
                        data.string(value);
                    }
                    return;
                }
                if (UModel* model = cast<UModel>(object)) {
                    mapwriter::writeModel(out, *model, data);
                    return;
                }
                if (UPolys* polys = cast<UPolys>(object)) {
                    mapwriter::writePolys(out, *polys, data);
                    return;
                }
                if (classNamed(object->cls, { "ExtString" })) {
                    uint8_t* text = object->propertyAddress(Name("Text"));
                    data.string(text ? *reinterpret_cast<std::string*>(text) : std::string());
                }
            });

            return writer.save(file, error);
        }

        namespace {
            // DDeusExGameEngine::SaveCurrentLevel sets LevelAction to
            // LEVACT_Saving and calls PaintProgress before it writes the level, for a save and for
            // the level left at a transition alike. The original leaves the action set while it
            // writes; here it is put back first, so a saved level never carries it.
            void paintSaving(World& world)
            {
                if (!state().progressPainter || !world.levelInfo || !world.levelInfo->data) {
                    return;
                }
                levelinfo::LevelAction(world.levelInfo) = 2;
                state().progressPainter();
                levelinfo::LevelAction(world.levelInfo) = 0;
            }
        }

        void setProgressPainter(std::function<void()> painter)
        {
            state().progressPainter = std::move(painter);
        }

        void setSnapshotSource(std::function<UObject*()> source)
        {
            state().snapshotSource = std::move(source);
        }

        bool saveGame(World& world, int32_t index, const std::string& description)
        {
            std::string map = mapBaseName(world.url);
            if (!world.level || map.empty()) {
                return false;
            }
            if (index == 0) {
                index = newSlotIndex();
            }
            // The picture the load screen shows, of the last frame shown, taken before the
            // SAVING frame below replaces it. The save screen hides its UI first; a quicksave
            // keeps the HUD, as in the original.
            UTexture* snapshot = state().snapshotSource ? cast<UTexture>(state().snapshotSource()) : nullptr;
            UPalette* snapshotPalette = snapshot ? snapshot->palette() : nullptr;
            auto dropSnapshot = [&snapshot, &snapshotPalette] {
                if (snapshot) {
                    objects::destroyObject(snapshot);
                    snapshot = nullptr;
                }
                if (snapshotPalette) {
                    objects::destroyObject(snapshotPalette);
                    snapshotPalette = nullptr;
                }
            };
            paintSaving(world);
            UObject* player = world.player();
            bool deusExPlayer = player && engineClasses().deusExPlayer && player->isA(engineClasses().deusExPlayer);
            if (deusExPlayer) {
                deusexplayer::saveCount(player) += 1;
            }
            // UGameEngine::SaveGame clears references to destroyed actors first.
            world.cleanupDestroyed(true);
            // That can delete a destroyed player, so ask the world again.
            player = world.player();
            deusExPlayer = player && engineClasses().deusExPlayer && player->isA(engineClasses().deusExPlayer);
            std::string error;
            std::filesystem::path current = currentDirectory();
            if (!saveLevel(world, current / (map + ".dxs"), true, &error)) {
                log::error("DevSave", "save failed: {}", error);
                dropSnapshot();
                return false;
            }
            std::filesystem::path slot = slotDirectory(index);
            std::filesystem::path building = slot;
            building += ".new";
            std::filesystem::path previous = slot;
            previous += ".old";
            std::error_code ignored;
            std::filesystem::remove_all(building, ignored);
            auto fail = [&](const char* what) {
                log::error("DevSave", "save failed: {}", what);
                std::error_code cleanup;
                std::filesystem::remove_all(building, cleanup);
                dropSnapshot();
                return false;
            };
            if (!copyLevels(current, building)) {
                return fail("cannot copy levels");
            }

            UClass* infoClass = objects::findClass("DeusEx.DeusExSaveInfo");
            if (!infoClass) {
                return fail("no save info class");
            }
            objects::flushLoads();
            UObject* info = objects::newObject(infoClass, objects::packageObject(Name("Transient")), Name("MyDeusExSaveInfo"), 0);
            vm::Call stamp(info, Name("UpdateTimeStamp"));
            if (stamp.valid()) {
                stamp.run();
            }
            UObject* levelInfo = findLevelInfo(world, "DeusExLevelInfo");
            deusexsaveinfo::DirectoryIndex(info) = index;
            deusexsaveinfo::Description(info) = description;
            deusexsaveinfo::MissionLocation(info) = levelInfo ? deusexlevelinfo::MissionLocation(levelInfo) : std::string();
            deusexsaveinfo::MapName(info) = map;
            deusexsaveinfo::saveCount(info) = deusExPlayer ? deusexplayer::saveCount(player) : 0;
            deusexsaveinfo::saveTime(info) = deusExPlayer ? static_cast<int32_t>(deusexplayer::saveTime(player)) : 0;
            deusexsaveinfo::bCheatsEnabled(info) = player ? playerpawn::bCheatsEnabled(player) : 0;
            deusexsaveinfo::Snapshot(info) = snapshot;
            PackageWriter infoWriter(Name("SaveInfo"));
            infoWriter.addExport(info, nullptr);
            if (snapshot) {
                infoWriter.addExport(snapshot, nullptr);
            }
            if (snapshotPalette) {
                infoWriter.addExport(snapshotPalette, nullptr);
            }
            infoWriter.setFilter([](UObject* object) {
                return !object->outer || cast<UClass>(object) || (object->linker && !object->linker->savedGame);
            });
            infoWriter.setNativeSerializer([](PackageWriter& out, UObject* object, Writer& data) {
                if (UPalette* palette = cast<UPalette>(object)) {
                    data.index(static_cast<int32_t>(palette->colors.size()));
                    for (uint32_t color : palette->colors) {
                        data.u32(color);
                    }
                    return;
                }
                UTexture* texture = cast<UTexture>(object);
                if (!texture) {
                    return;
                }
                data.index(static_cast<int32_t>(texture->mips.size()));
                for (const TextureMip& mip : texture->mips) {
                    size_t skip = out.beginLazyArray(data);
                    data.index(static_cast<int32_t>(mip.data.size()));
                    data.bytes(mip.data.data(), mip.data.size());
                    out.endLazyArray(data, skip);
                    data.u32(mip.width);
                    data.u32(mip.height);
                    data.u8(mip.ubits);
                    data.u8(mip.vbits);
                }
            });
            infoWriter.setFlagSelector([](UObject*) {
                return 0x00070000u | RF_Public;
            });
            bool saved = infoWriter.save(building / "SaveInfo.dxs", &error);
            objects::destroyObject(info);
            dropSnapshot();
            if (!saved) {
                return fail(error.c_str());
            }
            if (!appendExtras(building / "SaveInfo.dxs", state().saveExtras ? state().saveExtras() : std::string())) {
                return fail("cannot write save extras");
            }
            std::filesystem::remove_all(previous, ignored);
            std::error_code moveError;
            bool hadSlot = std::filesystem::exists(slot, moveError);
            if (hadSlot) {
                std::filesystem::rename(slot, previous, moveError);
                if (moveError) {
                    return fail("cannot move old save aside");
                }
            }
            std::filesystem::rename(building, slot, moveError);
            if (moveError) {
                if (hadSlot) {
                    std::error_code restore;
                    std::filesystem::rename(previous, slot, restore);
                }
                return fail("cannot move new save in place");
            }
            std::filesystem::remove_all(previous, ignored);
            return true;
        }

        bool deleteGame(int32_t index)
        {
            std::error_code error;
            return std::filesystem::remove_all(slotDirectory(index), error) > 0;
        }

        void setExtraState(std::function<std::string()> save, std::function<void(const std::string&)> restore)
        {
            state().saveExtras = std::move(save);
            state().restoreExtras = std::move(restore);
        }

        void deleteCurrent()
        {
            clearDirectory(currentDirectory());
            state().pending.clear();
            if (state().restoreExtras) {
                state().restoreExtras({});
            }
        }

        UObject* readSaveInfo(int32_t index)
        {
            std::filesystem::path file = slotDirectory(index) / "SaveInfo.dxs";
            std::error_code error;
            if (!std::filesystem::is_regular_file(file, error)) {
                return nullptr;
            }
            try {
                Linker* linker = objects::loadPackageFile(Name("SaveInfo_" + slotName(index)), file, true);
                const Package& package = linker->file();
                for (int32_t ref = 1; ref <= static_cast<int32_t>(package.exports().size()); ref++) {
                    if (package.classOf(ref) == "DeusExSaveInfo") {
                        UObject* info = linker->object(ref);
                        objects::ensureLoaded(info);
                        objects::flushLoads();
                        deusexsaveinfo::DirectoryIndex(info) = index;
                        return info;
                    }
                }
            } catch (const std::exception& failure) {
                log::error("DevLoad", "cannot read save info {}: {}", file.string(), failure.what());
            }
            return nullptr;
        }

        bool loadRequested(const std::string& url, int32_t& index)
        {
            std::string options = lowered(url.substr(std::min(url.find('?'), url.size())));
            size_t at = options.find("?loadgame=");
            if (at == std::string::npos) {
                return false;
            }
            index = std::atoi(options.c_str() + at + 10);
            return true;
        }

        bool prepareLoad(int32_t index, std::string& map)
        {
            UObject* info = readSaveInfo(index);
            if (!info) {
                return false;
            }
            map = deusexsaveinfo::MapName(info);
            std::filesystem::path slot = slotDirectory(index);
            std::error_code error;
            if (map.empty() || !std::filesystem::is_regular_file(slot / (map + ".dxs"), error)) {
                return false;
            }
            std::filesystem::path current = currentDirectory();
            clearDirectory(current);
            copyLevels(slot, current);
            if (state().restoreExtras) {
                state().restoreExtras(readExtras(slot / "SaveInfo.dxs"));
            }
            return true;
        }

        void storeDepartingLevel(World& world, const std::string& targetUrl)
        {
            paintSaving(world);
            // UGameEngine::LoadMap clears references to destroyed actors before leaving.
            world.cleanupDestroyed(true);
            std::string map = mapBaseName(world.url);
            std::string target = mapBaseName(targetUrl);
            std::filesystem::path current = currentDirectory();
            std::error_code error;
            UObject* levelInfo = findLevelInfo(world, "DeusExLevelInfo");
            int32_t mission = levelInfo ? deusexlevelinfo::missionNumber(levelInfo) : -1;
            if (!map.empty() && lowered(map) == lowered(target)) {
                std::filesystem::remove(current / (map + ".dxs"), error);
            } else if (!map.empty() && mission >= 0 && mission < 98) {
                std::string message;
                if (!saveLevel(world, current / (map + ".dxs"), false, &message)) {
                    log::error("DevSave", "cannot store {}: {}", map, message);
                }
            }
            std::string prefix = twoDigitPrefix(target);
            if (prefix.empty() || !std::filesystem::is_directory(current, error)) {
                return;
            }
            std::vector<std::filesystem::path> stale;
            for (std::filesystem::directory_iterator it(current, error), end; !error && it != end; it.increment(error)) {
                if (it->path().extension() == ".dxs" && twoDigitPrefix(it->path().stem().string()) != prefix) {
                    stale.push_back(it->path());
                }
            }
            for (const std::filesystem::path& path : stale) {
                std::error_code removeError;
                std::filesystem::remove(path, removeError);
            }
        }

        bool selectLevel(const std::string& url)
        {
            std::filesystem::path file = currentDirectory() / (mapBaseName(url) + ".dxs");
            std::error_code error;
            if (!std::filesystem::is_regular_file(file, error)) {
                state().pending.clear();
                return false;
            }
            state().pending = file;
            return true;
        }

        Linker* takePendingLevel(const std::string& map)
        {
            if (state().pending.empty()) {
                return nullptr;
            }
            std::filesystem::path file = state().pending;
            state().pending.clear();
            std::string base = mapBaseName(map);
            if (lowered(file.stem().string()) != lowered(base)) {
                return nullptr;
            }
            try {
                return objects::loadPackageFile(Name("SaveGame_" + base), file, true);
            } catch (const std::exception& failure) {
                log::error("DevLoad", "cannot open saved level {}: {}", file.string(), failure.what());
                return nullptr;
            }
        }

        void levelRestored(World& world)
        {
            if (world.level) {
                world.timeSeconds = world.level->timeSeconds;
            }
            static const Name bindEvents("ConBindEvents");
            std::vector<UObject*> snapshot = world.actors;
            for (UObject* a : snapshot) {
                if (!a || a->destroyed) {
                    continue;
                }
                UFunction* function = vm::findFunction(a, bindEvents);
                if (function) {
                    vm::Call call(a, function);
                    call.run();
                }
            }
        }
    }
}

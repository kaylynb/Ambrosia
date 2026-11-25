#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dx {
    class World;
    class Linker;
    class UObject;

    namespace savegame {
        constexpr int32_t QuickSaveIndex = -1;
        // Ambrosia's own state for the game being played, such as the gamepad quick slots, as
        // text added to the end of each slot's SaveInfo.dxs. save gives the text when a game is
        // saved; restore gets it back when one is loaded, and an empty text when a new game
        // starts.
        void setExtraState(std::function<std::string()> save, std::function<void(const std::string&)> restore);

        void setSaveRoot(const std::filesystem::path& root);
        std::filesystem::path saveRoot();
        std::filesystem::path currentDirectory();
        std::filesystem::path slotDirectory(int32_t index);
        std::vector<int32_t> slotIndices();
        int32_t newSlotIndex();
        uint64_t slotSize(int32_t index);
        std::string mapBaseName(const std::string& url);

        bool saveLevel(World& world, const std::filesystem::path& file, bool includePlayer, std::string* error);
        bool saveGame(World& world, int32_t index, const std::string& description);
        bool deleteGame(int32_t index);
        void deleteCurrent();
        UObject* readSaveInfo(int32_t index);
        bool loadRequested(const std::string& url, int32_t& index);
        bool prepareLoad(int32_t index, std::string& map);
        void storeDepartingLevel(World& world, const std::string& targetUrl);
        // Draws one frame while the level says it is saving, so the console prints its SAVING
        // message. The game sets it; without one nothing is drawn.
        void setProgressPainter(std::function<void()> painter);
        // Gives the picture stored with a save, a transient texture the save destroys once it
        // is written, or null for none. The game sets it.
        void setSnapshotSource(std::function<UObject*()> source);
        bool selectLevel(const std::string& url);
        Linker* takePendingLevel(const std::string& map);
        void levelRestored(World& world);
    }
}

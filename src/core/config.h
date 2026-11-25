#pragma once

#include "core/object.h"
#include "core/subscription.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dx {
    class Ini {
    public:
        bool load(const std::filesystem::path& path);
        bool save() const;
        std::optional<std::string> get(std::string_view section, std::string_view key) const;
        std::vector<std::string> getAll(std::string_view section, std::string_view key) const;
        void set(std::string_view section, std::string_view key, std::string_view value);
        std::vector<std::pair<std::string, std::string>> section(std::string_view section) const;

        std::filesystem::path path;
        // When set, what save leaves out of the file, though it stays in memory: called with a
        // section and an empty key for the whole section, then with each of its keys.
        std::function<bool(std::string_view section, std::string_view key)> leaveOut;

    private:
        struct Section {
            std::string name;
            std::vector<std::pair<std::string, std::string>> entries;
        };

        const Section* find(std::string_view name) const;
        std::vector<Section> sections;
    };

    namespace config {
        // The first of platform::gameDataCandidates that holds System/DeusEx.u, or an empty path.
        std::filesystem::path findGameRoot();
        std::filesystem::path userDirectory();
        void initialize(const std::filesystem::path& systemDirectory);
        Ini& system();
        Ini& user();
        Ini* forName(Name configName);
        Ini* localization(std::string_view package);
        std::string localize(std::string_view section, std::string_view key, std::string_view package);
        void applyClassDefaults(UClass* cls);
        void setApplyClassDefaults(bool enabled);
        void applyObjectConfig(UObject* object);
        void saveObjectConfig(UObject* object, UClass* cls);
        Name configNameOf(const UClass* cls);

        // A setting written while the program runs: the ini it is in, its section and its key.
        struct SettingChange {
            const Ini& ini;
            std::string_view section;
            std::string_view key;
        };

        // How code that acts on a setting learns that it changed. The notice is given by
        // settings::write (core/settings.h), the one write path: the console's set ini:, which
        // the menus' choices go through, and the engine's and game's own writes. Ini::set alone
        // gives none, so tests writing settings directly are not announced, nor is class
        // config's SaveConfig. Listeners are told in the order they subscribed, on the thread that
        // wrote the setting (the game thread), once the ini has been saved or failed to save;
        // one may unsubscribe while told.
        [[nodiscard]] Subscription onSettingChanged(std::function<void(const SettingChange& change)> listener);
        void settingChanged(const Ini& ini, std::string_view section, std::string_view key);
    }
}

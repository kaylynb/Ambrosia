#include "core/config.h"

#include "core/files.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/package.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/text.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace dx {
    namespace {
        std::string trim(std::string_view text)
        {
            size_t begin = 0;
            size_t end = text.size();
            while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
                begin++;
            }
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
                end--;
            }
            return std::string(text.substr(begin, end - begin));
        }

        struct ConfigState {
            std::filesystem::path systemDirectory;
            Ini system;
            Ini user;
            std::unordered_map<std::string, std::unique_ptr<Ini>> others;
            std::unordered_map<std::string, std::unique_ptr<Ini>> localizations;
        };

        ConfigState& state()
        {
            static ConfigState instance;
            return instance;
        }

        std::string packageOf(const UObject* object)
        {
            const UObject* current = object;
            while (current->outer) {
                current = current->outer;
            }
            return current->name.str();
        }

        std::string configSection(const UProperty* property, const UClass* cls)
        {
            const UClass* owner = cast<UClass>(property->outer);
            if ((property->propertyFlags & CPF_GlobalConfig) && owner) {
                cls = owner;
            }
            return packageOf(cls) + "." + cls->name.str();
        }

        void applyProperty(UProperty* property, uint8_t* data, Ini& ini, const std::string& section)
        {
            if (property->arrayDim > 1) {
                for (int32_t i = 0; i < property->arrayDim; i++) {
                    auto value = ini.get(section, property->name.str() + "[" + std::to_string(i) + "]");
                    if (value) {
                        property->fromText(data + property->offset + static_cast<size_t>(i) * property->elementSize(), *value);
                    }
                }
                return;
            }
            auto value = ini.get(section, property->name.str());
            if (value) {
                property->fromText(data + property->offset, *value);
            }
        }

        void loadConfigPropagated(UObject* object, UClass* cls, Ini& ini)
        {
            if (!(cls->classFlags & CLASS_Config)) {
                return;
            }
            if (cls->superClass()) {
                loadConfigPropagated(object, cls->superClass(), ini);
            }
            for (UProperty* property : cls->properties) {
                if (property->propertyFlags & CPF_Config) {
                    applyProperty(property, object->data, ini, configSection(property, cls));
                }
            }
        }
    }

    bool Ini::load(const std::filesystem::path& filePath)
    {
        path = filePath;
        std::optional<std::string> read = files::readFile(filePath);
        if (!read) {
            return false;
        }
        std::string content = std::move(*read);
        if (content.size() >= 2 && static_cast<uint8_t>(content[0]) == 0xff && static_cast<uint8_t>(content[1]) == 0xfe) {
            std::string narrow;
            for (size_t i = 2; i + 1 < content.size(); i += 2) {
                uint16_t c = static_cast<uint16_t>(static_cast<uint8_t>(content[i]) | (static_cast<uint8_t>(content[i + 1]) << 8));
                narrow.push_back(c < 256 ? static_cast<char>(c) : '?');
            }
            content = std::move(narrow);
        }
        Section* current = nullptr;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t end = content.find('\n', pos);
            if (end == std::string::npos) {
                end = content.size();
            }
            std::string line = trim(std::string_view(content).substr(pos, end - pos));
            pos = end + 1;
            if (line.empty() || line[0] == ';') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                std::string sectionName = line.substr(1, line.size() - 2);
                current = nullptr;
                for (Section& existing : sections) {
                    if (iequals(existing.name, sectionName)) {
                        current = &existing;
                    }
                }
                if (!current) {
                    sections.push_back({ sectionName, {} });
                    current = &sections.back();
                }
                continue;
            }
            size_t equals = line.find('=');
            if (!current || equals == std::string::npos) {
                continue;
            }
            current->entries.emplace_back(trim(line.substr(0, equals)), trim(line.substr(equals + 1)));
        }
        return true;
    }

    bool Ini::save() const
    {
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                return false;
            }
            for (const Section& section : sections) {
                if (leaveOut && leaveOut(section.name, {})) {
                    continue;
                }
                std::vector<const std::pair<std::string, std::string>*> kept;
                for (const auto& entry : section.entries) {
                    if (!leaveOut || !leaveOut(section.name, entry.first)) {
                        kept.push_back(&entry);
                    }
                }
                if (kept.empty() && !section.entries.empty()) {
                    continue;
                }
                file << "[" << section.name << "]\r\n";
                for (const auto* entry : kept) {
                    file << entry->first << "=" << entry->second << "\r\n";
                }
                file << "\r\n";
            }
            file.close();
            if (!file) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
        }
        std::error_code error;
        files::replace(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    const Ini::Section* Ini::find(std::string_view name) const
    {
        for (const Section& section : sections) {
            if (iequals(section.name, name)) {
                return &section;
            }
        }
        return nullptr;
    }

    std::optional<std::string> Ini::get(std::string_view sectionName, std::string_view key) const
    {
        const Section* section = find(sectionName);
        if (!section) {
            return std::nullopt;
        }
        for (const auto& [entryKey, value] : section->entries) {
            if (iequals(entryKey, key)) {
                return value;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> Ini::getAll(std::string_view sectionName, std::string_view key) const
    {
        std::vector<std::string> values;
        const Section* section = find(sectionName);
        if (!section) {
            return values;
        }
        for (const auto& [entryKey, value] : section->entries) {
            if (iequals(entryKey, key)) {
                values.push_back(value);
            }
        }
        return values;
    }

    void Ini::set(std::string_view sectionName, std::string_view key, std::string_view value)
    {
        Section* section = nullptr;
        for (Section& existing : sections) {
            if (iequals(existing.name, sectionName)) {
                section = &existing;
            }
        }
        if (!section) {
            sections.push_back({ std::string(sectionName), {} });
            section = &sections.back();
        }
        for (auto& [entryKey, entryValue] : section->entries) {
            if (iequals(entryKey, key)) {
                entryValue = value;
                return;
            }
        }
        section->entries.emplace_back(std::string(key), std::string(value));
    }

    std::vector<std::pair<std::string, std::string>> Ini::section(std::string_view sectionName) const
    {
        const Section* found = find(sectionName);
        if (!found) {
            return {};
        }
        return found->entries;
    }

    namespace config {
        std::filesystem::path findGameRoot()
        {
            std::filesystem::path found;
            for (const std::filesystem::path& candidate : platform::gameDataCandidates()) {
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate / "System" / "DeusEx.u", error)) {
                    found = candidate;
                    break;
                }
            }
            // Said once per folder, so the log shows which copy of the game was read.
            static std::mutex mutex;
            static std::filesystem::path said;
            std::lock_guard lock(mutex);
            if (!found.empty() && found != said) {
                said = found;
                log::info("Config", "game data: {}", found.string());
            }
            return found;
        }

        std::filesystem::path userDirectory()
        {
            // $XDG_DATA_HOME/ambrosia when it is set, on every platform, so tests can isolate their
            // user data; otherwise the platform's own place (platform::userDataDirectory).
            const char* xdg = std::getenv("XDG_DATA_HOME");
            std::filesystem::path directory = xdg && *xdg ? std::filesystem::path(xdg) / "ambrosia" : platform::userDataDirectory();
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            return directory;
        }

        void loadPreferred(Ini& ini, const std::filesystem::path& userFile, const std::filesystem::path& gameFile, const std::filesystem::path& fallbackFile)
        {
            if (!ini.load(userFile) && !ini.load(gameFile)) {
                ini.load(fallbackFile);
            }
            ini.path = userFile;
        }

        void initialize(const std::filesystem::path& systemDirectory)
        {
            ConfigState& s = state();
            s.systemDirectory = systemDirectory;
            std::filesystem::path userDir = userDirectory();
            loadPreferred(s.system, userDir / "DeusEx.ini", systemDirectory / "DeusEx.ini", systemDirectory / "Default.ini");
            loadPreferred(s.user, userDir / "User.ini", systemDirectory / "User.ini", systemDirectory / "DefUser.ini");
            // What Ambrosia writes leaves out the keys and sections nothing reads (settings.h).
            s.system.leaveOut = [](std::string_view section, std::string_view key) { return settings::leftOut(settings::File::System, section, key); };
            s.user.leaveOut = [](std::string_view section, std::string_view key) { return settings::leftOut(settings::File::User, section, key); };
        }

        Ini& system()
        {
            return state().system;
        }

        Ini& user()
        {
            return state().user;
        }

        Ini* forName(Name configName)
        {
            if (configName.none() || configName == Name("System")) {
                return &state().system;
            }
            if (configName == Name("User")) {
                return &state().user;
            }
            std::string key = lowered(configName.str());
            auto it = state().others.find(key);
            if (it != state().others.end()) {
                return it->second.get();
            }
            auto ini = std::make_unique<Ini>();
            loadPreferred(*ini, userDirectory() / (configName.str() + ".ini"), state().systemDirectory / (configName.str() + ".ini"), state().systemDirectory / (configName.str() + ".ini"));
            Ini* result = ini.get();
            state().others.emplace(key, std::move(ini));
            return result;
        }

        Ini* localization(std::string_view package)
        {
            std::string key = lowered(package);
            auto it = state().localizations.find(key);
            if (it != state().localizations.end()) {
                return it->second.get();
            }
            auto ini = std::make_unique<Ini>();
            if (!ini->load(state().systemDirectory / (std::string(package) + ".int"))) {
                std::error_code error;
                std::filesystem::directory_iterator items(state().systemDirectory, error);
                if (error) {
                    log::warn("DevConfig", "cannot list the system directory for {}.int: {}", package, error.message());
                }
                for (auto end = std::filesystem::directory_iterator(); !error && items != end; items.increment(error)) {
                    std::u8string filename = items->path().filename().u8string();
                    if (iequals(std::string(filename.begin(), filename.end()), std::string(package) + ".int")) {
                        ini->load(items->path());
                    }
                }
            }
            Ini* result = ini.get();
            state().localizations.emplace(key, std::move(ini));
            return result;
        }

        std::string localize(std::string_view section, std::string_view key, std::string_view package)
        {
            Ini* ini = localization(package);
            if (ini) {
                auto value = ini->get(section, key);
                if (value) {
                    std::string text = *value;
                    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                        text = text.substr(1, text.size() - 2);
                    }
                    return text;
                }
            }
            return "<?int?" + std::string(package) + "." + std::string(section) + "." + std::string(key) + "?>";
        }

        Name configNameOf(const UClass* cls)
        {
            for (const UClass* current = cls; current; current = current->superClass()) {
                if (!current->configName.none()) {
                    return current->configName;
                }
            }
            return Name("System");
        }

        namespace {
            bool& classDefaultsEnabled()
            {
                static bool enabled = true;
                return enabled;
            }
        }

        void setApplyClassDefaults(bool enabled)
        {
            classDefaultsEnabled() = enabled;
        }

        void applyClassDefaults(UClass* cls)
        {
            if (!classDefaultsEnabled()) {
                return;
            }
            uint8_t* data = cls->defaultObject->data;
            std::string package = packageOf(cls);
            Ini* ini = forName(configNameOf(cls));
            for (UProperty* property : cls->properties) {
                if ((property->propertyFlags & CPF_Config) && ini) {
                    applyProperty(property, data, *ini, configSection(property, cls));
                }
                if (property->propertyFlags & CPF_Localized) {
                    Ini* loc = localization(package);
                    if (loc) {
                        applyProperty(property, data, *loc, cls->name.str());
                    }
                }
            }
        }

        void applyObjectConfig(UObject* object)
        {
            UClass* cls = object->cls;
            Ini* ini = forName(configNameOf(cls));
            if (!ini) {
                return;
            }
            for (UProperty* property : cls->properties) {
                if (property->propertyFlags & (CPF_Config | CPF_GlobalConfig)) {
                    applyProperty(property, object->data, *ini, configSection(property, cls));
                }
            }
        }

        void saveObjectConfig(UObject* object, UClass* cls)
        {
            Ini* ini = forName(configNameOf(cls));
            if (!ini) {
                return;
            }
            for (UProperty* property : cls->properties) {
                if (!(property->propertyFlags & (CPF_Config | CPF_GlobalConfig))) {
                    continue;
                }
                std::string section = configSection(property, cls);
                if (property->arrayDim > 1) {
                    for (int32_t i = 0; i < property->arrayDim; i++) {
                        ini->set(section, property->name.str() + "[" + std::to_string(i) + "]", property->toText(object->data + property->offset + static_cast<size_t>(i) * property->elementSize()));
                    }
                } else {
                    ini->set(section, property->name.str(), property->toText(object->data + property->offset));
                }
            }
            ini->save();
            UClass* top = object->cls;
            while (top->superClass() && (top->superClass()->classFlags & CLASS_Config)) {
                top = top->superClass();
            }
            for (UObject* candidate : objects::all()) {
                UClass* loaded = cast<UClass>(candidate);
                if (loaded && loaded->defaultObject && loaded->isChildOf(top)) {
                    Ini* defaults = forName(configNameOf(loaded));
                    if (defaults) {
                        loadConfigPropagated(loaded->defaultObject, loaded, *defaults);
                    }
                }
            }
        }

        namespace {
            struct SettingListener {
                uint64_t id = 0;
                std::function<void(const SettingChange&)> listener;
            };

            struct SettingListeners {
                std::vector<SettingListener> entries;
                uint64_t nextId = 1;
            };

            // Never freed, so a subscription held by a static can give up its place at exit
            // whatever order statics go in.
            SettingListeners& settingListeners()
            {
                static SettingListeners* instance = new SettingListeners;
                return *instance;
            }
        }

        Subscription onSettingChanged(std::function<void(const SettingChange& change)> listener)
        {
            SettingListeners& listeners = settingListeners();
            uint64_t id = listeners.nextId++;
            listeners.entries.push_back({ id, std::move(listener) });
            return Subscription([](uint64_t gone) { std::erase_if(settingListeners().entries, [&](const SettingListener& entry) { return entry.id == gone; }); }, id);
        }

        void settingChanged(const Ini& ini, std::string_view section, std::string_view key)
        {
            SettingChange change { ini, section, key };
            std::vector<uint64_t> ids;
            for (const SettingListener& entry : settingListeners().entries) {
                ids.push_back(entry.id);
            }
            for (uint64_t id : ids) {
                std::vector<SettingListener>& entries = settingListeners().entries;
                auto found = std::find_if(entries.begin(), entries.end(), [&](const SettingListener& entry) { return entry.id == id; });
                if (found != entries.end()) {
                    // A copy, as the listener may unsubscribe and so free its own entry.
                    std::function<void(const SettingChange&)> call = found->listener;
                    if (call) {
                        call(change);
                    }
                }
            }
        }
    }
}

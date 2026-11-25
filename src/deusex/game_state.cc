#include "deusex/game_state.h"

#include "engine/savegame.h"

#include <map>
#include <sstream>

namespace dx::gamestate {
    namespace {
        std::map<std::string, std::map<std::string, std::string>> sections;
    }

    std::string get(const std::string& section, const std::string& key)
    {
        auto found = sections.find(section);
        if (found == sections.end()) {
            return {};
        }
        auto entry = found->second.find(key);
        return entry == found->second.end() ? std::string() : entry->second;
    }

    void set(const std::string& section, const std::string& key, const std::string& value)
    {
        if (value.empty()) {
            sections[section].erase(key);
        } else {
            sections[section][key] = value;
        }
    }

    bool has(const std::string& section, const std::string& key)
    {
        auto found = sections.find(section);
        return found != sections.end() && found->second.count(key) > 0;
    }

    bool add(const std::string& section, const std::string& key)
    {
        return sections[section].emplace(key, std::string()).second;
    }

    std::string save()
    {
        std::string text;
        for (const auto& [section, entries] : sections) {
            if (entries.empty()) {
                continue;
            }
            text += "[" + section + "]\n";
            for (const auto& [key, value] : entries) {
                text += value.empty() ? key + "\n" : key + "=" + value + "\n";
            }
        }
        return text;
    }

    void restore(const std::string& text)
    {
        sections.clear();
        std::istringstream in(text);
        std::map<std::string, std::string>* current = nullptr;
        for (std::string line; std::getline(in, line);) {
            if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
                current = &sections[line.substr(1, line.size() - 2)];
            } else if (current && !line.empty()) {
                size_t equals = line.find('=');
                (*current)[line.substr(0, equals)] = equals == std::string::npos ? std::string() : line.substr(equals + 1);
            }
        }
    }

    void install()
    {
        savegame::setExtraState(save, restore);
    }
}

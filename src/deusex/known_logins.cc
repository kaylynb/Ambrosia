#include "deusex/known_logins.h"

#include "core/text.h"
#include "deusex/game_state.h"
#include "core/package.h"

#include <map>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace dx::assets {
    std::span<const std::pair<std::string_view, std::span<const unsigned char>>> knownLogins();
}

namespace dx::knownlogins {
    namespace {
        struct Entry {
            std::string source;
            std::string account;
        };

        // The rows for each map and machine, both lower case.
        using Table = std::map<std::pair<std::string, std::string>, std::vector<Entry>>;

        // Only the rows that count: Likely ones, and others the review marked Y, less any
        // marked N.
        Table load()
        {
            Table table;
            for (const auto& [name, bytes] : assets::knownLogins()) {
                std::istringstream in(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                bool header = true;
                for (std::string line; std::getline(in, line);) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (line.empty() || line[0] == '#') {
                        continue;
                    }
                    if (header) {
                        header = false;
                        continue;
                    }
                    // source, source kind, map, machine, machine kind, account, confidence, verdict
                    std::vector<std::string> fields;
                    std::istringstream row(line);
                    for (std::string field; std::getline(row, field, '\t');) {
                        fields.push_back(field);
                    }
                    fields.resize(8);
                    const std::string& confidence = fields[6];
                    const std::string& verdict = fields[7];
                    if (verdict == "N" || (confidence != "Likely" && verdict != "Y")) {
                        continue;
                    }
                    table[{ lowered(fields[2]), lowered(fields[3]) }].push_back({ lowered(fields[0]), fields[5] });
                }
            }
            return table;
        }

        const Table& table()
        {
            static const Table loaded = load();
            return loaded;
        }

        // The sources read, as keys of the game state's [Read] section.
        constexpr const char* section = "Read";
    }

    void markRead(const std::string& source)
    {
        if (!source.empty() && source.back() != ':') {
            gamestate::add(section, lowered(source));
        }
    }

    std::vector<std::string> knownAccounts(const std::string& map, const std::string& machine)
    {
        std::vector<std::string> accounts;
        auto found = table().find({ lowered(map), lowered(machine) });
        if (found == table().end()) {
            return accounts;
        }
        for (const Entry& entry : found->second) {
            if (!gamestate::has(section, entry.source)) {
                continue;
            }
            bool seen = false;
            for (const std::string& account : accounts) {
                seen = seen || iequals(account, entry.account);
            }
            if (!seen) {
                accounts.push_back(entry.account);
            }
        }
        return accounts;
    }
}

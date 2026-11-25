#include "core/steam.h"

#include <fstream>
#include <sstream>
#include <string>

namespace dx::steam {
    namespace {
        // The quoted string at text[at] (at is the opening quote), with \\ and \" undone; at is
        // left after the closing quote. Empty and at the end when it is not closed.
        std::string quoted(std::string_view text, size_t& at)
        {
            std::string value;
            for (++at; at < text.size(); ++at) {
                char c = text[at];
                if (c == '"') {
                    ++at;
                    return value;
                }
                if (c == '\\' && at + 1 < text.size()) {
                    c = text[++at];
                }
                if (c == '\n') {
                    break;
                }
                value += c;
            }
            at = text.size();
            return {};
        }
    }

    std::filesystem::path gameFolder(const std::filesystem::path& library)
    {
        return library / "steamapps" / "common" / "Deus Ex";
    }

    std::vector<std::filesystem::path> libraryFolders(std::string_view vdf)
    {
        std::vector<std::filesystem::path> folders;
        size_t lineStart = 0;
        while (lineStart < vdf.size()) {
            size_t lineEnd = vdf.find('\n', lineStart);
            if (lineEnd == std::string_view::npos) {
                lineEnd = vdf.size();
            }
            std::string_view line = vdf.substr(lineStart, lineEnd - lineStart);
            size_t at = line.find('"');
            if (at != std::string_view::npos) {
                std::string key = quoted(line, at);
                size_t valueAt = line.find('"', at);
                if (key == "path" && valueAt != std::string_view::npos) {
                    std::string value = quoted(line, valueAt);
                    if (!value.empty()) {
                        folders.emplace_back(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
                    }
                }
            }
            lineStart = lineEnd + 1;
        }
        return folders;
    }

    std::vector<std::filesystem::path> libraries(const std::filesystem::path& steamRoot)
    {
        std::vector<std::filesystem::path> found { steamRoot };
        std::ifstream in(steamRoot / "steamapps" / "libraryfolders.vdf", std::ios::binary);
        if (in) {
            std::stringstream text;
            text << in.rdbuf();
            for (std::filesystem::path& folder : libraryFolders(text.str())) {
                found.push_back(std::move(folder));
            }
        }
        return found;
    }
}

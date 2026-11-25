#pragma once

#include <filesystem>
#include <istream>
#include <sstream>
#include <string>

namespace dx::editor {
    // Editor commands are plain text split on whitespace. A UI action that puts a file name in a
    // command wraps it with quoteArg; the command reads it back with readArg or restArg. Inside
    // quotes a doubled quote stands for one quote. Unquoted arguments, as typed in the Log panel or
    // given as a step of --do or --script, read as before.
    inline std::string quoteArg(const std::string& text)
    {
        std::string result = "\"";
        for (char c : text) {
            result += c;
            if (c == '"') {
                result += '"';
            }
        }
        return result + "\"";
    }

    inline std::string readArg(std::istream& input)
    {
        std::string result;
        input >> std::ws;
        if (input.peek() != '"') {
            input >> result;
            return result;
        }
        input.get();
        for (int c = input.get(); c != std::char_traits<char>::eof(); c = input.get()) {
            if (c == '"') {
                if (input.peek() != '"') {
                    return result;
                }
                input.get();
            }
            result += static_cast<char>(c);
        }
        input.clear(std::ios::eofbit);
        return result;
    }

    // The rest of a command taken as one argument: a quoted argument when it starts with a quote,
    // otherwise the text as given, spaces included.
    inline std::string restArg(const std::string& rest)
    {
        size_t first = rest.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return {};
        }
        if (rest[first] != '"') {
            return rest.substr(first);
        }
        std::istringstream input(rest.substr(first));
        return readArg(input);
    }

    // ImGui text is UTF-8; path::string() uses the ANSI code page on Windows and can throw.
    inline std::filesystem::path utf8Path(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    inline std::string utf8String(const std::filesystem::path& path)
    {
        std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }
}

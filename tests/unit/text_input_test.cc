#include "deusex/text_input.h"

#include <catch_amalgamated.hpp>

#include <string>

TEST_CASE("typed UTF-8 becomes one Windows-1252 byte per character", "[text]")
{
    CHECK(dx::textinput::fromUtf8("abc 123") == "abc 123");
    CHECK(dx::textinput::fromUtf8("\xc3\xa9") == "\xe9");
    CHECK(dx::textinput::fromUtf8("caf\xc3\xa9!") == "caf\xe9!");
    CHECK(dx::textinput::fromUtf8("\xc2\xa0\xc3\xbf") == "\xa0\xff");
    CHECK(dx::textinput::fromUtf8("\xe2\x82\xac") == "\x80");
    CHECK(dx::textinput::fromUtf8("\xe2\x80\x9chi\xe2\x80\x9d") == "\x93hi\x94");
    CHECK(dx::textinput::fromUtf8("\xc5\xb8") == "\x9f");
}

TEST_CASE("typed characters outside Windows-1252 are dropped", "[text]")
{
    // Greek, CJK, an emoji and a C1 control, which the game has no characters for.
    CHECK(dx::textinput::fromUtf8("a\xce\xb1z") == "az");
    CHECK(dx::textinput::fromUtf8("\xe4\xb8\xad") == "");
    CHECK(dx::textinput::fromUtf8("\xf0\x9f\x98\x80x") == "x");
    CHECK(dx::textinput::fromUtf8("\xc2\x81") == "");
}

TEST_CASE("malformed UTF-8 is dropped without losing the text around it", "[text]")
{
    // A stray continuation byte, a lead with nothing after it, and a lead cut short.
    CHECK(dx::textinput::fromUtf8("a\x80z") == "az");
    CHECK(dx::textinput::fromUtf8("a\xc3") == "a");
    CHECK(dx::textinput::fromUtf8("\xe2\x82z") == "z");
    // Overlong forms, a surrogate, past U+10FFFF, and bytes that never lead.
    CHECK(dx::textinput::fromUtf8("\xc0\xaf\xc1\xbf") == "");
    CHECK(dx::textinput::fromUtf8("\xe0\x80\xaf") == "");
    CHECK(dx::textinput::fromUtf8("\xed\xa0\x80") == "");
    CHECK(dx::textinput::fromUtf8("\xf4\x90\x80\x80") == "");
    CHECK(dx::textinput::fromUtf8("\xf5\xfe\xff") == "");
}

TEST_CASE("the game's characters go to a system keyboard as UTF-8 and come back unchanged", "[text]")
{
    CHECK(dx::textinput::toUtf8("abc 123") == "abc 123");
    CHECK(dx::textinput::toUtf8("caf\xe9") == "caf\xc3\xa9");
    CHECK(dx::textinput::toUtf8("\x80\x93hi\x94") == "\xe2\x82\xac\xe2\x80\x9chi\xe2\x80\x9d");
    // 0x81 has no character in Windows-1252.
    CHECK(dx::textinput::toUtf8("a\x81z") == "az");
    std::string every;
    for (int c = 1; c < 256; c++) {
        if (c != 0x81 && c != 0x8d && c != 0x8f && c != 0x90 && c != 0x9d) {
            every.push_back(static_cast<char>(c));
        }
    }
    CHECK(dx::textinput::fromUtf8(dx::textinput::toUtf8(every)) == every);
}

TEST_CASE("clipboard text pasted into the console becomes one line it can type", "[text]")
{
    CHECK(dx::textinput::consolePaste("summon Medkit") == "summon Medkit");
    // Line breaks, either kind, and tabs are spaces; a trailing one is trimmed.
    CHECK(dx::textinput::consolePaste("set a\r\nb\nc\td\r\n") == "set a b c d");
    CHECK(dx::textinput::consolePaste("a\r\rb") == "a  b");
    // The console's toggle keys and other control characters are dropped.
    CHECK(dx::textinput::consolePaste("say ~hi`\x01\x7f!") == "say hi!");
    // UTF-8 becomes the game's characters, as typing does.
    CHECK(dx::textinput::consolePaste("caf\xc3\xa9") == "caf\xe9");
    CHECK(dx::textinput::consolePaste("") == "");
}

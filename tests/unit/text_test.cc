#include "core/crc.h"
#include "core/name.h"
#include "core/text.h"

#include <catch_amalgamated.hpp>

#include <string>

TEST_CASE("lowered and iequals fold ASCII case only", "[text]")
{
    CHECK(dx::lowered("MiXeD 123_Case") == "mixed 123_case");
    CHECK(dx::lowered("") == "");
    // Bytes past ASCII, such as UTF-8's, are left as they are.
    CHECK(dx::lowered("Caf\xc3\x89") == "caf\xc3\x89");
    CHECK(dx::iequals("DeusEx.JCDentonMale", "deusex.jcdentonmale"));
    CHECK(dx::iequals("", ""));
    CHECK_FALSE(dx::iequals("abc", "abd"));
    CHECK_FALSE(dx::iequals("abc", "abcd"));
}

TEST_CASE("a name is one entry whatever its case, and keeps the case it was first given", "[text]")
{
    dx::Name first("TextTestMixedCase");
    dx::Name again("texttestmixedcase");
    CHECK(first == again);
    CHECK(first.str() == "TextTestMixedCase");
    CHECK(std::string(again.c_str()) == "TextTestMixedCase");
    CHECK_FALSE(first.none());
    CHECK(dx::Name::fromIndex(first.index()) == first);
}

TEST_CASE("an empty name is None, and finding or indexing past the table makes none", "[text]")
{
    CHECK(dx::Name("").none());
    CHECK(dx::Name().str() == "None");
    CHECK(dx::Name("NONE") == dx::Name());

    size_t before = dx::Name::count();
    CHECK(dx::Name::find("TextTestNeverMade").none());
    CHECK(dx::Name::count() == before);
    CHECK(dx::Name::fromIndex(static_cast<uint32_t>(before) + 100).none());

    dx::Name made("TextTestFound");
    CHECK(dx::Name::find("TEXTTESTFOUND") == made);
}

TEST_CASE("the CRC table is the CRC-32 polynomial worked most significant bit first", "[text]")
{
    CHECK(dx::crcTable[0] == 0u);
    CHECK(dx::crcTable[1] == 0x04C11DB7u);
    CHECK(dx::crcTable[2] == 0x09823B6Eu);
    CHECK(dx::crcTable[128] == 0x690CE0EEu);
    CHECK(dx::crcTable[255] == 0xB1F740B4u);
}

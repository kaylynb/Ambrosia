#include "core/package.h"

#include <catch_amalgamated.hpp>

#include <vector>

// The size and array index of a tagged property header, as every package reader decodes them.

namespace {
    size_t tagSize(std::vector<uint8_t> bytes, uint8_t info, size_t& used)
    {
        dx::Reader reader(bytes, 0);
        size_t size = dx::readTagSize(reader, info);
        used = reader.tell();
        return size;
    }

    int32_t tagIndex(std::vector<uint8_t> bytes, size_t& used)
    {
        dx::Reader reader(bytes, 0);
        int32_t index = dx::readTagArrayIndex(reader);
        used = reader.tell();
        return index;
    }
}

TEST_CASE("tag size codes 0 to 4 are fixed sizes and 5 to 7 read the size after the header")
{
    size_t used = 0;
    const size_t fixed[] = { 1, 2, 4, 12, 16 };
    for (uint8_t code = 0; code < 5; code++) {
        INFO(int(code));
        CHECK(tagSize({ 0xff, 0xff, 0xff, 0xff }, static_cast<uint8_t>(code << 4), used) == fixed[code]);
        CHECK(used == 0);
    }
    CHECK(tagSize({ 0xfe, 0x01 }, 0x50, used) == 254);
    CHECK(used == 1);
    // The array and bool bit and the type bits do not change the size.
    CHECK(tagSize({ 0xfe, 0x01 }, 0xd3, used) == 254);
    CHECK(used == 1);
    // A two-byte size is unsigned.
    CHECK(tagSize({ 0x34, 0x92, 0x00 }, 0x60, used) == 0x9234);
    CHECK(used == 2);
    CHECK(tagSize({ 0x78, 0x56, 0x34, 0x12, 0x00 }, 0x70, used) == 0x12345678);
    CHECK(used == 4);
}

TEST_CASE("tag array indices take one, two or four bytes by their top bits")
{
    size_t used = 0;
    CHECK(tagIndex({ 0x05, 0xff }, used) == 5);
    CHECK(used == 1);
    CHECK(tagIndex({ 0x7f, 0xff }, used) == 127);
    CHECK(used == 1);
    CHECK(tagIndex({ 0x81, 0x02, 0xff }, used) == 0x102);
    CHECK(used == 2);
    CHECK(tagIndex({ 0xbf, 0xff, 0xff }, used) == 0x3fff);
    CHECK(used == 2);
    CHECK(tagIndex({ 0xc1, 0x02, 0x03, 0x04, 0xff }, used) == 0x01020304);
    CHECK(used == 4);
    CHECK(tagIndex({ 0xff, 0xff, 0xff, 0xff }, used) == 0x3fffffff);
    CHECK(used == 4);
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>

#include <gtest/gtest.h>

#include "serialization/ByteArray.hpp"

TEST(ByteArrayTest, FixedWidthWireOrderAndSeek)
{
    sylar::ByteArray data;
    data.writeU8(0x12);
    data.writeU16(0x3456);
    data.writeU32(0x789abcde);
    data.writeU64(0x0123456789abcdefULL);
    ASSERT_EQ(data.size(), 15);
    const auto bytes = data.bytes();
    EXPECT_EQ(bytes[0], std::byte{0x12});
    EXPECT_EQ(bytes[1], std::byte{0x34});
    EXPECT_EQ(bytes[2], std::byte{0x56});
    data.seek(0);
    EXPECT_EQ(data.readU8(), 0x12);
    EXPECT_EQ(data.readU16(), 0x3456);
    EXPECT_EQ(data.readU32(), 0x789abcdeU);
    EXPECT_EQ(data.readU64(), 0x0123456789abcdefULL);
    EXPECT_THROW(data.readU8(), std::out_of_range);
    EXPECT_THROW(data.seek(16), std::out_of_range);
}

TEST(ByteArrayTest, VarintsAndStrings)
{
    sylar::ByteArray data;
    for (auto value :
         {std::uint64_t{0},
          std::uint64_t{127},
          std::uint64_t{128},
          std::numeric_limits<std::uint64_t>::max()})
    {
        data.writeVarUInt(value);
    }
    for (auto value :
         {std::int64_t{0},
          std::int64_t{-1},
          std::numeric_limits<std::int64_t>::min(),
          std::numeric_limits<std::int64_t>::max()})
    {
        data.writeVarInt(value);
    }
    data.writeString(std::string_view("hello\0world", 11));
    data.seek(0);
    EXPECT_EQ(data.readVarUInt(), 0);
    EXPECT_EQ(data.readVarUInt(), 127);
    EXPECT_EQ(data.readVarUInt(), 128);
    EXPECT_EQ(
        data.readVarUInt(),
        std::numeric_limits<std::uint64_t>::max()
    );
    EXPECT_EQ(data.readVarInt(), 0);
    EXPECT_EQ(data.readVarInt(), -1);
    EXPECT_EQ(
        data.readVarInt(),
        std::numeric_limits<std::int64_t>::min()
    );
    EXPECT_EQ(
        data.readVarInt(),
        std::numeric_limits<std::int64_t>::max()
    );
    EXPECT_EQ(data.readString(), std::string("hello\0world", 11));
}

TEST(ByteArrayTest, RejectsMalformedLengthsAndVarints)
{
    sylar::ByteArray malformed;
    for (int i = 0; i < 10; ++i)
    {
        malformed.writeU8(0x80);
    }
    malformed.seek(0);
    EXPECT_THROW(malformed.readVarUInt(), std::invalid_argument);
    sylar::ByteArray text;
    text.writeVarUInt(100);
    text.seek(0);
    EXPECT_THROW(text.readString(), std::out_of_range);
}

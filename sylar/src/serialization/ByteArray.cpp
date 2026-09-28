#include "serialization/ByteArray.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace sylar
{

ByteArray::ByteArray(std::size_t reserve)
{
    m_data.reserve(reserve);
}

std::size_t ByteArray::size() const noexcept
{
    return m_data.size();
}

std::size_t ByteArray::position() const noexcept
{
    return m_position;
}

std::size_t ByteArray::remaining() const noexcept
{
    return m_data.size() - m_position;
}

void ByteArray::seek(std::size_t position)
{
    if (position > m_data.size())
    {
        throw std::out_of_range("ByteArray seek past end");
    }
    m_position = position;
}

void ByteArray::clear() noexcept
{
    m_data.clear();
    m_position = 0;
}

std::span<const std::byte> ByteArray::bytes() const noexcept
{
    return m_data;
}

void ByteArray::write(std::span<const std::byte> input)
{
    if (input.size() >
        std::numeric_limits<std::size_t>::max() - m_position)
    {
        throw std::length_error("ByteArray size overflow");
    }
    const auto end = m_position + input.size();
    if (end > m_data.size())
    {
        m_data.resize(end);
    }
    std::copy(
        input.begin(),
        input.end(),
        m_data.begin() + m_position
    );
    m_position = end;
}

void ByteArray::read(std::span<std::byte> output)
{
    if (output.size() > remaining())
    {
        throw std::out_of_range("ByteArray read past end");
    }
    std::copy_n(
        m_data.begin() + m_position,
        output.size(),
        output.begin()
    );
    m_position += output.size();
}

template <typename T> void ByteArray::writeFixed(T value)
{
    static_assert(std::is_unsigned_v<T>);
    if constexpr (std::endian::native == std::endian::little)
    {
        value = std::byteswap(value);
    }
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    write(std::span(first, sizeof(value)));
}

template <typename T> T ByteArray::readFixed()
{
    static_assert(std::is_unsigned_v<T>);
    T value{};
    auto* first = reinterpret_cast<std::byte*>(&value);
    read(std::span(first, sizeof(value)));
    if constexpr (std::endian::native == std::endian::little)
    {
        value = std::byteswap(value);
    }
    return value;
}

void ByteArray::writeU8(std::uint8_t value)
{
    writeFixed(value);
}

void ByteArray::writeU16(std::uint16_t value)
{
    writeFixed(value);
}

void ByteArray::writeU32(std::uint32_t value)
{
    writeFixed(value);
}

void ByteArray::writeU64(std::uint64_t value)
{
    writeFixed(value);
}

std::uint8_t ByteArray::readU8()
{
    return readFixed<std::uint8_t>();
}

std::uint16_t ByteArray::readU16()
{
    return readFixed<std::uint16_t>();
}

std::uint32_t ByteArray::readU32()
{
    return readFixed<std::uint32_t>();
}

std::uint64_t ByteArray::readU64()
{
    return readFixed<std::uint64_t>();
}

void ByteArray::writeVarUInt(std::uint64_t value)
{
    while (value >= 0x80)
    {
        writeU8(static_cast<std::uint8_t>(value) | 0x80);
        value >>= 7;
    }
    writeU8(static_cast<std::uint8_t>(value));
}

std::uint64_t ByteArray::readVarUInt()
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 10; ++index)
    {
        const auto byte = readU8();
        if (index == 9 && byte > 1)
        {
            throw std::invalid_argument("varint overflow");
        }
        value |= std::uint64_t(byte & 0x7f) << (7 * index);
        if ((byte & 0x80) == 0)
        {
            return value;
        }
    }
    throw std::invalid_argument("unterminated varint");
}

void ByteArray::writeVarInt(std::int64_t value)
{
    const auto sign = value < 0 ? ~std::uint64_t{0} : 0;
    writeVarUInt((static_cast<std::uint64_t>(value) << 1) ^ sign);
}

std::int64_t ByteArray::readVarInt()
{
    const auto value = readVarUInt();
    return std::bit_cast<std::int64_t>(
        (value >> 1) ^ (0 - (value & 1))
    );
}

void ByteArray::writeString(std::string_view value)
{
    writeVarUInt(value.size());
    const auto* data = reinterpret_cast<const std::byte*>(value.data()
    );
    write(std::span(data, value.size()));
}

std::string ByteArray::readString(std::size_t max_length)
{
    const auto length = readVarUInt();
    if (length > max_length || length > remaining())
    {
        throw std::out_of_range("invalid string length");
    }
    std::string result(length, '\0');
    if (length != 0)
    {
        auto* data = reinterpret_cast<std::byte*>(result.data());
        read(std::span(data, length));
    }
    return result;
}

void ByteArray::save(const std::filesystem::path& path) const
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("cannot open ByteArray output");
    }
    stream.write(
        reinterpret_cast<const char*>(m_data.data()),
        static_cast<std::streamsize>(m_data.size())
    );
    if (!stream)
    {
        throw std::runtime_error("cannot write ByteArray output");
    }
}

ByteArray ByteArray::load(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error("cannot open ByteArray input");
    }
    const std::vector<char> content(
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}
    );
    if (!stream.eof() && !stream)
    {
        throw std::runtime_error("cannot read ByteArray input");
    }
    ByteArray result(content.size());
    result.m_data.resize(content.size());
    std::memcpy(result.m_data.data(), content.data(), content.size());
    return result;
}

} // namespace sylar

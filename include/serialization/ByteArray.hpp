#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sylar
{

class ByteArray
{
public:
    explicit ByteArray(std::size_t reserve = 0);

    std::size_t size() const noexcept;
    std::size_t position() const noexcept;
    std::size_t remaining() const noexcept;
    void seek(std::size_t position);
    void clear() noexcept;
    std::span<const std::byte> bytes() const noexcept;

    void write(std::span<const std::byte> input);
    void read(std::span<std::byte> output);

    void writeU8(std::uint8_t value);
    void writeU16(std::uint16_t value);
    void writeU32(std::uint32_t value);
    void writeU64(std::uint64_t value);
    std::uint8_t readU8();
    std::uint16_t readU16();
    std::uint32_t readU32();
    std::uint64_t readU64();

    void writeVarUInt(std::uint64_t value);
    std::uint64_t readVarUInt();
    void writeVarInt(std::int64_t value);
    std::int64_t readVarInt();

    void writeString(std::string_view value);
    std::string readString(std::size_t max_length = 16 * 1024 * 1024);

    void save(const std::filesystem::path& path) const;
    static ByteArray load(const std::filesystem::path& path);

private:
    template <typename T> void writeFixed(T value);

    template <typename T> T readFixed();

    std::vector<std::byte> m_data;
    std::size_t m_position = 0;
};

} // namespace sylar

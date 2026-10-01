#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Little-endian byte encoding for the native file format
// (docs/NATIVE_FILE_FORMAT_RESEARCH.md §3): fixed-width integers, LEB128
// varints, zig-zag signed varints, and IEEE CRC-32.

namespace le::persistence
{
    static_assert(std::endian::native == std::endian::little, "the native format's fixed-width fields assume a little-endian host");

    /// @brief A malformed, truncated or corrupt file. Thrown inside the
    /// persistence library only - save_native/load_native catch it and
    /// return its message, so nothing outside ever sees an exception.
    struct FormatError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    inline uint64_t zigzag_encode(int64_t value) { return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63); }
    inline int64_t zigzag_decode(uint64_t value) { return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1); }

    class ByteWriter
    {
    public:
        std::vector<uint8_t> &bytes() { return bytes_; }
        const std::vector<uint8_t> &bytes() const { return bytes_; }
        size_t size() const { return bytes_.size(); }

        void u8(uint8_t value) { bytes_.push_back(value); }

        template <class T>
        void fixed(T value)
        {
            const size_t at = bytes_.size();
            bytes_.resize(at + sizeof(T));
            std::memcpy(bytes_.data() + at, &value, sizeof(T));
        }

        void varint(uint64_t value)
        {
            while (value >= 0x80)
            {
                bytes_.push_back(static_cast<uint8_t>(value | 0x80));
                value >>= 7;
            }
            bytes_.push_back(static_cast<uint8_t>(value));
        }

        void svarint(int64_t value) { varint(zigzag_encode(value)); }

        void raw(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            bytes_.insert(bytes_.end(), p, p + size);
        }

        /// @brief varint length + bytes.
        void string(std::string_view value)
        {
            varint(value.size());
            raw(value.data(), value.size());
        }

    private:
        std::vector<uint8_t> bytes_;
    };

    class ByteReader
    {
    public:
        ByteReader(const uint8_t *data, size_t size, std::string_view what) : data_(data), size_(size), what_(what) {}

        size_t position() const { return pos_; }
        size_t remaining() const { return size_ - pos_; }
        bool at_end() const { return pos_ == size_; }

        uint8_t u8()
        {
            need(1);
            return data_[pos_++];
        }

        template <class T>
        T fixed()
        {
            need(sizeof(T));
            T value;
            std::memcpy(&value, data_ + pos_, sizeof(T));
            pos_ += sizeof(T);
            return value;
        }

        uint64_t varint()
        {
            uint64_t value = 0;
            for (int shift = 0; shift < 64; shift += 7)
            {
                const uint8_t byte = u8();
                value |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if (!(byte & 0x80))
                    return value;
            }
            fail("varint longer than 64 bits");
        }

        int64_t svarint() { return zigzag_decode(varint()); }

        const uint8_t *raw(size_t size)
        {
            need(size);
            const uint8_t *p = data_ + pos_;
            pos_ += size;
            return p;
        }

        std::string_view string()
        {
            const uint64_t size = varint();
            if (size > remaining())
                fail("string length past the end");
            return {reinterpret_cast<const char *>(raw(size)), size};
        }

        /// @brief The number of elements a varint count claims, checked
        /// against the bytes left (each element takes at least
        /// `min_element_bytes`), so a corrupt count can't trigger a huge
        /// allocation.
        uint64_t count(size_t min_element_bytes = 1)
        {
            const uint64_t n = varint();
            if (min_element_bytes && n > remaining() / min_element_bytes)
                fail("element count larger than the remaining data");
            return n;
        }

        [[noreturn]] void fail(std::string_view message) const
        {
            throw FormatError(std::string(what_) + ": " + std::string(message) + " (at byte " + std::to_string(pos_) + ")");
        }

    private:
        void need(size_t n) const
        {
            if (n > size_ - pos_)
                fail("unexpected end of data");
        }

        const uint8_t *data_;
        size_t size_;
        size_t pos_ = 0;
        std::string_view what_;
    };

    /// @brief IEEE 802.3 CRC-32 (the zlib/PNG polynomial).
    uint32_t crc32(const uint8_t *data, size_t size);
}

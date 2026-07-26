#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Explicit little endian on the wire. Both ends are x86-64 today, but pinning it
// down means a future arm build can't silently disagree.
namespace wire
{
class Writer
{
public:
    explicit Writer(std::vector<uint8_t>& aBuffer)
        : m_buffer(aBuffer)
    {
    }

    void U8(uint8_t aValue)
    {
        m_buffer.push_back(aValue);
    }

    void U16(uint16_t aValue)
    {
        U8(static_cast<uint8_t>(aValue));
        U8(static_cast<uint8_t>(aValue >> 8));
    }

    void U32(uint32_t aValue)
    {
        U16(static_cast<uint16_t>(aValue));
        U16(static_cast<uint16_t>(aValue >> 16));
    }

    void U64(uint64_t aValue)
    {
        U32(static_cast<uint32_t>(aValue));
        U32(static_cast<uint32_t>(aValue >> 32));
    }

    void F32(float aValue)
    {
        uint32_t bits;
        std::memcpy(&bits, &aValue, sizeof(bits));
        U32(bits);
    }

    // Length-prefixed, so a truncated payload can't run into the next field.
    void Str(std::string_view aValue)
    {
        U16(static_cast<uint16_t>(aValue.size()));
        m_buffer.insert(m_buffer.end(), aValue.begin(), aValue.end());
    }

    void Bytes(std::span<const uint8_t> aValue)
    {
        m_buffer.insert(m_buffer.end(), aValue.begin(), aValue.end());
    }

    size_t Size() const
    {
        return m_buffer.size();
    }

private:
    std::vector<uint8_t>& m_buffer;
};

// Failure is sticky: once a read runs out of bytes every later read fails too,
// so a caller that forgets one check still can't act on garbage.
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> aData)
        : m_data(aData)
    {
    }

    bool U8(uint8_t& aOut)
    {
        if (!Available(1))
        {
            return false;
        }

        aOut = m_data[m_offset++];
        return true;
    }

    bool U16(uint16_t& aOut)
    {
        uint8_t low, high;
        if (!U8(low) || !U8(high))
        {
            return false;
        }

        aOut = static_cast<uint16_t>(low | (high << 8));
        return true;
    }

    bool U32(uint32_t& aOut)
    {
        uint16_t low, high;
        if (!U16(low) || !U16(high))
        {
            return false;
        }

        aOut = static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 16);
        return true;
    }

    bool U64(uint64_t& aOut)
    {
        uint32_t low, high;
        if (!U32(low) || !U32(high))
        {
            return false;
        }

        aOut = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
        return true;
    }

    bool F32(float& aOut)
    {
        uint32_t bits;
        if (!U32(bits))
        {
            return false;
        }

        std::memcpy(&aOut, &bits, sizeof(aOut));
        return true;
    }

    bool Str(std::string& aOut, uint16_t aMaxSize)
    {
        uint16_t size;
        if (!U16(size))
        {
            return false;
        }

        // Bound before allocating: the length prefix comes from the network.
        if (size > aMaxSize || !Available(size))
        {
            Fail();
            return false;
        }

        aOut.assign(reinterpret_cast<const char*>(m_data.data() + m_offset), size);
        m_offset += size;

        return true;
    }

    std::span<const uint8_t> Rest() const
    {
        return m_failed ? std::span<const uint8_t>{} : m_data.subspan(m_offset);
    }

    size_t Remaining() const
    {
        return m_failed ? 0 : m_data.size() - m_offset;
    }

    bool Ok() const
    {
        return !m_failed;
    }

private:
    bool Available(size_t aCount)
    {
        if (m_failed || m_data.size() - m_offset < aCount)
        {
            Fail();
            return false;
        }

        return true;
    }

    void Fail()
    {
        m_failed = true;
    }

    std::span<const uint8_t> m_data;
    size_t m_offset{};
    bool m_failed{};
};
} // namespace wire

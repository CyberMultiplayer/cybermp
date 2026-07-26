#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Serialize.hpp"

namespace proto
{
// Bump on any wire change. The handshake refuses a mismatch outright rather than
// letting two versions talk and misbehave in ways that look like gameplay bugs.
constexpr uint32_t kVersion = 1;

// Keeps a datagram under the usual MTU, so nothing gets fragmented.
constexpr size_t kMaxDatagram = 1200;
constexpr uint16_t kMaxStringSize = 256;

enum class Type : uint8_t
{
    Invalid = 0,
    Hello = 1,
    HelloAck = 2,
    Ping = 3,
    Pong = 4,
};

enum Flags : uint8_t
{
    None = 0,
    Reliable = 1 << 0,
};

// 4 bytes, and small on purpose: it rides along every single message.
// The size field excludes the header, which lets several messages share a datagram later.
struct Header
{
    static constexpr size_t kSize = 4;

    Type type{Type::Invalid};
    uint8_t flags{Flags::None};
    uint16_t size{};

    bool IsReliable() const
    {
        return (flags & Flags::Reliable) != 0;
    }

    void Write(wire::Writer& aWriter) const
    {
        aWriter.U8(static_cast<uint8_t>(type));
        aWriter.U8(flags);
        aWriter.U16(size);
    }

    bool Read(wire::Reader& aReader)
    {
        uint8_t rawType;
        if (!aReader.U8(rawType) || !aReader.U8(flags) || !aReader.U16(size))
        {
            return false;
        }

        // An unknown type from the network is data, not a bug: reject, don't assume.
        if (rawType > static_cast<uint8_t>(Type::Pong))
        {
            return false;
        }

        type = static_cast<Type>(rawType);
        return type != Type::Invalid;
    }
};

struct Hello
{
    static constexpr Type kType = Type::Hello;
    static constexpr uint8_t kFlags = Flags::Reliable;

    uint32_t version{kVersion};
    std::string username;

    void Write(wire::Writer& aWriter) const
    {
        aWriter.U32(version);
        aWriter.Str(username);
    }

    bool Read(wire::Reader& aReader)
    {
        return aReader.U32(version) && aReader.Str(username, kMaxStringSize);
    }
};

struct HelloAck
{
    static constexpr Type kType = Type::HelloAck;
    static constexpr uint8_t kFlags = Flags::Reliable;

    bool accepted{};
    uint32_t version{kVersion};
    std::string reason;

    void Write(wire::Writer& aWriter) const
    {
        aWriter.U8(accepted ? 1 : 0);
        aWriter.U32(version);
        aWriter.Str(reason);
    }

    bool Read(wire::Reader& aReader)
    {
        uint8_t raw;
        if (!aReader.U8(raw) || !aReader.U32(version) || !aReader.Str(reason, kMaxStringSize))
        {
            return false;
        }

        accepted = raw != 0;
        return true;
    }
};

// Unreliable on purpose: a lost ping is answered by the next one, and holding
// the channel for retransmits would only inflate the measured rtt.
struct Ping
{
    static constexpr Type kType = Type::Ping;
    static constexpr uint8_t kFlags = Flags::None;

    uint64_t sentAt{};

    void Write(wire::Writer& aWriter) const
    {
        aWriter.U64(sentAt);
    }

    bool Read(wire::Reader& aReader)
    {
        return aReader.U64(sentAt);
    }
};

struct Pong
{
    static constexpr Type kType = Type::Pong;
    static constexpr uint8_t kFlags = Flags::None;

    uint64_t sentAt{};

    void Write(wire::Writer& aWriter) const
    {
        aWriter.U64(sentAt);
    }

    bool Read(wire::Reader& aReader)
    {
        return aReader.U64(sentAt);
    }
};

// Header size is filled in after the body, since we only know it then.
template<typename T>
bool Encode(const T& aMessage, std::vector<uint8_t>& aOut)
{
    aOut.clear();

    wire::Writer writer(aOut);
    Header{T::kType, T::kFlags, 0}.Write(writer);
    aMessage.Write(writer);

    const auto bodySize = aOut.size() - Header::kSize;
    if (aOut.size() > kMaxDatagram || bodySize > UINT16_MAX)
    {
        aOut.clear();
        return false;
    }

    aOut[2] = static_cast<uint8_t>(bodySize);
    aOut[3] = static_cast<uint8_t>(bodySize >> 8);

    return true;
}

inline bool PeekHeader(std::span<const uint8_t> aData, Header& aOut)
{
    wire::Reader reader(aData);
    return aOut.Read(reader);
}

// Trusts nothing: type must match, the declared size must match what actually arrived.
template<typename T>
bool Decode(std::span<const uint8_t> aData, T& aOut)
{
    wire::Reader reader(aData);

    Header header;
    if (!header.Read(reader) || header.type != T::kType)
    {
        return false;
    }

    if (header.size != reader.Remaining())
    {
        return false;
    }

    return aOut.Read(reader) && reader.Ok();
}
} // namespace proto

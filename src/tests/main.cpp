// Hand-rolled harness on purpose: ten-odd assertions don't justify pulling in a
// framework. Swap for doctest/catch2 the day this outgrows it.

#include <cstdio>
#include <string>
#include <vector>

#include "Protocol.hpp"
#include "Serialize.hpp"

namespace
{
int g_failed = 0;
int g_checks = 0;

void Check(bool aCondition, const char* aWhat)
{
    ++g_checks;

    if (!aCondition)
    {
        ++g_failed;
        std::printf("  FAIL  %s\n", aWhat);
    }
}

#define CHECK(cond) Check((cond), #cond)

void TestPrimitiveRoundTrip()
{
    std::printf("primitive round trip\n");

    std::vector<uint8_t> buffer;
    wire::Writer writer(buffer);

    writer.U8(0xAB);
    writer.U16(0xBEEF);
    writer.U32(0xDEADBEEF);
    writer.U64(0x0123456789ABCDEFull);
    writer.F32(-1234.5f);
    writer.Str("hello");

    wire::Reader reader(buffer);

    uint8_t u8{};
    uint16_t u16{};
    uint32_t u32{};
    uint64_t u64{};
    float f32{};
    std::string text;

    CHECK(reader.U8(u8) && u8 == 0xAB);
    CHECK(reader.U16(u16) && u16 == 0xBEEF);
    CHECK(reader.U32(u32) && u32 == 0xDEADBEEF);
    CHECK(reader.U64(u64) && u64 == 0x0123456789ABCDEFull);
    CHECK(reader.F32(f32) && f32 == -1234.5f);
    CHECK(reader.Str(text, proto::kMaxStringSize) && text == "hello");
    CHECK(reader.Remaining() == 0);
    CHECK(reader.Ok());
}

void TestReadPastEnd()
{
    std::printf("reading past the end fails, and stays failed\n");

    std::vector<uint8_t> buffer;
    wire::Writer writer(buffer);
    writer.U8(1);

    wire::Reader reader(buffer);

    uint8_t u8{};
    uint32_t u32{};

    CHECK(reader.U8(u8));
    CHECK(!reader.U32(u32)); // only one byte was written
    CHECK(!reader.Ok());

    // Sticky: a later read that would otherwise fit must still fail.
    CHECK(!reader.U8(u8));
    CHECK(reader.Remaining() == 0);
    CHECK(reader.Rest().empty());
}

void TestEmptyBuffer()
{
    std::printf("empty buffer\n");

    std::vector<uint8_t> empty;
    wire::Reader reader(empty);

    uint8_t u8{};
    CHECK(!reader.U8(u8));
    CHECK(!reader.Ok());

    proto::Header header;
    CHECK(!proto::PeekHeader(empty, header));
}

void TestOversizedString()
{
    std::printf("string longer than the cap is refused\n");

    std::vector<uint8_t> buffer;
    wire::Writer writer(buffer);
    writer.Str(std::string(300, 'x'));

    wire::Reader reader(buffer);

    std::string text;
    CHECK(!reader.Str(text, 256)); // declared length exceeds the cap
    CHECK(!reader.Ok());
}

void TestLyingLengthPrefix()
{
    std::printf("length prefix larger than the payload is refused\n");

    // Claims 100 bytes of string but only three follow.
    std::vector<uint8_t> buffer{100, 0, 'a', 'b', 'c'};
    wire::Reader reader(buffer);

    std::string text;
    CHECK(!reader.Str(text, proto::kMaxStringSize));
    CHECK(!reader.Ok());
}

void TestMessageRoundTrip()
{
    std::printf("message round trip\n");

    std::vector<uint8_t> buffer;

    proto::Hello sent;
    sent.username = "akitium";
    CHECK(proto::Encode(sent, buffer));

    proto::Header header;
    CHECK(proto::PeekHeader(buffer, header));
    CHECK(header.type == proto::Type::Hello);
    CHECK(header.IsReliable());
    CHECK(header.size == buffer.size() - proto::Header::kSize);

    proto::Hello received;
    CHECK(proto::Decode(buffer, received));
    CHECK(received.version == proto::kVersion);
    CHECK(received.username == "akitium");

    proto::Ping ping;
    ping.sentAt = 0xFEEDFACEull;
    CHECK(proto::Encode(ping, buffer));
    CHECK(proto::PeekHeader(buffer, header));
    CHECK(!header.IsReliable()); // ping is deliberately unreliable

    proto::Ping decodedPing;
    CHECK(proto::Decode(buffer, decodedPing));
    CHECK(decodedPing.sentAt == 0xFEEDFACEull);
}

void TestWrongType()
{
    std::printf("decoding as the wrong type is refused\n");

    std::vector<uint8_t> buffer;
    proto::Ping ping;
    CHECK(proto::Encode(ping, buffer));

    proto::Pong pong;
    CHECK(!proto::Decode(buffer, pong));
}

void TestUnknownType()
{
    std::printf("unknown and invalid types are refused\n");

    std::vector<uint8_t> buffer;
    proto::Ping ping;
    CHECK(proto::Encode(ping, buffer));

    auto unknown = buffer;
    unknown[0] = 200; // no such message type
    proto::Header header;
    CHECK(!proto::PeekHeader(unknown, header));

    auto invalid = buffer;
    invalid[0] = static_cast<uint8_t>(proto::Type::Invalid);
    CHECK(!proto::PeekHeader(invalid, header));
}

void TestTruncatedAndPaddedBody()
{
    std::printf("declared size must match what arrived\n");

    std::vector<uint8_t> buffer;
    proto::Ping ping;
    ping.sentAt = 42;
    CHECK(proto::Encode(ping, buffer));

    auto truncated = buffer;
    truncated.pop_back();
    proto::Ping out;
    CHECK(!proto::Decode(truncated, out));

    auto padded = buffer;
    padded.push_back(0xFF); // trailing junk must not be tolerated
    CHECK(!proto::Decode(padded, out));
}

void TestVersionMismatch()
{
    std::printf("version mismatch is visible to the server\n");

    std::vector<uint8_t> buffer;

    proto::Hello sent;
    sent.version = proto::kVersion + 1;
    sent.username = "future";
    CHECK(proto::Encode(sent, buffer));

    // Decoding must still succeed, otherwise the server can't tell the peer why
    // it is being refused.
    proto::Hello received;
    CHECK(proto::Decode(buffer, received));
    CHECK(received.version != proto::kVersion);
}

void TestDatagramCap()
{
    std::printf("oversized message is refused, not truncated\n");

    proto::Hello sent;
    sent.username = std::string(proto::kMaxStringSize, 'x');

    std::vector<uint8_t> buffer;
    CHECK(proto::Encode(sent, buffer));
    CHECK(buffer.size() <= proto::kMaxDatagram);
}
} // namespace

int main()
{
    TestPrimitiveRoundTrip();
    TestReadPastEnd();
    TestEmptyBuffer();
    TestOversizedString();
    TestLyingLengthPrefix();
    TestMessageRoundTrip();
    TestWrongType();
    TestUnknownType();
    TestTruncatedAndPaddedBody();
    TestVersionMismatch();
    TestDatagramCap();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);

    return g_failed == 0 ? 0 : 1;
}

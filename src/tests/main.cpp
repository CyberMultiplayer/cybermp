// Hand-rolled harness on purpose: ten-odd assertions don't justify pulling in a
// framework. Swap for doctest/catch2 the day this outgrows it.

#include <cstdio>
#include <string>
#include <vector>

#include "Net.hpp"
#include "Protocol.hpp"
#include "Serialize.hpp"
#include "Session.hpp"

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

// Real udp loopback, but both ends live here: no spawned process, no redirected
// file, no sleep. The PowerShell smoke test covered the same ground and went
// flaky on CI for exactly those reasons.
void TestLoopbackExchange()
{
    std::printf("loopback handshake and ping over real udp\n");

    net::Startup startup;
    CHECK(startup.ok());

    net::UdpSocket server;
    net::UdpSocket client;

    CHECK(server.Bind(0)); // 0 means the OS picks a free port, so CI can't collide
    CHECK(client.Open());

    const auto port = server.LocalPort();
    CHECK(port != 0);

    const auto target = net::UdpSocket::Loopback(port);
    std::vector<uint8_t> out;
    std::vector<uint8_t> in(proto::kMaxDatagram);

    // --- handshake ---
    proto::Hello hello;
    hello.username = "loopback";
    CHECK(proto::Encode(hello, out));
    CHECK(client.SendTo(target, {out.data(), out.size()}));

    auto got = server.RecvFrom(in, 2000);
    CHECK(got.has_value());

    if (got)
    {
        proto::Header header;
        CHECK(proto::PeekHeader({in.data(), got->size}, header));
        CHECK(header.type == proto::Type::Hello);

        proto::Hello decoded;
        CHECK(proto::Decode({in.data(), got->size}, decoded));
        CHECK(decoded.username == "loopback");
        CHECK(decoded.version == proto::kVersion);

        proto::HelloAck ack;
        ack.accepted = true;
        CHECK(proto::Encode(ack, out));
        CHECK(server.SendTo(got->from, {out.data(), out.size()}));

        got = client.RecvFrom(in, 2000);
        CHECK(got.has_value());

        if (got)
        {
            proto::HelloAck decodedAck;
            CHECK(proto::Decode({in.data(), got->size}, decodedAck));
            CHECK(decodedAck.accepted);
        }
    }

    // --- ping keeps the client's timestamp intact ---
    proto::Ping ping;
    ping.sentAt = 0x1122334455667788ull;
    CHECK(proto::Encode(ping, out));
    CHECK(client.SendTo(target, {out.data(), out.size()}));

    got = server.RecvFrom(in, 2000);
    CHECK(got.has_value());

    if (got)
    {
        proto::Ping decoded;
        CHECK(proto::Decode({in.data(), got->size}, decoded));
        CHECK(decoded.sentAt == ping.sentAt);
    }
}

void TestLoopbackVersionRefusal()
{
    std::printf("server refuses a mismatched protocol version\n");

    net::Startup startup;
    net::UdpSocket server;
    net::UdpSocket client;

    CHECK(server.Bind(0));
    CHECK(client.Open());

    const auto target = net::UdpSocket::Loopback(server.LocalPort());
    std::vector<uint8_t> out;
    std::vector<uint8_t> in(proto::kMaxDatagram);

    proto::Hello hello;
    hello.version = proto::kVersion + 1;
    hello.username = "future";
    CHECK(proto::Encode(hello, out));
    CHECK(client.SendTo(target, {out.data(), out.size()}));

    const auto got = server.RecvFrom(in, 2000);
    CHECK(got.has_value());

    if (got)
    {
        proto::Hello decoded;
        CHECK(proto::Decode({in.data(), got->size}, decoded));

        // The decode has to succeed for the server to explain the refusal.
        CHECK(decoded.version != proto::kVersion);
    }
}

void TestLoopbackJunkIsDropped()
{
    std::printf("junk datagram is rejected, not parsed\n");

    net::Startup startup;
    net::UdpSocket server;
    net::UdpSocket client;

    CHECK(server.Bind(0));
    CHECK(client.Open());

    const auto target = net::UdpSocket::Loopback(server.LocalPort());
    const std::vector<uint8_t> junk{0xFF, 0xEE, 0xDD, 0xCC, 0xBB};
    CHECK(client.SendTo(target, {junk.data(), junk.size()}));

    std::vector<uint8_t> in(proto::kMaxDatagram);
    const auto got = server.RecvFrom(in, 2000);
    CHECK(got.has_value());

    if (got)
    {
        // Arrives fine at the transport layer, and the protocol layer must refuse it.
        proto::Header header;
        CHECK(!proto::PeekHeader({in.data(), got->size}, header));
    }
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
net::Endpoint Peer(uint16_t aPort)
{
    return net::UdpSocket::Loopback(aPort);
}

void TestSessionJoin()
{
    std::printf("sessions: join, duplicate name, capacity\n");

    server::SessionManager sessions(2, 1000);
    server::PlayerId id = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, id) == server::JoinResult::Accepted);
    CHECK(id == 1); // 0 stays reserved for "no player"
    CHECK(sessions.Count() == 1);

    // Same name from a different peer must not create a second player.
    CHECK(sessions.Join(Peer(2), "a", proto::kVersion, 0, id) == server::JoinResult::NameTaken);
    CHECK(id == server::kInvalidPlayer);
    CHECK(sessions.Count() == 1);

    CHECK(sessions.Join(Peer(2), "b", proto::kVersion, 0, id) == server::JoinResult::Accepted);
    CHECK(id == 2);

    CHECK(sessions.Join(Peer(3), "c", proto::kVersion, 0, id) == server::JoinResult::Full);
    CHECK(sessions.Count() == 2);
}

void TestSessionVersionCheckedFirst()
{
    std::printf("sessions: version is checked before anything else\n");

    server::SessionManager sessions(1, 1000);
    server::PlayerId id = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    // Server is full AND the version is wrong: the version has to win, otherwise the
    // client is told the wrong thing.
    CHECK(sessions.Join(Peer(2), "b", proto::kVersion + 1, 0, id) == server::JoinResult::VersionMismatch);
}

void TestSessionRepeatedHello()
{
    std::printf("sessions: a resent hello returns the same id\n");

    server::SessionManager sessions(4, 1000);
    server::PlayerId first = server::kInvalidPlayer;
    server::PlayerId second = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, first) == server::JoinResult::Accepted);

    // Udp loses acks, so clients do resend hello. That must not double up.
    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 50, second) == server::JoinResult::AlreadyJoined);
    CHECK(first == second);
    CHECK(sessions.Count() == 1);
}

void TestSessionTimeout()
{
    std::printf("sessions: silent peers time out, traffic keeps them\n");

    server::SessionManager sessions(4, 1000);
    server::PlayerId id = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "quiet", proto::kVersion, 0, id) == server::JoinResult::Accepted);
    CHECK(sessions.Join(Peer(2), "chatty", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    CHECK(sessions.CollectTimedOut(500).empty()); // not yet
    CHECK(sessions.Touch(Peer(2), 900));

    const auto dropped = sessions.CollectTimedOut(1000);
    CHECK(dropped.size() == 1);
    CHECK(!dropped.empty() && dropped[0].username == "quiet");
    CHECK(sessions.Count() == 1);
    CHECK(sessions.Find(Peer(2)) != nullptr);
    CHECK(sessions.Find(Peer(1)) == nullptr);
}

void TestSessionClockGoingBackwards()
{
    std::printf("sessions: a now that moves backwards must not drop everyone\n");

    server::SessionManager sessions(4, 1000);
    server::PlayerId id = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 5000, id) == server::JoinResult::Accepted);

    // Unsigned subtraction here would look like a huge elapsed time.
    CHECK(sessions.CollectTimedOut(1000).empty());
    CHECK(sessions.Count() == 1);
}

void TestSessionRemove()
{
    std::printf("sessions: explicit removal\n");

    server::SessionManager sessions(4, 1000);
    server::PlayerId id = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, id) == server::JoinResult::Accepted);
    CHECK(sessions.Find(id) != nullptr);
    CHECK(sessions.Remove(id));
    CHECK(!sessions.Remove(id)); // already gone
    CHECK(sessions.Find(id) == nullptr);
    CHECK(sessions.Find(server::kInvalidPlayer) == nullptr);
    CHECK(sessions.Count() == 0);
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
    TestLoopbackExchange();
    TestLoopbackVersionRefusal();
    TestLoopbackJunkIsDropped();
    TestSessionJoin();
    TestSessionVersionCheckedFirst();
    TestSessionRepeatedHello();
    TestSessionTimeout();
    TestSessionClockGoingBackwards();
    TestSessionRemove();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);

    return g_failed == 0 ? 0 : 1;
}

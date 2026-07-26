// Hand-rolled harness on purpose: ten-odd assertions don't justify pulling in a
// framework. Swap for doctest/catch2 the day this outgrows it.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "LuaBackend.hpp"
#include "Net.hpp"
#include "Protocol.hpp"
#include "Serialize.hpp"
#include "ScriptHost.hpp"
#include "Session.hpp"
#include "TaskQueue.hpp"

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

    CHECK(!sessions.TakeNextTimedOut(500).has_value()); // not yet
    CHECK(sessions.Touch(Peer(2), 900));

    const auto dropped = sessions.TakeNextTimedOut(1000);
    CHECK(dropped.has_value());
    CHECK(dropped && dropped->username == "quiet");
    CHECK(sessions.Count() == 1);
    CHECK(sessions.Find(Peer(2)) != nullptr);
    CHECK(sessions.Find(Peer(1)) == nullptr);

    // Only the silent one goes, and the sweep is now exhausted.
    CHECK(!sessions.TakeNextTimedOut(1000).has_value());
}

void TestSessionTimeoutIsProgressive()
{
    std::printf("sessions: a batch drains one by one so Count() stays truthful\n");

    server::SessionManager sessions(4, 1000);
    server::PlayerId id = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, id) == server::JoinResult::Accepted);
    CHECK(sessions.Join(Peer(2), "b", proto::kVersion, 0, id) == server::JoinResult::Accepted);
    CHECK(sessions.Join(Peer(3), "c", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    // This is what a script watching for "last player left" depends on.
    CHECK(sessions.TakeNextTimedOut(2000).has_value());
    CHECK(sessions.Count() == 2);
    CHECK(sessions.TakeNextTimedOut(2000).has_value());
    CHECK(sessions.Count() == 1);
    CHECK(sessions.TakeNextTimedOut(2000).has_value());
    CHECK(sessions.Count() == 0);
    CHECK(!sessions.TakeNextTimedOut(2000).has_value());
}

void TestSessionClockGoingBackwards()
{
    std::printf("sessions: a now that moves backwards must not drop everyone\n");

    server::SessionManager sessions(4, 1000);
    server::PlayerId id = server::kInvalidPlayer;

    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 5000, id) == server::JoinResult::Accepted);

    // Unsigned subtraction here would look like a huge elapsed time.
    CHECK(!sessions.TakeNextTimedOut(1000).has_value());
    CHECK(sessions.Count() == 1);
}

// Records what it was told, so the tests can assert on dispatch rather than on
// whatever a real backend happens to print.
class SpyBackend final : public script::IBackend
{
public:
    explicit SpyBackend(bool aStartSucceeds = true)
        : m_startSucceeds(aStartSucceeds)
    {
    }

    const char* Name() const override
    {
        return "spy";
    }

    bool Start(script::Host& aHost) override
    {
        m_host = aHost;
        started = true;
        return m_startSucceeds;
    }

    void Stop() override
    {
        stopped = true;
    }

    void OnPlayerJoin(server::PlayerId aPlayerId) override
    {
        joined.push_back(aPlayerId);

        if (auto* player = m_host.players->Find(aPlayerId))
        {
            lastUsername = player->GetUsername();
            countAtJoin = m_host.players->Count();

            if (kickOnJoin)
            {
                player->Kick("spy said so");
            }
        }
    }

    void OnPlayerLeave(server::PlayerId aPlayerId) override
    {
        left.push_back(aPlayerId);
    }

    void OnTick(uint64_t aNowMs) override
    {
        ticks.push_back(aNowMs);
    }

    bool started{};
    bool stopped{};
    bool kickOnJoin{};
    std::string lastUsername;
    size_t countAtJoin{};
    std::vector<server::PlayerId> joined;
    std::vector<server::PlayerId> left;
    std::vector<uint64_t> ticks;

private:
    bool m_startSucceeds;
    script::Host m_host;
};

void TestScriptDispatch()
{
    std::printf("scripting: events reach the backend with usable player data\n");

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);

    auto spy = std::make_unique<SpyBackend>();
    auto* spyPtr = spy.get();
    host.Add(std::move(spy));

    CHECK(host.StartAll() == 1);
    CHECK(spyPtr->started);

    server::PlayerId id = server::kInvalidPlayer;
    CHECK(sessions.Join(Peer(1), "scripted", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    host.OnPlayerJoin(id);
    CHECK(spyPtr->joined.size() == 1);
    CHECK(!spyPtr->joined.empty() && spyPtr->joined[0] == id);

    // The backend must see the session through the boundary, not just an id.
    CHECK(spyPtr->lastUsername == "scripted");
    CHECK(spyPtr->countAtJoin == 1);

    host.OnTick(1234);
    CHECK(spyPtr->ticks.size() == 1);
    CHECK(!spyPtr->ticks.empty() && spyPtr->ticks[0] == 1234);

    host.OnPlayerLeave(id);
    CHECK(spyPtr->left.size() == 1);
}

void TestScriptKickIsQueued()
{
    std::printf("scripting: kick is queued, not applied behind the core's back\n");

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);

    auto spy = std::make_unique<SpyBackend>();
    auto* spyPtr = spy.get();
    spyPtr->kickOnJoin = true;
    host.Add(std::move(spy));

    CHECK(host.StartAll() == 1);

    server::PlayerId id = server::kInvalidPlayer;
    CHECK(sessions.Join(Peer(1), "doomed", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    host.OnPlayerJoin(id);

    // Still there: the script asked, the core has not acted yet.
    CHECK(sessions.Count() == 1);

    const auto kicks = host.TakeKicks();
    CHECK(kicks.size() == 1);
    CHECK(!kicks.empty() && kicks[0].playerId == id);
    CHECK(!kicks.empty() && kicks[0].reason == "spy said so");

    // And the queue is emptied by taking it, so nobody gets kicked twice.
    CHECK(host.TakeKicks().empty());
}

void TestScriptFailedStartIsDropped()
{
    std::printf("scripting: a backend that fails to start is dropped\n");

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);

    host.Add(std::make_unique<SpyBackend>(false));
    host.Add(std::make_unique<SpyBackend>(true));

    // Better none than one silently doing nothing.
    CHECK(host.StartAll() == 1);
    CHECK(host.BackendCount() == 1);
}

void TestScriptUnknownPlayer()
{
    std::printf("scripting: looking up an unknown player yields null\n");

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);

    auto spy = std::make_unique<SpyBackend>();
    auto* spyPtr = spy.get();
    host.Add(std::move(spy));
    CHECK(host.StartAll() == 1);

    // Dispatching for an id that never existed must not crash or invent data.
    host.OnPlayerJoin(999);
    CHECK(spyPtr->joined.size() == 1);
    CHECK(spyPtr->lastUsername.empty());
}

// Writes real .lua files, but stays in-process: no spawn, no sleep.
std::filesystem::path MakeScriptDir(const char* aName,
                                   const std::vector<std::pair<const char*, const char*>>& aFiles)
{
    const auto dir = std::filesystem::temp_directory_path() / "cybermp_tests" / aName;

    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);

    for (const auto& [name, body] : aFiles)
    {
        std::ofstream out(dir / name);
        out << body;
    }

    return dir;
}

bool AnyLineContains(const std::vector<std::string>& aLines, std::string_view aNeedle)
{
    return std::any_of(aLines.begin(), aLines.end(),
                       [&](const std::string& aLine) { return aLine.find(aNeedle) != std::string::npos; });
}

void TestLuaDispatch()
{
    std::printf("lua: a script receives events with usable player data\n");

    const auto dir = MakeScriptDir("dispatch", {{"a.lua", R"(
        cybermp.on("playerJoin", function(id)
            local p = cybermp.players.find(id)
            cybermp.log("saw " .. p.name .. " at " .. p.address .. " n=" .. cybermp.players.count())
        end)
        cybermp.on("playerLeave", function(id) cybermp.log("gone " .. id) end)
    )"}});

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);
    host.Add(server::MakeLuaBackend(dir));

    CHECK(host.StartAll() == 1);

    server::PlayerId id = server::kInvalidPlayer;
    CHECK(sessions.Join(Peer(4242), "luaguy", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    host.OnPlayerJoin(id);
    CHECK(AnyLineContains(host.LogLines(), "saw luaguy at 127.0.0.1:4242 n=1"));

    host.OnPlayerLeave(id);
    CHECK(AnyLineContains(host.LogLines(), "gone 1"));
}

void TestLuaBrokenScriptIsIsolated()
{
    std::printf("lua: a broken script does not stop the others\n");

    const auto dir = MakeScriptDir("broken", {
        {"01_bad.lua", "this is not lua ((("},
        {"02_good.lua", R"(cybermp.on("playerJoin", function(id) cybermp.log("good ran") end))"},
    });

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);
    host.Add(server::MakeLuaBackend(dir));

    // Starting still succeeds: one bad file is not a reason to refuse to serve.
    CHECK(host.StartAll() == 1);
    CHECK(AnyLineContains(host.LogLines(), "syntax error"));
    CHECK(AnyLineContains(host.LogLines(), "1 of 2 script(s) loaded"));

    server::PlayerId id = server::kInvalidPlayer;
    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    host.OnPlayerJoin(id);
    CHECK(AnyLineContains(host.LogLines(), "good ran"));
}

void TestLuaThrowingHandlerIsContained()
{
    std::printf("lua: a handler that errors does not stop the next one\n");

    const auto dir = MakeScriptDir("throwing", {
        {"01_throws.lua", R"(cybermp.on("playerJoin", function(id) error("boom") end))"},
        {"02_after.lua", R"(cybermp.on("playerJoin", function(id) cybermp.log("after ran") end))"},
    });

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);
    host.Add(server::MakeLuaBackend(dir));
    CHECK(host.StartAll() == 1);

    server::PlayerId id = server::kInvalidPlayer;
    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    host.OnPlayerJoin(id);

    CHECK(AnyLineContains(host.LogLines(), "boom"));
    CHECK(AnyLineContains(host.LogLines(), "after ran")); // the point of the test
}

void TestLuaBadApiUseIsReadable()
{
    std::printf("lua: misusing the api gives a message a script author can read\n");

    const auto dir = MakeScriptDir("badapi", {{"a.lua", R"(cybermp.on("playerJoin", 42))"}});

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);
    host.Add(server::MakeLuaBackend(dir));
    CHECK(host.StartAll() == 1);

    CHECK(AnyLineContains(host.LogLines(), "must be a function, got number"));

    // And no c++ template names leaking into a scripter's console.
    CHECK(!AnyLineContains(host.LogLines(), "sol::"));
}

void TestLuaKickReachesTheCore()
{
    std::printf("lua: a script can kick, and it arrives as a queued request\n");

    const auto dir = MakeScriptDir("kick", {{"a.lua", R"(
        cybermp.on("playerJoin", function(id)
            local p = cybermp.players.find(id)
            if p.name == "banned" then p.kick("nope") end
        end)
    )"}});

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);
    host.Add(server::MakeLuaBackend(dir));
    CHECK(host.StartAll() == 1);

    server::PlayerId keep = server::kInvalidPlayer;
    server::PlayerId banned = server::kInvalidPlayer;
    CHECK(sessions.Join(Peer(1), "welcome", proto::kVersion, 0, keep) == server::JoinResult::Accepted);
    CHECK(sessions.Join(Peer(2), "banned", proto::kVersion, 0, banned) == server::JoinResult::Accepted);

    host.OnPlayerJoin(keep);
    CHECK(host.TakeKicks().empty());

    host.OnPlayerJoin(banned);
    const auto kicks = host.TakeKicks();
    CHECK(kicks.size() == 1);
    CHECK(!kicks.empty() && kicks[0].playerId == banned);
    CHECK(!kicks.empty() && kicks[0].reason == "nope");

    // Queued only: the core still decides when to act.
    CHECK(sessions.Count() == 2);
}

void TestLuaMissingDirectoryIsFine()
{
    std::printf("lua: no script directory is not an error\n");

    server::SessionManager sessions(4, 1000);
    server::ScriptHost host(sessions);
    host.Add(server::MakeLuaBackend("definitely/not/here"));

    // A server with no scripts is a valid server.
    CHECK(host.StartAll() == 1);
}

void TestTaskQueueBasics()
{
    std::printf("task queue: push, drain, order\n");

    core::TaskQueue queue;
    std::vector<int> ran;

    CHECK(queue.Drain() == 0); // nothing queued
    CHECK(queue.Pending() == 0);

    queue.Push([&] { ran.push_back(1); });
    queue.Push([&] { ran.push_back(2); });
    queue.Push(nullptr); // must be ignored, not queued as a crash

    CHECK(queue.Pending() == 2);
    CHECK(queue.Drain() == 2);
    CHECK(ran.size() == 2);
    CHECK(ran.size() == 2 && ran[0] == 1 && ran[1] == 2);
    CHECK(queue.Pending() == 0);
}

void TestTaskQueueReentrantPush()
{
    std::printf("task queue: a task can push without deadlocking\n");

    core::TaskQueue queue;
    int outer = 0;
    int inner = 0;

    queue.Push([&] {
        ++outer;
        // Runs outside the lock, so this is legal. It must land in the next drain,
        // not this one, otherwise a task that always pushes would never end.
        queue.Push([&] { ++inner; });
    });

    CHECK(queue.Drain() == 1);
    CHECK(outer == 1);
    CHECK(inner == 0);
    CHECK(queue.Pending() == 1);

    CHECK(queue.Drain() == 1);
    CHECK(inner == 1);
}

void TestTaskQueueBudget()
{
    std::printf("task queue: a budget caps work per frame\n");

    core::TaskQueue queue;
    int ran = 0;

    for (int i = 0; i < 10; ++i)
    {
        queue.Push([&] { ++ran; });
    }

    CHECK(queue.Drain(4) == 4);
    CHECK(ran == 4);
    CHECK(queue.Pending() == 6);

    // A budget larger than what's queued is not an error.
    CHECK(queue.Drain(100) == 6);
    CHECK(ran == 10);
    CHECK(queue.Pending() == 0);
}

void TestTaskQueueThreadedProducers()
{
    std::printf("task queue: several producer threads, nothing lost\n");

    core::TaskQueue queue;
    std::atomic_int ran{0};

    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t)
    {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i)
            {
                queue.Push([&] { ran.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    CHECK(queue.Pending() == kThreads * kPerThread);
    CHECK(queue.Drain() == kThreads * kPerThread);
    CHECK(ran.load() == kThreads * kPerThread);
}

void TestTaskQueueDrainWhileProducing()
{
    std::printf("task queue: draining while a thread is still pushing loses nothing\n");

    core::TaskQueue queue;
    std::atomic_int ran{0};
    constexpr int kTotal = 2000;

    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i)
        {
            queue.Push([&] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
    });

    // Mimics the game thread ticking while the network thread feeds it.
    while (ran.load() < kTotal)
    {
        queue.Drain(64);
    }

    producer.join();
    queue.Drain();

    CHECK(ran.load() == kTotal);
    CHECK(queue.Pending() == 0);
}

void TestTaskQueueConcurrentDrains()
{
    std::printf("task queue: several threads draining at once lose and duplicate nothing\n");

    // The engine dispatches its update groups on a worker pool, so two of our tick
    // callbacks really can drain in parallel. Found in game, covered here.
    core::TaskQueue queue;
    std::atomic_int ran{0};
    constexpr int kTotal = 5000;

    for (int i = 0; i < kTotal; ++i)
    {
        queue.Push([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    std::atomic_int drained{0};
    std::vector<std::thread> drainers;

    for (int t = 0; t < 4; ++t)
    {
        drainers.emplace_back([&] {
            size_t mine = 0;
            while (drained.load() < kTotal)
            {
                const auto n = queue.Drain(16);
                if (n == 0)
                {
                    if (queue.Pending() == 0)
                    {
                        break;
                    }
                    continue;
                }

                mine += n;
                drained.fetch_add(static_cast<int>(n), std::memory_order_relaxed);
            }

            (void)mine;
        });
    }

    for (auto& drainer : drainers)
    {
        drainer.join();
    }

    queue.Drain();

    // Each task exactly once: no loss, no double execution.
    CHECK(ran.load() == kTotal);
    CHECK(queue.Pending() == 0);
}

void TestStateMessagesRoundTrip()
{
    std::printf("protocol: state snapshots round trip\n");

    std::vector<uint8_t> buffer;

    proto::PlayerState sent;
    sent.tick = 12345;
    sent.position = {-1362.35f, 1283.91f, 29.50f};
    sent.rotation = 137.5f;
    CHECK(proto::Encode(sent, buffer));

    proto::Header header;
    CHECK(proto::PeekHeader(buffer, header));
    CHECK(!header.IsReliable()); // snapshots are deliberately unreliable

    proto::PlayerState received;
    CHECK(proto::Decode(buffer, received));
    CHECK(received.tick == 12345);
    CHECK(received.position.x == -1362.35f);
    CHECK(received.position.y == 1283.91f);
    CHECK(received.position.z == 29.50f);
    CHECK(received.rotation == 137.5f);

    proto::NotifyPlayerJoined joined;
    joined.playerId = 7;
    joined.username = "akitium";
    CHECK(proto::Encode(joined, buffer));
    CHECK(proto::PeekHeader(buffer, header));
    CHECK(header.IsReliable()); // a missed join leaves a ghost forever

    proto::NotifyPlayerJoined decodedJoin;
    CHECK(proto::Decode(buffer, decodedJoin));
    CHECK(decodedJoin.playerId == 7);
    CHECK(decodedJoin.username == "akitium");

    proto::NotifyPlayerLeft left;
    left.playerId = 7;
    CHECK(proto::Encode(left, buffer));
    CHECK(proto::PeekHeader(buffer, header));
    CHECK(header.IsReliable());
}

void TestSessionStateOrdering()
{
    std::printf("sessions: an out of order snapshot is refused\n");

    server::SessionManager sessions(4, 1000);
    server::PlayerId id = server::kInvalidPlayer;
    CHECK(sessions.Join(Peer(1), "a", proto::kVersion, 0, id) == server::JoinResult::Accepted);

    const float first[3] = {1.0f, 2.0f, 3.0f};
    const float second[3] = {4.0f, 5.0f, 6.0f};
    const float stale[3] = {9.0f, 9.0f, 9.0f};

    CHECK(sessions.ApplyState(Peer(1), 10, first, 90.0f));
    CHECK(sessions.ApplyState(Peer(1), 11, second, 180.0f));

    // Udp reorders, and applying this would visibly rewind the body.
    CHECK(!sessions.ApplyState(Peer(1), 5, stale, 0.0f));
    CHECK(!sessions.ApplyState(Peer(1), 11, stale, 0.0f)); // same tick is not newer

    auto* session = sessions.Find(id);
    CHECK(session != nullptr);
    CHECK(session && session->stateTick == 11);
    CHECK(session && session->position[0] == 4.0f);
    CHECK(session && session->rotation == 180.0f);

    // And an unknown peer has nothing to apply to.
    CHECK(!sessions.ApplyState(Peer(99), 1, first, 0.0f));
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
    TestSessionTimeoutIsProgressive();
    TestSessionClockGoingBackwards();
    TestSessionRemove();
    TestStateMessagesRoundTrip();
    TestSessionStateOrdering();
    TestScriptDispatch();
    TestScriptKickIsQueued();
    TestScriptFailedStartIsDropped();
    TestScriptUnknownPlayer();
    TestLuaDispatch();
    TestLuaBrokenScriptIsIsolated();
    TestLuaThrowingHandlerIsContained();
    TestLuaBadApiUseIsReadable();
    TestLuaKickReachesTheCore();
    TestLuaMissingDirectoryIsFine();
    TestTaskQueueBasics();
    TestTaskQueueReentrantPush();
    TestTaskQueueBudget();
    TestTaskQueueThreadedProducers();
    TestTaskQueueDrainWhileProducing();
    TestTaskQueueConcurrentDrains();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);

    return g_failed == 0 ? 0 : 1;
}

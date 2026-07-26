// cybermp server -- handshake, sessions, timeouts, ping/pong.
// Raw UDP for now; GameNetworkingSockets comes in once we need reliability and
// encryption. Nothing is logged on the per-datagram path: unbuffered stdout costs
// a syscall per line and showed up as +23 ms of rtt.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <vector>

#include <Version.hpp>

#include "Net.hpp"
#include "Protocol.hpp"
#include "Session.hpp"

namespace
{
constexpr uint16_t kDefaultPort = 11780;
constexpr size_t kMaxPlayers = 32;
constexpr uint64_t kTimeoutMs = 10000;
constexpr uint32_t kPollMs = 250;

std::atomic_bool g_running{true};

void OnSignal(int)
{
    g_running = false;
}

uint64_t NowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool Send(net::UdpSocket& aSocket, const net::Endpoint& aTo, const std::vector<uint8_t>& aData)
{
    return aSocket.SendTo(aTo, {aData.data(), aData.size()});
}

void HandleHello(net::UdpSocket& aSocket, server::SessionManager& aSessions, const net::Endpoint& aFrom,
                 std::span<const uint8_t> aData, uint64_t aNowMs)
{
    proto::Hello hello;
    if (!proto::Decode(aData, hello))
    {
        std::printf("!! malformed hello from %s\n", aFrom.ToString().c_str());
        return;
    }

    server::PlayerId playerId = server::kInvalidPlayer;
    const auto result = aSessions.Join(aFrom, hello.username, hello.version, aNowMs, playerId);

    proto::HelloAck ack;
    ack.accepted = result == server::JoinResult::Accepted || result == server::JoinResult::AlreadyJoined;

    if (!ack.accepted)
    {
        // Always say why: a client that cannot connect needs something to show.
        ack.reason = server::ToString(result);
        std::printf("-- refused '%s' from %s: %s\n", hello.username.c_str(), aFrom.ToString().c_str(),
                    ack.reason.c_str());
    }
    else if (result == server::JoinResult::Accepted)
    {
        std::printf("++ join  #%u '%s' from %s  (%zu online)\n", playerId, hello.username.c_str(),
                    aFrom.ToString().c_str(), aSessions.Count());
    }

    std::vector<uint8_t> out;
    if (proto::Encode(ack, out))
    {
        Send(aSocket, aFrom, out);
    }
}

void HandlePing(net::UdpSocket& aSocket, server::SessionManager& aSessions, const net::Endpoint& aFrom,
                std::span<const uint8_t> aData)
{
    // Only known peers get answered, so an unknown sender can't use us as a reflector.
    if (!aSessions.Find(aFrom))
    {
        return;
    }

    proto::Ping ping;
    if (!proto::Decode(aData, ping))
    {
        return;
    }

    // Echo the client's own timestamp: it measures the rtt, we stay stateless.
    proto::Pong pong;
    pong.sentAt = ping.sentAt;

    std::vector<uint8_t> out;
    if (proto::Encode(pong, out))
    {
        Send(aSocket, aFrom, out);
    }
}
} // namespace

int main(int argc, char** argv)
{
    const auto port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : kDefaultPort;

    // Redirected stdout is fully buffered on msvc, which loses every log line if the
    // process is killed. A server has to log as it goes.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::printf("cybermp server %s (%s), protocol %u\n", CYBERMP_VERSION, CYBERMP_GIT_HASH, proto::kVersion);

    net::Startup startup;
    if (!startup.ok())
    {
        std::printf("winsock init failed\n");
        return 1;
    }

    net::UdpSocket socket;
    if (!socket.Bind(port))
    {
        std::printf("could not bind port %u\n", port);
        return 1;
    }

    std::printf("listening on udp %u, %zu slots, %llu ms timeout\n", port, kMaxPlayers, kTimeoutMs);

    server::SessionManager sessions(kMaxPlayers, kTimeoutMs);
    std::vector<uint8_t> buffer(proto::kMaxDatagram);
    uint64_t rejected = 0;

    while (g_running)
    {
        const auto now = NowMs();

        // Several peers can expire in one sweep, so the running total is only correct
        // once the whole batch is gone. Report it after the loop, not per line.
        const auto dropped = sessions.CollectTimedOut(now);
        for (const auto& session : dropped)
        {
            std::printf("-- leave #%u '%s' timed out\n", session.id, session.username.c_str());
        }

        if (!dropped.empty())
        {
            std::printf("-- %zu online\n", sessions.Count());
        }

        const auto received = socket.RecvFrom(buffer, kPollMs);
        if (!received)
        {
            continue; // poll timeout, loop so timeouts still get checked
        }

        const std::span<const uint8_t> datagram(buffer.data(), received->size);

        proto::Header header;
        if (!proto::PeekHeader(datagram, header))
        {
            // Anything off the wire can be junk. Drop it and keep serving.
            ++rejected;
            continue;
        }

        sessions.Touch(received->from, now);

        switch (header.type)
        {
        case proto::Type::Hello:
            HandleHello(socket, sessions, received->from, datagram, now);
            break;

        case proto::Type::Ping:
            HandlePing(socket, sessions, received->from, datagram);
            break;

        default:
            ++rejected;
            break;
        }
    }

    std::printf("stopped, %zu online, %llu datagram(s) rejected\n", sessions.Count(), rejected);

    return 0;
}

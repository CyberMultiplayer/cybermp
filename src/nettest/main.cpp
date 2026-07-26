// Console client: handshake, then ping/pong with rtt. Stands in for the game so
// the protocol can be exercised without launching it.
//
//   cybermp_nettest [port] [rounds] [protocolOverride] [username]
//   cybermp_nettest --walk [port] [x] [y] [z] [radius] [username]
//
// The override exists to check the server actually refuses a mismatched version.
//
// --walk stands in for a second game instance: it joins and sends snapshots along a
// circle, which makes a stutter or a rewind obvious in a way two human players never
// would.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Windows.h> // GetCurrentProcessId, for a unique default name

#include <Version.hpp>

#include "Net.hpp"
#include "Protocol.hpp"

namespace
{
constexpr uint16_t kDefaultPort = 11780;
constexpr uint32_t kTimeoutMs = 1000;

uint64_t NowMicros()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool Exchange(net::UdpSocket& aSocket, const net::Endpoint& aServer, const std::vector<uint8_t>& aRequest,
              std::vector<uint8_t>& aBuffer, size_t& aReceived)
{
    if (!aSocket.SendTo(aServer, {aRequest.data(), aRequest.size()}))
    {
        return false;
    }

    const auto received = aSocket.RecvFrom(aBuffer, kTimeoutMs);
    if (!received)
    {
        return false;
    }

    aReceived = received->size;
    return true;
}
} // namespace

namespace
{
// Shared by both modes: join, then hand back the socket ready to use.
bool Handshake(net::UdpSocket& aSocket, const net::Endpoint& aServer, const std::string& aUsername,
               uint32_t aVersion, std::vector<uint8_t>& aBuffer)
{
    proto::Hello hello;
    hello.version = aVersion;
    hello.username = aUsername;

    std::vector<uint8_t> request;
    if (!proto::Encode(hello, request))
    {
        std::printf("could not encode hello\n");
        return false;
    }

    size_t received = 0;
    if (!Exchange(aSocket, aServer, request, aBuffer, received))
    {
        std::printf("handshake: no answer after %u ms\n", kTimeoutMs);
        return false;
    }

    proto::HelloAck ack;
    if (!proto::Decode({aBuffer.data(), received}, ack))
    {
        std::printf("handshake: malformed answer\n");
        return false;
    }

    if (!ack.accepted)
    {
        std::printf("handshake refused: %s (server protocol %u)\n", ack.reason.c_str(), ack.version);
        return false;
    }

    std::printf("handshake ok, server protocol %u\n", ack.version);
    return true;
}

int RunWalk(int argc, char** argv)
{
    const auto port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : kDefaultPort;
    const auto centreX = argc > 3 ? std::strtof(argv[3], nullptr) : 0.0f;
    const auto centreY = argc > 4 ? std::strtof(argv[4], nullptr) : 0.0f;
    const auto centreZ = argc > 5 ? std::strtof(argv[5], nullptr) : 0.0f;
    const auto radius = argc > 6 ? std::strtof(argv[6], nullptr) : 4.0f;
    const std::string username = argc > 7 ? argv[7] : ("walker-" + std::to_string(GetCurrentProcessId()));

    net::Startup startup;
    if (!startup.ok())
    {
        std::printf("winsock init failed\n");
        return 1;
    }

    net::UdpSocket socket;
    if (!socket.Open())
    {
        std::printf("could not open socket\n");
        return 1;
    }

    const auto server = net::UdpSocket::Loopback(port);
    std::vector<uint8_t> buffer(proto::kMaxDatagram);

    std::printf("walking as '%s' around %.2f, %.2f, %.2f radius %.2f\n", username.c_str(), centreX, centreY, centreZ,
                radius);

    if (!Handshake(socket, server, username, proto::kVersion, buffer))
    {
        return 1;
    }

    uint64_t tick = 0;
    uint64_t statesIn = 0;
    uint64_t outOfOrder = 0;
    std::unordered_map<uint32_t, uint64_t> lastTickPerPlayer;

    std::printf("sending snapshots at ~15 Hz, ctrl+c to stop\n");

    for (;;)
    {
        ++tick;

        // One full circle every 8 seconds at 15 Hz.
        const auto angle = static_cast<float>(tick) * (6.283185307f / 120.0f);

        proto::PlayerState state;
        state.tick = tick;
        state.position = {centreX + radius * std::cos(angle), centreY + radius * std::sin(angle), centreZ};
        state.rotation = angle * 57.29577951f; // degrees, so the body faces its travel

        std::vector<uint8_t> out;
        if (proto::Encode(state, out) && !socket.SendTo(server, {out.data(), out.size()}))
        {
            std::printf("send failed\n");
            return 1;
        }

        // Drain what the server relays, and count it: this is the only proof from a
        // console that other players' snapshots actually come back.
        while (const auto received = socket.RecvFrom(buffer, 0))
        {
            const std::span<const uint8_t> datagram(buffer.data(), received->size);

            proto::Header header;
            if (!proto::PeekHeader(datagram, header))
            {
                continue;
            }

            if (header.type == proto::Type::NotifyPlayerState)
            {
                proto::NotifyPlayerState notify;
                if (!proto::Decode(datagram, notify))
                {
                    continue;
                }

                ++statesIn;

                auto& last = lastTickPerPlayer[notify.playerId];
                if (last != 0 && notify.tick <= last)
                {
                    ++outOfOrder;
                }
                else
                {
                    last = notify.tick;
                }
            }
            else if (header.type == proto::Type::NotifyPlayerJoined)
            {
                proto::NotifyPlayerJoined joined;
                if (proto::Decode(datagram, joined))
                {
                    std::printf("saw player #%u '%s' join\n", joined.playerId, joined.username.c_str());
                }
            }
            else if (header.type == proto::Type::NotifyPlayerLeft)
            {
                proto::NotifyPlayerLeft left;
                if (proto::Decode(datagram, left))
                {
                    std::printf("saw player #%u leave\n", left.playerId);
                }
            }
        }

        if (tick % 15 == 0)
        {
            std::printf("tick %llu at %.2f, %.2f | others=%zu statesIn=%llu outOfOrder=%llu\n", tick,
                        state.position.x, state.position.y, lastTickPerPlayer.size(), statesIn, outOfOrder);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(66));
    }
}
} // namespace

int main(int argc, char** argv)
{
    // Redirected stdout is fully buffered on msvc, so a killed process loses
    // everything it printed. Same trap the server hit.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc > 1 && std::string_view(argv[1]) == "--walk")
    {
        std::printf("cybermp nettest %s (%s), protocol %u\n", CYBERMP_VERSION, CYBERMP_GIT_HASH, proto::kVersion);
        return RunWalk(argc, argv);
    }

    const auto port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : kDefaultPort;
    const auto rounds = argc > 2 ? std::atoi(argv[2]) : 3;
    const auto protocolOverride = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : proto::kVersion;

    std::printf("cybermp nettest %s (%s), protocol %u\n", CYBERMP_VERSION, CYBERMP_GIT_HASH, protocolOverride);

    net::Startup startup;
    if (!startup.ok())
    {
        std::printf("winsock init failed\n");
        return 1;
    }

    net::UdpSocket socket;
    if (!socket.Open())
    {
        std::printf("could not open socket\n");
        return 1;
    }

    const auto server = net::UdpSocket::Loopback(port);
    std::vector<uint8_t> buffer(proto::kMaxDatagram);
    std::vector<uint8_t> request;
    size_t received = 0;

    // --- handshake ---
    // Name derived from the pid so several instances can join at once without
    // colliding on the server's duplicate-name check.
    proto::Hello hello;
    hello.version = protocolOverride;
    hello.username = argc > 4 ? argv[4] : ("nettest-" + std::to_string(GetCurrentProcessId()));

    std::printf("joining as '%s'\n", hello.username.c_str());

    if (!proto::Encode(hello, request))
    {
        std::printf("could not encode hello\n");
        return 1;
    }

    if (!Exchange(socket, server, request, buffer, received))
    {
        std::printf("handshake: no answer after %u ms\n", kTimeoutMs);
        return 1;
    }

    proto::HelloAck ack;
    if (!proto::Decode({buffer.data(), received}, ack))
    {
        std::printf("handshake: malformed answer\n");
        return 1;
    }

    if (!ack.accepted)
    {
        std::printf("handshake refused: %s (server protocol %u)\n", ack.reason.c_str(), ack.version);
        return 2; // distinct from a transport failure
    }

    std::printf("handshake ok, server protocol %u\n", ack.version);

    // --- ping / pong ---
    int answered = 0;

    for (int round = 1; round <= rounds; ++round)
    {
        proto::Ping ping;
        ping.sentAt = NowMicros();

        if (!proto::Encode(ping, request))
        {
            std::printf("%d: could not encode ping\n", round);
            continue;
        }

        if (!Exchange(socket, server, request, buffer, received))
        {
            std::printf("%d: timeout after %u ms\n", round, kTimeoutMs);
            continue;
        }

        proto::Pong pong;
        if (!proto::Decode({buffer.data(), received}, pong))
        {
            std::printf("%d: malformed pong\n", round);
            continue;
        }

        if (pong.sentAt != ping.sentAt)
        {
            std::printf("%d: pong timestamp mismatch\n", round);
            continue;
        }

        const auto rtt = static_cast<double>(NowMicros() - pong.sentAt) / 1000.0;
        std::printf("%d: pong, rtt %.3f ms\n", round, rtt);

        ++answered;
    }

    std::printf("%d/%d answered\n", answered, rounds);

    return answered == rounds ? 0 : 1;
}

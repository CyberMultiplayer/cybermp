// Console client: handshake, then ping/pong with rtt. Stands in for the game so
// the protocol can be exercised without launching it.
//
//   cybermp_nettest [port] [rounds] [protocolOverride] [username]
//
// The override exists to check the server actually refuses a mismatched version.

#include <chrono>
#include <cstdio>
#include <string>
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

int main(int argc, char** argv)
{
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

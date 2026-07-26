// cybermp server -- speaks the B1 protocol: handshake then ping/pong.
// Raw UDP for now. GameNetworkingSockets comes in once we need reliability and
// encryption; the point here is the message loop, not the transport.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <vector>

#include <Version.hpp>

#include "Net.hpp"
#include "Protocol.hpp"

namespace
{
constexpr uint16_t kDefaultPort = 11780;

std::atomic_bool g_running{true};

void OnSignal(int)
{
    g_running = false;
}

bool Send(net::UdpSocket& aSocket, const net::Endpoint& aTo, const std::vector<uint8_t>& aData)
{
    return aSocket.SendTo(aTo, {aData.data(), aData.size()});
}

void HandleHello(net::UdpSocket& aSocket, const net::Endpoint& aFrom, std::span<const uint8_t> aData)
{
    proto::Hello hello;
    if (!proto::Decode(aData, hello))
    {
        std::printf("!! malformed hello from %s\n", aFrom.ToString().c_str());
        return;
    }

    proto::HelloAck ack;
    ack.accepted = hello.version == proto::kVersion;

    if (!ack.accepted)
    {
        // Say why, so a mismatched client can report something useful instead of
        // silently failing to connect.
        ack.reason = "protocol version mismatch";
        std::printf("-- refused '%s': client protocol %u, server %u\n", hello.username.c_str(), hello.version,
                    proto::kVersion);
    }
    else
    {
        std::printf("-- accepted '%s' from %s\n", hello.username.c_str(), aFrom.ToString().c_str());
    }

    std::vector<uint8_t> out;
    if (proto::Encode(ack, out))
    {
        Send(aSocket, aFrom, out);
    }
}

void HandlePing(net::UdpSocket& aSocket, const net::Endpoint& aFrom, std::span<const uint8_t> aData)
{
    proto::Ping ping;
    if (!proto::Decode(aData, ping))
    {
        std::printf("!! malformed ping from %s\n", aFrom.ToString().c_str());
        return;
    }

    // Echo the client's own timestamp back: it measures the rtt, we stay stateless.
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

    std::printf("listening on udp %u, ctrl+c to stop\n", port);

    std::vector<uint8_t> buffer(proto::kMaxDatagram);
    uint64_t handled = 0;
    uint64_t rejected = 0;

    while (g_running)
    {
        const auto received = socket.RecvFrom(buffer, 250);
        if (!received)
        {
            continue; // timeout, poll again
        }

        const std::span<const uint8_t> datagram(buffer.data(), received->size);

        proto::Header header;
        if (!proto::PeekHeader(datagram, header))
        {
            // Anything off the wire can be junk. Drop it and keep serving.
            std::printf("!! unreadable datagram from %s, %zu bytes\n", received->from.ToString().c_str(),
                        received->size);
            ++rejected;
            continue;
        }

        switch (header.type)
        {
        case proto::Type::Hello:
            HandleHello(socket, received->from, datagram);
            break;

        case proto::Type::Ping:
            HandlePing(socket, received->from, datagram);
            break;

        default:
            std::printf("!! unexpected type %u from %s\n", static_cast<unsigned>(header.type),
                        received->from.ToString().c_str());
            ++rejected;
            break;
        }

        ++handled;
    }

    std::printf("stopped, %llu handled, %llu rejected\n", handled, rejected);

    return 0;
}

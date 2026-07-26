// B0.3 -- console client: sends PING, waits for PONG, reports RTT.
// Stands in for the game client so the transport can be exercised without launching it.

#include <chrono>
#include <cstdio>
#include <string_view>
#include <vector>

#include <Version.hpp>

#include "Net.hpp"

namespace
{
constexpr uint16_t kDefaultPort = 11780;
constexpr uint32_t kTimeoutMs = 1000;
constexpr size_t kMaxDatagram = 1200;
} // namespace

int main(int argc, char** argv)
{
    const auto port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : kDefaultPort;
    const auto rounds = argc > 2 ? std::atoi(argv[2]) : 3;

    std::printf("cybermp nettest %s (%s)\n", CYBERMP_VERSION, CYBERMP_GIT_HASH);

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
    std::vector<uint8_t> buffer(kMaxDatagram);

    int answered = 0;

    for (int round = 1; round <= rounds; ++round)
    {
        constexpr std::string_view ping = "PING";
        const auto* bytes = reinterpret_cast<const uint8_t*>(ping.data());

        const auto sentAt = std::chrono::steady_clock::now();

        if (!socket.SendTo(server, {bytes, ping.size()}))
        {
            std::printf("%d: send failed\n", round);
            continue;
        }

        const auto received = socket.RecvFrom(buffer, kTimeoutMs);
        if (!received)
        {
            std::printf("%d: timeout after %u ms\n", round, kTimeoutMs);
            continue;
        }

        const auto rtt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sentAt).count();
        const std::string_view payload(reinterpret_cast<const char*>(buffer.data()), received->size);

        std::printf("%d: '%.*s' from %s, rtt %.3f ms\n", round, static_cast<int>(payload.size()), payload.data(),
                    received->from.ToString().c_str(), rtt);

        if (payload == "PONG")
        {
            ++answered;
        }
    }

    std::printf("%d/%d answered\n", answered, rounds);

    return answered == rounds ? 0 : 1;
}

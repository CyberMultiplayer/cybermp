// cybermp server -- B0.2: listens and answers PING with PONG.
// Raw UDP for now. GameNetworkingSockets comes in once we need reliability
// and encryption; the point here is to prove the loop, not the transport.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string_view>
#include <vector>

#include <Version.hpp>

#include "Net.hpp"

namespace
{
constexpr uint16_t kDefaultPort = 11780;
constexpr size_t kMaxDatagram = 1200; // stays under the usual MTU

std::atomic_bool g_running{true};

void OnSignal(int)
{
    g_running = false;
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

    std::printf("cybermp server %s (%s)\n", CYBERMP_VERSION, CYBERMP_GIT_HASH);

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

    std::vector<uint8_t> buffer(kMaxDatagram);
    uint64_t handled = 0;

    while (g_running)
    {
        const auto received = socket.RecvFrom(buffer, 250);
        if (!received)
        {
            continue; // timeout, just poll again
        }

        const std::string_view payload(reinterpret_cast<const char*>(buffer.data()), received->size);
        std::printf("<- %s  %zu bytes  '%.*s'\n", received->from.ToString().c_str(), received->size,
                    static_cast<int>(payload.size()), payload.data());

        if (payload == "PING")
        {
            constexpr std::string_view pong = "PONG";
            const auto* bytes = reinterpret_cast<const uint8_t*>(pong.data());

            if (socket.SendTo(received->from, {bytes, pong.size()}))
            {
                std::printf("-> %s  PONG\n", received->from.ToString().c_str());
            }
            else
            {
                std::printf("!! send failed\n");
            }
        }

        ++handled;
    }

    std::printf("stopped after %llu datagram(s)\n", handled);

    return 0;
}

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace net
{
// Winsock needs a process-wide init/teardown pair.
class Startup
{
public:
    Startup();
    ~Startup();

    Startup(const Startup&) = delete;
    Startup& operator=(const Startup&) = delete;

    bool ok() const
    {
        return m_ok;
    }

private:
    bool m_ok{};
};

struct Endpoint
{
    uint32_t address{}; // network byte order
    uint16_t port{};

    std::string ToString() const;
};

struct Received
{
    size_t size{};
    Endpoint from;
};

class UdpSocket
{
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Open();

    // Pass 0 to let the OS pick a free port, then read it back with LocalPort().
    bool Bind(uint16_t aPort);
    uint16_t LocalPort() const;
    void Close();

    bool SendTo(const Endpoint& aTo, std::span<const uint8_t> aData);

    // Blocks up to aTimeoutMs. Empty result means timeout, not an error.
    std::optional<Received> RecvFrom(std::span<uint8_t> aBuffer, uint32_t aTimeoutMs);

    static Endpoint Loopback(uint16_t aPort);

private:
    uintptr_t m_handle{};
    bool m_open{};
};
} // namespace net

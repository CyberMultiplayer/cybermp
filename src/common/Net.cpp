#include "Net.hpp"

#include <format>

#include <WinSock2.h>
#include <WS2tcpip.h>

namespace net
{
Startup::Startup()
{
    WSADATA data{};
    m_ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

Startup::~Startup()
{
    if (m_ok)
    {
        WSACleanup();
    }
}

std::string Endpoint::ToString() const
{
    in_addr addr{};
    addr.s_addr = address;

    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr, text, sizeof(text));

    return std::format("{}:{}", text, port);
}

UdpSocket::~UdpSocket()
{
    Close();
}

bool UdpSocket::Open()
{
    if (m_open)
    {
        return true;
    }

    const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET)
    {
        return false;
    }

    m_handle = static_cast<uintptr_t>(handle);
    m_open = true;

    return true;
}

bool UdpSocket::Bind(uint16_t aPort)
{
    if (!m_open && !Open())
    {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(aPort);

    return bind(static_cast<SOCKET>(m_handle), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR;
}

uint16_t UdpSocket::LocalPort() const
{
    if (!m_open)
    {
        return 0;
    }

    sockaddr_in address{};
    int size = sizeof(address);

    if (getsockname(static_cast<SOCKET>(m_handle), reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR)
    {
        return 0;
    }

    return ntohs(address.sin_port);
}

void UdpSocket::Close()
{
    if (m_open)
    {
        closesocket(static_cast<SOCKET>(m_handle));
        m_open = false;
        m_handle = 0;
    }
}

bool UdpSocket::SendTo(const Endpoint& aTo, std::span<const uint8_t> aData)
{
    if (!m_open)
    {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = aTo.address;
    address.sin_port = htons(aTo.port);

    const auto sent = sendto(static_cast<SOCKET>(m_handle), reinterpret_cast<const char*>(aData.data()),
                             static_cast<int>(aData.size()), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));

    return sent == static_cast<int>(aData.size());
}

std::optional<Received> UdpSocket::RecvFrom(std::span<uint8_t> aBuffer, uint32_t aTimeoutMs)
{
    if (!m_open)
    {
        return std::nullopt;
    }

    const auto handle = static_cast<SOCKET>(m_handle);

    fd_set readable{};
    FD_ZERO(&readable);
    FD_SET(handle, &readable);

    timeval timeout{};
    timeout.tv_sec = static_cast<long>(aTimeoutMs / 1000);
    timeout.tv_usec = static_cast<long>((aTimeoutMs % 1000) * 1000);

    if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
    {
        return std::nullopt;
    }

    sockaddr_in address{};
    int addressSize = sizeof(address);

    const auto read = recvfrom(handle, reinterpret_cast<char*>(aBuffer.data()), static_cast<int>(aBuffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&address), &addressSize);

    if (read <= 0)
    {
        return std::nullopt;
    }

    Received result{};
    result.size = static_cast<size_t>(read);
    result.from.address = address.sin_addr.s_addr;
    result.from.port = ntohs(address.sin_port);

    return result;
}

Endpoint UdpSocket::Loopback(uint16_t aPort)
{
    Endpoint endpoint{};
    inet_pton(AF_INET, "127.0.0.1", &endpoint.address);
    endpoint.port = aPort;

    return endpoint;
}
} // namespace net

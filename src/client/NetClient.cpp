#include "NetClient.hpp"

#include <chrono>
#include <vector>

#include "Log.hpp"
#include "Protocol.hpp"

namespace client
{
namespace
{
constexpr uint32_t kPollMs = 50;
constexpr uint32_t kHelloRetryMs = 500;
constexpr uint32_t kMaxHelloAttempts = 10;
constexpr uint32_t kPingIntervalMs = 1000;

// No traffic for this long and we consider the server gone. Generous compared to
// the ping interval so one lost datagram doesn't drop the connection.
constexpr uint64_t kServerTimeoutMs = 8000;

uint64_t NowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t NowMicros()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}
} // namespace

const char* ToString(NetState aState)
{
    switch (aState)
    {
    case NetState::Disconnected:
        return "disconnected";
    case NetState::Connecting:
        return "connecting";
    case NetState::Connected:
        return "connected";
    case NetState::Refused:
        return "refused";
    }

    return "unknown";
}

NetClient::NetClient(core::TaskQueue& aTasks)
    : m_tasks(aTasks)
{
}

NetClient::~NetClient()
{
    Disconnect();
}

bool NetClient::Connect(const std::string& aHost, uint16_t aPort, const std::string& aUsername)
{
    Disconnect();

    if (!m_startup.ok())
    {
        SetError("winsock init failed");
        return false;
    }

    if (!m_socket.Open())
    {
        SetError("could not open socket");
        return false;
    }

    // Loopback only for now: resolving a hostname is C2's problem, not C1's.
    m_server = net::UdpSocket::Loopback(aPort);
    m_username = aUsername;

    m_sent = 0;
    m_received = 0;
    m_malformed = 0;
    m_helloAttempts = 0;
    m_lastRttMs = 0.0;
    m_appliedOnGameThread = 0;
    m_statesSent = 0;
    m_statesReceived = 0;
    m_statesOutOfOrder = 0;
    m_stateTick = 0;

    {
        std::scoped_lock lock(m_remoteMutex);
        m_remote.clear();
    }

    {
        std::scoped_lock lock(m_eventMutex);
        m_events.clear();
    }

    SetError({});

    m_state = NetState::Connecting;
    m_thread = std::jthread([this](std::stop_token aStop) { Run(aStop); });

    CYBERMP_INFO("connecting to %s:%u as '%s'", aHost.c_str(), aPort, aUsername.c_str());

    return true;
}

void NetClient::Disconnect()
{
    if (m_thread.joinable())
    {
        m_thread.request_stop();
        m_thread.join();
    }

    m_socket.Close();
    m_state = NetState::Disconnected;
}

void NetClient::Run(std::stop_token aStop)
{
    std::vector<uint8_t> buffer(proto::kMaxDatagram);

    uint64_t lastHelloAt = 0;
    uint64_t lastPingAt = 0;
    uint64_t lastServerAt = NowMs();

    while (!aStop.stop_requested())
    {
        const auto now = NowMs();
        const auto state = m_state.load();

        if (state == NetState::Connecting)
        {
            if (now - lastHelloAt >= kHelloRetryMs)
            {
                if (m_helloAttempts.load() >= kMaxHelloAttempts)
                {
                    SetError("no answer from server");
                    m_state = NetState::Disconnected;
                    return;
                }

                // Udp loses packets, so hello is resent until acked. The server
                // answers a repeated hello with the same player id.
                SendHello();
                lastHelloAt = now;
            }
        }
        else if (state == NetState::Connected)
        {
            if (now - lastPingAt >= kPingIntervalMs)
            {
                SendPing();
                lastPingAt = now;
            }

            if (now - lastServerAt >= kServerTimeoutMs)
            {
                SetError("server stopped answering");
                m_state = NetState::Disconnected;
                return;
            }
        }
        else
        {
            return; // refused or disconnected, nothing left to do
        }

        const auto received = m_socket.RecvFrom(buffer, kPollMs);
        if (!received)
        {
            continue; // poll timeout, loop so the timers still run
        }

        // Only the server we asked for. Anything else is noise or spoofing.
        if (!(received->from == m_server))
        {
            continue;
        }

        m_received.fetch_add(1, std::memory_order_relaxed);
        lastServerAt = now;

        HandleDatagram({buffer.data(), received->size});
    }
}

bool NetClient::SendHello()
{
    proto::Hello hello;
    hello.username = m_username;

    std::vector<uint8_t> out;
    if (!proto::Encode(hello, out))
    {
        SetError("could not encode hello");
        return false;
    }

    m_helloAttempts.fetch_add(1, std::memory_order_relaxed);
    m_sent.fetch_add(1, std::memory_order_relaxed);

    return m_socket.SendTo(m_server, {out.data(), out.size()});
}

bool NetClient::SendPing()
{
    proto::Ping ping;
    ping.sentAt = NowMicros();

    std::vector<uint8_t> out;
    if (!proto::Encode(ping, out))
    {
        return false;
    }

    m_sent.fetch_add(1, std::memory_order_relaxed);

    return m_socket.SendTo(m_server, {out.data(), out.size()});
}

void NetClient::HandleDatagram(std::span<const uint8_t> aData)
{
    proto::Header header;
    if (!proto::PeekHeader(aData, header))
    {
        m_malformed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    switch (header.type)
    {
    case proto::Type::HelloAck:
    {
        proto::HelloAck ack;
        if (!proto::Decode(aData, ack))
        {
            m_malformed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (ack.accepted)
        {
            m_state = NetState::Connected;
            CYBERMP_INFO("connected, server protocol %u", ack.version);
        }
        else
        {
            SetError(ack.reason.empty() ? "refused" : ack.reason);
            m_state = NetState::Refused;
            CYBERMP_ERROR("refused: %s", ack.reason.c_str());
        }

        break;
    }

    case proto::Type::NotifyPlayerJoined:
    {
        proto::NotifyPlayerJoined joined;
        if (!proto::Decode(aData, joined))
        {
            m_malformed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        {
            std::scoped_lock lock(m_eventMutex);
            m_events.push_back({true, joined.playerId, joined.username});
        }

        CYBERMP_INFO("player #%u '%s' joined", joined.playerId, joined.username.c_str());
        break;
    }

    case proto::Type::NotifyPlayerLeft:
    {
        proto::NotifyPlayerLeft left;
        if (!proto::Decode(aData, left))
        {
            m_malformed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        {
            std::scoped_lock lock(m_remoteMutex);
            m_remote.erase(left.playerId);
        }

        {
            std::scoped_lock lock(m_eventMutex);
            m_events.push_back({false, left.playerId, {}});
        }

        CYBERMP_INFO("player #%u left", left.playerId);
        break;
    }

    case proto::Type::NotifyPlayerState:
    {
        proto::NotifyPlayerState notify;
        if (!proto::Decode(aData, notify))
        {
            m_malformed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        m_statesReceived.fetch_add(1, std::memory_order_relaxed);

        std::scoped_lock lock(m_remoteMutex);
        auto& snapshot = m_remote[notify.playerId];

        // Udp reorders, and an older snapshot would visibly rewind the body.
        if (snapshot.tick != 0 && notify.tick <= snapshot.tick)
        {
            m_statesOutOfOrder.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        snapshot.tick = notify.tick;
        snapshot.x = notify.position.x;
        snapshot.y = notify.position.y;
        snapshot.z = notify.position.z;
        snapshot.rotation = notify.rotation;

        break;
    }

    case proto::Type::Pong:
    {
        proto::Pong pong;
        if (!proto::Decode(aData, pong))
        {
            m_malformed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const auto rtt = static_cast<double>(NowMicros() - pong.sentAt) / 1000.0;
        m_lastRttMs.store(rtt, std::memory_order_relaxed);

        // The whole point of C1: work that belongs to the game crosses over here and
        // runs in the tick, never on this thread.
        m_tasks.Push([this] { m_appliedOnGameThread.fetch_add(1, std::memory_order_relaxed); });

        break;
    }

    default:
        m_malformed.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

bool NetClient::SendState(float aX, float aY, float aZ, float aRotation)
{
    if (m_state.load() != NetState::Connected)
    {
        return false;
    }

    proto::PlayerState state;
    state.tick = m_stateTick.fetch_add(1, std::memory_order_relaxed) + 1;
    state.position = {aX, aY, aZ};
    state.rotation = aRotation;

    std::vector<uint8_t> out;
    if (!proto::Encode(state, out))
    {
        return false;
    }

    m_sent.fetch_add(1, std::memory_order_relaxed);
    m_statesSent.fetch_add(1, std::memory_order_relaxed);

    return m_socket.SendTo(m_server, {out.data(), out.size()});
}

std::vector<RemoteEvent> NetClient::TakeEvents()
{
    std::vector<RemoteEvent> events;

    std::scoped_lock lock(m_eventMutex);
    events.swap(m_events);

    return events;
}

void NetClient::CopyRemoteSnapshots(std::vector<std::pair<uint32_t, RemoteSnapshot>>& aOut) const
{
    // Copies into the caller's buffer so the tick can reuse it and allocate nothing
    // once it has grown.
    aOut.clear();

    std::scoped_lock lock(m_remoteMutex);
    for (const auto& [playerId, snapshot] : m_remote)
    {
        aOut.emplace_back(playerId, snapshot);
    }
}

void NetClient::SetError(std::string aMessage)
{
    if (!aMessage.empty())
    {
        CYBERMP_ERROR("%s", aMessage.c_str());
    }

    std::scoped_lock lock(m_errorMutex);
    m_lastError = std::move(aMessage);
}

NetStats NetClient::GetStats() const
{
    NetStats stats;
    stats.state = m_state.load();
    stats.sent = m_sent.load();
    stats.received = m_received.load();
    stats.malformed = m_malformed.load();
    stats.helloAttempts = m_helloAttempts.load();
    stats.lastRttMs = m_lastRttMs.load();
    stats.appliedOnGameThread = m_appliedOnGameThread.load();
    stats.statesSent = m_statesSent.load();
    stats.statesReceived = m_statesReceived.load();
    stats.statesOutOfOrder = m_statesOutOfOrder.load();

    {
        std::scoped_lock lock(m_remoteMutex);
        stats.remoteCount = m_remote.size();
    }

    {
        std::scoped_lock lock(m_errorMutex);
        stats.lastError = m_lastError;
    }

    return stats;
}
} // namespace client

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Net.hpp"

namespace server
{
using PlayerId = uint32_t;

constexpr PlayerId kInvalidPlayer = 0;

struct Session
{
    PlayerId id{kInvalidPlayer};
    std::string username;
    net::Endpoint endpoint;
    uint64_t lastSeenMs{};
};

enum class JoinResult
{
    Accepted,
    AlreadyJoined, // same endpoint said hello twice, which udp makes likely
    VersionMismatch,
    NameTaken,
    Full,
};

const char* ToString(JoinResult aResult);

// Owns who is connected. Time is always passed in, never read from a clock, so
// timeouts can be tested without sleeping.
class SessionManager
{
public:
    SessionManager(size_t aMaxPlayers, uint64_t aTimeoutMs);

    JoinResult Join(const net::Endpoint& aFrom, const std::string& aUsername, uint32_t aVersion, uint64_t aNowMs,
                    PlayerId& aPlayerId);

    Session* Find(const net::Endpoint& aFrom);
    Session* Find(PlayerId aPlayerId);

    // Any traffic from a peer keeps it alive.
    bool Touch(const net::Endpoint& aFrom, uint64_t aNowMs);

    // Drops silent peers and hands back who went away, so the caller can announce it.
    std::vector<Session> CollectTimedOut(uint64_t aNowMs);

    bool Remove(PlayerId aPlayerId);

    size_t Count() const
    {
        return m_sessions.size();
    }

    const std::vector<Session>& Sessions() const
    {
        return m_sessions;
    }

private:
    size_t m_maxPlayers;
    uint64_t m_timeoutMs;
    PlayerId m_nextId{1}; // 0 stays reserved for "no player"
    std::vector<Session> m_sessions;
};
} // namespace server

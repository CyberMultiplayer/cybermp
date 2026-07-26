#include "Session.hpp"

#include <algorithm>

#include "Protocol.hpp"

namespace server
{
const char* ToString(JoinResult aResult)
{
    switch (aResult)
    {
    case JoinResult::Accepted:
        return "accepted";
    case JoinResult::AlreadyJoined:
        return "already joined";
    case JoinResult::VersionMismatch:
        return "protocol version mismatch";
    case JoinResult::NameTaken:
        return "name already taken";
    case JoinResult::Full:
        return "server full";
    }

    return "unknown";
}

SessionManager::SessionManager(size_t aMaxPlayers, uint64_t aTimeoutMs)
    : m_maxPlayers(aMaxPlayers)
    , m_timeoutMs(aTimeoutMs)
{
}

JoinResult SessionManager::Join(const net::Endpoint& aFrom, const std::string& aUsername, uint32_t aVersion,
                                uint64_t aNowMs, PlayerId& aPlayerId)
{
    aPlayerId = kInvalidPlayer;

    // Version first: refusing for any other reason would hide the real problem.
    if (aVersion != proto::kVersion)
    {
        return JoinResult::VersionMismatch;
    }

    // A dropped ack makes the client resend hello. Answer the same id instead of
    // creating a second session for the same peer.
    if (auto* existing = Find(aFrom))
    {
        existing->lastSeenMs = aNowMs;
        aPlayerId = existing->id;
        return JoinResult::AlreadyJoined;
    }

    if (aUsername.empty() || aUsername.size() > proto::kMaxStringSize)
    {
        return JoinResult::NameTaken;
    }

    const auto taken = std::any_of(m_sessions.begin(), m_sessions.end(),
                                   [&](const Session& aSession) { return aSession.username == aUsername; });
    if (taken)
    {
        return JoinResult::NameTaken;
    }

    if (m_sessions.size() >= m_maxPlayers)
    {
        return JoinResult::Full;
    }

    Session session;
    session.id = m_nextId++;
    session.username = aUsername;
    session.endpoint = aFrom;
    session.lastSeenMs = aNowMs;

    aPlayerId = session.id;
    m_sessions.push_back(std::move(session));

    return JoinResult::Accepted;
}

Session* SessionManager::Find(const net::Endpoint& aFrom)
{
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
                                 [&](const Session& aSession) { return aSession.endpoint == aFrom; });

    return it == m_sessions.end() ? nullptr : &*it;
}

Session* SessionManager::Find(PlayerId aPlayerId)
{
    if (aPlayerId == kInvalidPlayer)
    {
        return nullptr;
    }

    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
                                 [&](const Session& aSession) { return aSession.id == aPlayerId; });

    return it == m_sessions.end() ? nullptr : &*it;
}

bool SessionManager::Touch(const net::Endpoint& aFrom, uint64_t aNowMs)
{
    auto* session = Find(aFrom);
    if (!session)
    {
        return false;
    }

    session->lastSeenMs = aNowMs;
    return true;
}

std::optional<Session> SessionManager::TakeNextTimedOut(uint64_t aNowMs)
{
    const auto expired = [&](const Session& aSession) {
        // Guard against a now that went backwards rather than wrapping into a
        // gigantic unsigned difference.
        return aNowMs > aSession.lastSeenMs && aNowMs - aSession.lastSeenMs >= m_timeoutMs;
    };

    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(), expired);
    if (it == m_sessions.end())
    {
        return std::nullopt;
    }

    Session dropped = std::move(*it);
    m_sessions.erase(it);

    return dropped;
}

bool SessionManager::Remove(PlayerId aPlayerId)
{
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
                                 [&](const Session& aSession) { return aSession.id == aPlayerId; });

    if (it == m_sessions.end())
    {
        return false;
    }

    m_sessions.erase(it);
    return true;
}
} // namespace server

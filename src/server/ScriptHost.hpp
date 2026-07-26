#pragma once

#include <memory>
#include <vector>

#include "Script.hpp"
#include "Session.hpp"

namespace server
{
struct PendingKick
{
    PlayerId playerId{};
    std::string reason;
};

// Bridges SessionManager to the scripting boundary and dispatches events to the
// registered backends. Owns nothing about any language.
class ScriptHost
{
public:
    explicit ScriptHost(SessionManager& aSessions);
    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    void Add(std::unique_ptr<script::IBackend> aBackend);

    // Starts every backend. A backend that fails to start is dropped rather than
    // left half-alive.
    size_t StartAll();
    void StopAll();

    void OnPlayerJoin(PlayerId aPlayerId);
    void OnPlayerLeave(PlayerId aPlayerId);
    void OnTick(uint64_t aNowMs);

    // Drained by the server loop, so a script can't remove a session mid-iteration.
    std::vector<PendingKick> TakeKicks();

    // Test seam: lets a test observe what scripts logged.
    script::ILogger& Logger();

    size_t BackendCount() const
    {
        return m_backends.size();
    }

private:
    class Players;
    class Logger_;

    SessionManager& m_sessions;
    std::unique_ptr<Players> m_players;
    std::unique_ptr<Logger_> m_logger;
    std::vector<std::unique_ptr<script::IBackend>> m_backends;
    std::vector<PendingKick> m_kicks;
};

// Built-in backend, no dependency. Proves the wiring before Lua exists, and stays
// useful as a way to see events without writing a script.
std::unique_ptr<script::IBackend> MakeLogBackend();
} // namespace server

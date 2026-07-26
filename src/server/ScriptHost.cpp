#include "ScriptHost.hpp"

#include <cstdio>
#include <format>
#include <optional>

namespace server
{
namespace
{
// Thin view over a Session. Handed out per call and never stored by the host, so a
// script cannot hold a pointer past the lifetime of the session behind it.
class PlayerView final : public script::IPlayer
{
public:
    PlayerView(Session& aSession, std::vector<PendingKick>& aKicks)
        : m_session(aSession)
        , m_kicks(aKicks)
    {
    }

    PlayerId GetId() const override
    {
        return m_session.id;
    }

    std::string GetUsername() const override
    {
        return m_session.username;
    }

    std::string GetAddress() const override
    {
        return m_session.endpoint.ToString();
    }

    void Kick(std::string_view aReason) override
    {
        m_kicks.push_back({m_session.id, std::string(aReason)});
    }

private:
    Session& m_session;
    std::vector<PendingKick>& m_kicks;
};
} // namespace

class ScriptHost::Players final : public script::IPlayers
{
public:
    Players(SessionManager& aSessions, std::vector<PendingKick>& aKicks)
        : m_sessions(aSessions)
        , m_kicks(aKicks)
    {
    }

    size_t Count() const override
    {
        return m_sessions.Count();
    }

    script::IPlayer* Find(PlayerId aPlayerId) override
    {
        auto* session = m_sessions.Find(aPlayerId);
        if (!session)
        {
            return nullptr;
        }

        // Rebuilt in place each lookup: the view must never outlive its session.
        m_view.emplace(*session, m_kicks);
        return &*m_view;
    }

    std::vector<PlayerId> Ids() const override
    {
        std::vector<PlayerId> ids;
        ids.reserve(m_sessions.Count());

        for (const auto& session : m_sessions.Sessions())
        {
            ids.push_back(session.id);
        }

        return ids;
    }

private:
    SessionManager& m_sessions;
    std::vector<PendingKick>& m_kicks;
    std::optional<PlayerView> m_view;
};

class ScriptHost::Logger_ final : public script::ILogger
{
public:
    void Info(std::string_view aMessage) override
    {
        Emit("info", aMessage);
    }

    void Warn(std::string_view aMessage) override
    {
        Emit("warn", aMessage);
    }

    void Error(std::string_view aMessage) override
    {
        Emit("error", aMessage);
    }

    const std::vector<std::string>& Lines() const
    {
        return m_lines;
    }

private:
    void Emit(const char* aLevel, std::string_view aMessage)
    {
        auto line = std::format("[script:{}] {}", aLevel, aMessage);
        std::printf("%s\n", line.c_str());
        m_lines.push_back(std::move(line));
    }

    std::vector<std::string> m_lines;
};

ScriptHost::ScriptHost(SessionManager& aSessions)
    : m_sessions(aSessions)
    , m_logger(std::make_unique<Logger_>())
{
    m_players = std::make_unique<Players>(m_sessions, m_kicks);
}

ScriptHost::~ScriptHost()
{
    StopAll();
}

void ScriptHost::Add(std::unique_ptr<script::IBackend> aBackend)
{
    if (aBackend)
    {
        m_backends.push_back(std::move(aBackend));
    }
}

size_t ScriptHost::StartAll()
{
    script::Host host;
    host.players = m_players.get();
    host.logger = m_logger.get();

    for (auto it = m_backends.begin(); it != m_backends.end();)
    {
        if ((*it)->Start(host))
        {
            ++it;
        }
        else
        {
            // Better no backend than one that failed to load its scripts and would
            // silently do nothing.
            std::printf("!! backend '%s' failed to start, dropped\n", (*it)->Name());
            it = m_backends.erase(it);
        }
    }

    return m_backends.size();
}

void ScriptHost::StopAll()
{
    for (auto& backend : m_backends)
    {
        backend->Stop();
    }

    m_backends.clear();
}

void ScriptHost::OnPlayerJoin(PlayerId aPlayerId)
{
    for (auto& backend : m_backends)
    {
        backend->OnPlayerJoin(aPlayerId);
    }
}

void ScriptHost::OnPlayerLeave(PlayerId aPlayerId)
{
    for (auto& backend : m_backends)
    {
        backend->OnPlayerLeave(aPlayerId);
    }
}

void ScriptHost::OnTick(uint64_t aNowMs)
{
    for (auto& backend : m_backends)
    {
        backend->OnTick(aNowMs);
    }
}

std::vector<PendingKick> ScriptHost::TakeKicks()
{
    std::vector<PendingKick> kicks;
    kicks.swap(m_kicks);

    return kicks;
}

script::ILogger& ScriptHost::Logger()
{
    return *m_logger;
}

const std::vector<std::string>& ScriptHost::LogLines() const
{
    return m_logger->Lines();
}

namespace
{
class LogBackend final : public script::IBackend
{
public:
    const char* Name() const override
    {
        return "log";
    }

    bool Start(script::Host& aHost) override
    {
        m_host = aHost;
        m_host.logger->Info("log backend started");

        return true;
    }

    void Stop() override
    {
        if (m_host.logger)
        {
            m_host.logger->Info("log backend stopped");
        }
    }

    void OnPlayerJoin(PlayerId aPlayerId) override
    {
        auto* player = m_host.players->Find(aPlayerId);
        if (!player)
        {
            return;
        }

        m_host.logger->Info(std::format("join #{} '{}' from {}, {} online", player->GetId(), player->GetUsername(),
                                        player->GetAddress(), m_host.players->Count()));
    }

    void OnPlayerLeave(PlayerId aPlayerId) override
    {
        // The session is already gone by now, so only the id is meaningful here.
        m_host.logger->Info(std::format("leave #{}, {} online", aPlayerId, m_host.players->Count()));
    }

    void OnTick(uint64_t) override
    {
        // Nothing per tick: this is the hot path.
    }

private:
    script::Host m_host;
};
} // namespace

std::unique_ptr<script::IBackend> MakeLogBackend()
{
    return std::make_unique<LogBackend>();
}
} // namespace server

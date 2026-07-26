// The Lua scripting backend. sol2 lives only in this translation unit, so its
// compile cost is not paid across the project.
//
// Everything a script calls is wrapped: a bad script must never take the server
// down, so handlers run protected and errors are logged and swallowed.

#include "LuaBackend.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include <sol/sol.hpp>

namespace server
{
namespace
{
class LuaBackend final : public script::IBackend
{
public:
    explicit LuaBackend(std::filesystem::path aScriptDir)
        : m_scriptDir(std::move(aScriptDir))
    {
    }

    const char* Name() const override
    {
        return "lua";
    }

    bool Start(script::Host& aHost) override
    {
        m_host = aHost;

        // Deliberately not the full standard library. No io, no os, no package:
        // scripts are the server owner's, but there is no reason to hand them file
        // and process access before anyone asks for it.
        m_lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math,
                             sol::lib::coroutine);

        BindApi();

        return LoadScripts();
    }

    void Stop() override
    {
        m_handlers.clear();
    }

    void OnPlayerJoin(PlayerId aPlayerId) override
    {
        Fire("playerJoin", aPlayerId);
    }

    void OnPlayerLeave(PlayerId aPlayerId) override
    {
        Fire("playerLeave", aPlayerId);
    }

    void OnTick(uint64_t aNowMs) override
    {
        // Only pay for a call if a script actually asked for ticks.
        if (m_handlers.contains("tick"))
        {
            Fire("tick", aNowMs);
        }
    }

private:
    void BindApi()
    {
        auto api = m_lua.create_named_table("cybermp");

        api.set_function("log", [this](sol::object aMessage) {
            m_host.logger->Info(Describe(aMessage));
        });

        api.set_function("warn", [this](sol::object aMessage) {
            m_host.logger->Warn(Describe(aMessage));
        });

        // Takes an object rather than a function so a wrong type produces a message a
        // script author can read, instead of sol2 printing its own template names.
        api.set_function("on", [this](std::string aEvent, sol::object aHandler) {
            if (!aHandler.is<sol::protected_function>())
            {
                m_host.logger->Error(std::format("cybermp.on('{}'): second argument must be a function, got {}",
                                                 aEvent, sol::type_name(m_lua.lua_state(), aHandler.get_type())));
                return;
            }

            m_handlers[aEvent].push_back(aHandler.as<sol::protected_function>());
        });

        auto players = api.create_named("players");

        players.set_function("count", [this] { return m_host.players->Count(); });

        players.set_function("ids", [this] {
            // A Lua array is 1-based, and sol2 won't do that for us.
            auto table = m_lua.create_table();
            for (const auto id : m_host.players->Ids())
            {
                table.add(id);
            }

            return table;
        });

        players.set_function("find", [this](PlayerId aPlayerId) -> sol::object {
            auto* player = m_host.players->Find(aPlayerId);
            if (!player)
            {
                // nil rather than an empty table, so `if p then` behaves as expected.
                return sol::lua_nil;
            }

            auto table = m_lua.create_table();
            table["id"] = player->GetId();
            table["name"] = player->GetUsername();
            table["address"] = player->GetAddress();

            // Captures the id, not the pointer: the view behind it is transient.
            table.set_function("kick", [this, id = aPlayerId](sol::optional<std::string> aReason) {
                if (auto* target = m_host.players->Find(id))
                {
                    target->Kick(aReason.value_or("kicked by script"));
                }
            });

            return table;
        });
    }

    bool LoadScripts()
    {
        std::error_code error;
        if (!std::filesystem::exists(m_scriptDir, error))
        {
            m_host.logger->Info(std::format("no script directory at '{}'", m_scriptDir.string()));
            return true; // a server with no scripts is fine
        }

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(m_scriptDir, error))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".lua")
            {
                files.push_back(entry.path());
            }
        }

        // Directory order is not guaranteed, and load order changes behaviour.
        std::sort(files.begin(), files.end());

        size_t loaded = 0;
        for (const auto& file : files)
        {
            const auto result = m_lua.safe_script_file(file.string(), sol::script_pass_on_error);
            if (result.valid())
            {
                ++loaded;
                m_host.logger->Info(std::format("loaded {}", file.filename().string()));
            }
            else
            {
                // One broken script must not stop the others from loading.
                const sol::error err = result;
                m_host.logger->Error(std::format("{}: {}", file.filename().string(), err.what()));
            }
        }

        m_host.logger->Info(std::format("{} of {} script(s) loaded", loaded, files.size()));

        return true;
    }

    template<typename... Args>
    void Fire(const char* aEvent, Args&&... aArgs)
    {
        const auto it = m_handlers.find(aEvent);
        if (it == m_handlers.end())
        {
            return;
        }

        for (auto& handler : it->second)
        {
            const auto result = handler(aArgs...);
            if (!result.valid())
            {
                const sol::error err = result;
                m_host.logger->Error(std::format("{} handler: {}", aEvent, err.what()));
            }
        }
    }

    std::string Describe(const sol::object& aValue)
    {
        // Scripts pass numbers to log() as often as strings.
        if (aValue.is<std::string>())
        {
            return aValue.as<std::string>();
        }

        if (aValue.is<double>())
        {
            return std::format("{}", aValue.as<double>());
        }

        if (aValue.is<bool>())
        {
            return aValue.as<bool>() ? "true" : "false";
        }

        return sol::type_name(m_lua.lua_state(), aValue.get_type());
    }

    std::filesystem::path m_scriptDir;
    script::Host m_host;
    sol::state m_lua;
    std::unordered_map<std::string, std::vector<sol::protected_function>> m_handlers;
};
} // namespace

std::unique_ptr<script::IBackend> MakeLuaBackend(std::filesystem::path aScriptDir)
{
    return std::make_unique<LuaBackend>(std::move(aScriptDir));
}
} // namespace server

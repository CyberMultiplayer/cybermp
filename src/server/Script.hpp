#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Session.hpp"

// The boundary between the server core and whatever language scripts are written in.
// Deliberately flat and free of any language type: adding Lua, or replacing it, must
// not touch anything on the core side.
namespace script
{
class IPlayer
{
public:
    virtual ~IPlayer() = default;

    virtual server::PlayerId GetId() const = 0;
    virtual std::string GetUsername() const = 0;
    virtual std::string GetAddress() const = 0;

    // Queued rather than immediate: a script must not be able to invalidate the
    // session the core is currently iterating over.
    virtual void Kick(std::string_view aReason) = 0;
};

class IPlayers
{
public:
    virtual ~IPlayers() = default;

    virtual size_t Count() const = 0;
    virtual IPlayer* Find(server::PlayerId aPlayerId) = 0;
    virtual std::vector<server::PlayerId> Ids() const = 0;
};

class ILogger
{
public:
    virtual ~ILogger() = default;

    virtual void Info(std::string_view aMessage) = 0;
    virtual void Warn(std::string_view aMessage) = 0;
    virtual void Error(std::string_view aMessage) = 0;
};

// Everything a backend is allowed to reach.
struct Host
{
    IPlayers* players{};
    ILogger* logger{};
};

// What a scripting backend implements. One process can run several.
class IBackend
{
public:
    virtual ~IBackend() = default;

    virtual const char* Name() const = 0;

    // False aborts startup: a backend that cannot load its scripts must not run
    // silently doing nothing.
    virtual bool Start(Host& aHost) = 0;
    virtual void Stop() = 0;

    virtual void OnPlayerJoin(server::PlayerId aPlayerId) = 0;
    virtual void OnPlayerLeave(server::PlayerId aPlayerId) = 0;
    virtual void OnTick(uint64_t aNowMs) = 0;
};
} // namespace script

#include "WorldSystem.hpp"

#include <format>

// RedLib.hpp only pulls a handful of generated headers, these aren't among them.
#include <RED4ext/Scripting/Natives/Generated/game/Object.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/PlayerSystem.hpp>

#include "Log.hpp"

namespace
{
// PlayerSystem has no typed methods in the sdk, so go through the rtti.
Red::Handle<Red::GameObject> GetLocalPlayer()
{
    auto* playerSystem = Red::GetGameSystem<Red::gamePlayerSystem>();
    if (!playerSystem)
    {
        return {};
    }

    Red::Handle<Red::GameObject> player;
    Red::CallVirtual(playerSystem, "GetLocalPlayerControlledGameObject", player);

    return player;
}
} // namespace

void WorldSystem::OnWorldAttached(Red::world::RuntimeScene*)
{
    m_worldAttached = true;
    CYBERMP_INFO("world attached");
}

void WorldSystem::OnWorldDetached(Red::world::RuntimeScene*)
{
    m_worldAttached = false;
    CYBERMP_INFO("world detached");
}

bool WorldSystem::IsWorldReady()
{
    auto* self = Red::GetGameSystem<WorldSystem>();

    return self && self->m_worldAttached;
}

Red::Vector4 WorldSystem::GetPlayerPosition()
{
    // Zeroed Vector4 when there's no player, e.g. at the main menu.
    auto player = GetLocalPlayer();
    if (!player)
    {
        return {};
    }

    Red::Vector4 position;
    Red::CallVirtual(player.instance, "GetWorldPosition", position);

    return position;
}

// Debug helper, so keep the log here rather than in GetPlayerPosition which will
// end up on the per-frame path. CET buffers its own console log, ours is reliable.
Red::CString WorldSystem::GetPlayerPositionText()
{
    const auto position = GetPlayerPosition();
    const auto text = std::format("{:.2f}, {:.2f}, {:.2f}", position.X, position.Y, position.Z);

    CYBERMP_INFO("player position: %s", text.c_str());

    return Red::CString(text.c_str());
}

RTTI_DEFINE_CLASS(WorldSystem, "Cybermp.WorldSystem", {
    RTTI_METHOD(IsWorldReady);
    RTTI_METHOD(GetPlayerPosition);
    RTTI_METHOD(GetPlayerPositionText);
});

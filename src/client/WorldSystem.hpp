#pragma once

#include <RedLib.hpp>

// Lifecycle anchor. The game builds and attaches this once a world is loaded,
// which is the earliest point where the player and other systems actually exist.
// Deriving from IGameSystem is enough -- RedLib registers it in the game instance.
class WorldSystem : public Red::IGameSystem
{
public:
    // Static entry points so the CET console can reach them with GetSingleton,
    // before we have a redscript accessor on GameInstance.
    static bool IsWorldReady();
    static Red::Vector4 GetPlayerPosition();
    static Red::CString GetPlayerPositionText();

    // Spawn on the player. Needs Codeware -- the game has no generic spawn api.
    static Red::CString SpawnProp();
    static Red::CString SpawnNpc(const Red::CString& aRecord);

private:
    void OnWorldAttached(Red::world::RuntimeScene* aScene) override;
    void OnWorldDetached(Red::world::RuntimeScene* aScene) override;

    bool m_worldAttached{};

    RTTI_IMPL_TYPEINFO(WorldSystem);
    RTTI_IMPL_ALLOCATOR();
};

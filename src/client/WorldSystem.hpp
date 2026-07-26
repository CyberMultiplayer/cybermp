#pragma once

#include <mutex>
#include <vector>

#include <RedLib.hpp>

// RedLib.hpp doesn't pull the generated headers we need in this interface.
#include <RED4ext/Scripting/Natives/Generated/ent/EntityID.hpp>

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

    static uint32_t GetSpawnedCount();
    static uint32_t DespawnAll();

    // Called by the spawn helpers once the game accepted a request.
    static void TrackSpawned(Red::EntityID aEntityID);

private:
    void OnWorldAttached(Red::world::RuntimeScene* aScene) override;
    void OnWorldDetached(Red::world::RuntimeScene* aScene) override;

    bool m_worldAttached{};

    // We own what we spawn rather than leaning on the game's tags: the same registry
    // becomes the serverId -> EntityID map once puppets are networked.
    // Guarded because engine callbacks reach us from several threads.
    std::vector<Red::EntityID> m_spawned;
    std::mutex m_spawnedMutex;

    RTTI_IMPL_TYPEINFO(WorldSystem);
    RTTI_IMPL_ALLOCATOR();
};

#include "WorldSystem.hpp"

#include <format>

// RedLib.hpp only pulls a handful of generated headers, these aren't among them.
#include <RED4ext/Scripting/Natives/Generated/game/Object.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/PlayerSystem.hpp>
#include <RED4ext/Scripting/Natives/Generated/red/ResourceReferenceScriptToken.hpp>

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

// Builds a spec placed on the player. Caller fills in templatePath or recordID.
Red::Handle<Red::IScriptable> MakeSpecAtPlayer()
{
    auto spec = Red::MakeScriptedHandle<Red::IScriptable>("DynamicEntitySpec");
    if (!spec)
    {
        CYBERMP_ERROR("DynamicEntitySpec not found -- is Codeware installed?");
        return {};
    }

    auto* position = Red::GetPropertyPtr<Red::Vector4>(spec.instance, "position");
    auto* persistState = Red::GetPropertyPtr<bool>(spec.instance, "persistState");
    auto* persistSpawn = Red::GetPropertyPtr<bool>(spec.instance, "persistSpawn");

    if (!position || !persistState || !persistSpawn)
    {
        CYBERMP_ERROR("DynamicEntitySpec layout changed");
        return {};
    }

    *position = WorldSystem::GetPlayerPosition();

    // The server owns what we spawn, never the save file.
    *persistState = false;
    *persistSpawn = false;

    return spec;
}

bool SubmitSpec(const Red::Handle<Red::IScriptable>& aSpec, Red::EntityID& aEntityID)
{
    Red::Handle<Red::IScriptable> system;
    if (!Red::CallStatic("ScriptGameInstance", "GetDynamicEntitySystem", system) || !system)
    {
        CYBERMP_ERROR("GetDynamicEntitySystem failed");
        return false;
    }

    if (!Red::CallVirtual(system.instance, "CreateEntity", aEntityID, aSpec))
    {
        CYBERMP_ERROR("CreateEntity call failed");
        return false;
    }

    // An unknown record or template doesn't fail the call, it hands back a null id.
    // Without this check a typo looks exactly like a successful spawn.
    if (!aEntityID.IsDefined())
    {
        CYBERMP_ERROR("CreateEntity returned a null id -- unknown record or template?");
        return false;
    }

    return true;
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

    // Entities die with the world, so keeping their ids would only leak stale ones.
    {
        std::scoped_lock lock(m_spawnedMutex);
        m_spawned.clear();
    }

    CYBERMP_INFO("world detached");
}

void WorldSystem::TrackSpawned(Red::EntityID aEntityID)
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return;
    }

    std::scoped_lock lock(self->m_spawnedMutex);
    self->m_spawned.push_back(aEntityID);
}

uint32_t WorldSystem::GetSpawnedCount()
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return 0;
    }

    std::scoped_lock lock(self->m_spawnedMutex);
    return static_cast<uint32_t>(self->m_spawned.size());
}

uint32_t WorldSystem::DespawnAll()
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return 0;
    }

    std::vector<Red::EntityID> pending;
    {
        std::scoped_lock lock(self->m_spawnedMutex);
        pending.swap(self->m_spawned);
    }

    Red::Handle<Red::IScriptable> system;
    if (!Red::CallStatic("ScriptGameInstance", "GetDynamicEntitySystem", system) || !system)
    {
        CYBERMP_ERROR("GetDynamicEntitySystem failed, %zu ids dropped", pending.size());
        return 0;
    }

    // Signature from Codeware's own declaration in Codeware.Global.reds:
    //   public native func DeleteEntity(id: EntityID) -> Bool
    uint32_t removed = 0;
    for (const auto& entityID : pending)
    {
        bool deleted = false;
        if (Red::CallVirtual(system.instance, "DeleteEntity", deleted, entityID) && deleted)
        {
            ++removed;
        }
        else
        {
            CYBERMP_ERROR("DeleteEntity failed for id %llu", entityID.hash);
        }
    }

    CYBERMP_INFO("despawned %u of %zu", removed, pending.size());

    return removed;
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

// Ported from the lua validated in the CET console, so the rtti names below are known good.
// Every step logs: a silent failure here would be untraceable otherwise.
Red::CString WorldSystem::SpawnProp()
{
    constexpr auto kTemplate = "base\\gameplay\\devices\\ladder\\appearances\\10m_gen_default.ent";

    auto spec = MakeSpecAtPlayer();
    if (!spec)
    {
        return "spec failed";
    }

    auto* templatePath = Red::GetPropertyPtr<Red::ResRef>(spec.instance, "templatePath");
    if (!templatePath)
    {
        CYBERMP_ERROR("templatePath property missing");
        return "no templatePath";
    }

    templatePath->resource = Red::ResourcePath(kTemplate);

    Red::EntityID entityID;
    if (!SubmitSpec(spec, entityID))
    {
        return "CreateEntity failed";
    }

    // Spawning is async: a valid id here only means the request was accepted.
    TrackSpawned(entityID);
    CYBERMP_INFO("prop spawn requested, id %llu", entityID.hash);

    return Red::CString(std::format("spawn requested, id {}", entityID.hash).c_str());
}

// Takes the record name so different characters can be tried from the console
// without rebuilding. Default is the one from Codeware's own docs.
Red::CString WorldSystem::SpawnNpc(const Red::CString& aRecord)
{
    constexpr auto kDefaultRecord = "Character.spr_animals_bouncer1_ranged1_omaha_mb";

    const char* record = aRecord.Length() > 0 ? aRecord.c_str() : kDefaultRecord;

    auto spec = MakeSpecAtPlayer();
    if (!spec)
    {
        return "spec failed";
    }

    auto* recordID = Red::GetPropertyPtr<Red::TweakDBID>(spec.instance, "recordID");
    if (!recordID)
    {
        CYBERMP_ERROR("recordID property missing");
        return "no recordID";
    }

    *recordID = Red::TweakDBID(record);

    Red::EntityID entityID;
    if (!SubmitSpec(spec, entityID))
    {
        return Red::CString(std::format("spawn refused, unknown record '{}'?", record).c_str());
    }

    TrackSpawned(entityID);
    CYBERMP_INFO("npc spawn requested, record '%s', id %llu", record, entityID.hash);

    return Red::CString(std::format("spawn requested, record {}, id {}", record, entityID.hash).c_str());
}

RTTI_DEFINE_CLASS(WorldSystem, "Cybermp.WorldSystem", {
    RTTI_METHOD(IsWorldReady);
    RTTI_METHOD(GetPlayerPosition);
    RTTI_METHOD(GetPlayerPositionText);
    RTTI_METHOD(SpawnProp);
    RTTI_METHOD(SpawnNpc);
    RTTI_METHOD(GetSpawnedCount);
    RTTI_METHOD(DespawnAll);
});

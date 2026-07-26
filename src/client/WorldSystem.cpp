#include "WorldSystem.hpp"

#include <chrono>
#include <cmath>
#include <format>
#include <thread>

// RedLib.hpp only pulls a handful of generated headers, and these keep not being
// among them. Fifth time: worth a project header that groups what we actually use.
#include <RED4ext/Scripting/Natives/Generated/Quaternion.hpp>
#include <RED4ext/Scripting/Natives/Generated/WorldTransform.hpp>
#include <RED4ext/Scripting/Natives/Generated/ent/Entity.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/Object.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/PlayerSystem.hpp>
#include <RED4ext/Scripting/Natives/Generated/red/ResourceReferenceScriptToken.hpp>
#include <RED4ext/Scripting/Natives/WorldPosition.hpp>
#include <RED4ext/Scripting/Natives/moveComponent.hpp>

#include "Log.hpp"
#include "NetClient.hpp"
#include "Plugin.hpp"

namespace
{
size_t CurrentThreadId()
{
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

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

namespace
{
constexpr uint64_t kStateSendIntervalMs = 66; // ~15 Hz, not per frame

// Render the past rather than the present. Snapshots arrive every ~66 ms, so
// holding one and a half intervals means there is almost always a later sample to
// interpolate towards instead of extrapolating into nothing.
constexpr uint64_t kInterpolationDelayMs = 100;
constexpr size_t kMaxSamples = 8;

float LerpAngleDegrees(float aFrom, float aTo, float aT)
{
    // Shortest way round, so 350 -> 10 turns 20 degrees and not 340.
    auto delta = std::fmod(aTo - aFrom + 540.0f, 360.0f) - 180.0f;
    return aFrom + delta * aT;
}

// Yaw only: a standing body needs no pitch or roll, and sending three angles for
// one useful one would be waste on an unreliable channel.
Red::Quaternion YawToQuaternion(float aDegrees)
{
    const auto radians = aDegrees * 0.017453292519943295f; // pi / 180
    const auto half = radians * 0.5f;

    Red::Quaternion quaternion;
    quaternion.i = 0.0f;
    quaternion.j = 0.0f;
    quaternion.k = std::sin(half); // Z is up in REDengine, so yaw turns around k
    quaternion.r = std::cos(half);

    return quaternion;
}

bool WriteTransform(const Red::Handle<Red::Entity>& aEntity, const client::RemoteSnapshot& aSnapshot)
{
    Red::WorldTransform transform;
    transform.Position = Red::WorldPosition(Red::Vector4(aSnapshot.x, aSnapshot.y, aSnapshot.z, 1.0f));
    transform.Orientation = YawToQuaternion(aSnapshot.rotation);

    // Codeware adds SetWorldTransform to Entity; the base game does not expose it.
    return Red::CallVirtual(aEntity.instance, "SetWorldTransform", transform);
}

// An npc carries moveComponent, moveMotionPlannerComponent and movePoliciesComponent
// (confirmed by dumping a spawned body's 143 components). The motion planner owns the
// position, which is why writing only the entity transform is not honoured until the
// game wakes the npc up.
bool WriteMover(const Red::Handle<Red::Entity>& aEntity, const client::RemoteSnapshot& aSnapshot,
                const Red::Vector3& aVelocity)
{
    Red::Handle<Red::IComponent> component;
    if (!Red::CallVirtual(aEntity.instance, "FindComponentByType", component, Red::CName("moveComponent")) ||
        !component)
    {
        return false;
    }

    // Raw field writes. The sdk asserts these offsets, so a layout change after a
    // game patch breaks the build rather than corrupting memory silently.
    auto* mover = reinterpret_cast<Red::moveComponent*>(component.instance);

    const Red::Vector4 position(aSnapshot.x, aSnapshot.y, aSnapshot.z, 1.0f);
    mover->position = position;
    mover->worldTransform.Position = Red::WorldPosition(position);
    mover->worldTransform.Orientation = YawToQuaternion(aSnapshot.rotation);

    // The animation system reads speed to pick idle, walk or run. Without it a moving
    // body would slide along the ground.
    mover->speed = aVelocity;

    return true;
}

// The game's own way of relocating something. Goes through the systems that a raw
// field write bypasses, which is exactly what an ai driven npc keeps undoing.
bool WriteTeleport(const Red::Handle<Red::Entity>& aEntity, const client::RemoteSnapshot& aSnapshot)
{
    Red::Handle<Red::IScriptable> facility;
    if (!Red::CallStatic("ScriptGameInstance", "GetTeleportationFacility", facility) || !facility)
    {
        return false;
    }

    const Red::Vector4 position(aSnapshot.x, aSnapshot.y, aSnapshot.z, 1.0f);

    Red::EulerAngles rotation;
    rotation.Roll = 0.0f;
    rotation.Pitch = 0.0f;
    rotation.Yaw = aSnapshot.rotation;

    return Red::CallVirtual(facility.instance, "Teleport", aEntity, position, rotation);
}
} // namespace

namespace
{
// Remote bodies are not in the m_spawned registry: they have their own lifetime,
// tied to a player rather than to a console command.
bool DestroyBodyEntity(Red::EntityID aEntityId)
{
    if (!aEntityId.IsDefined())
    {
        return false;
    }

    Red::Handle<Red::IScriptable> system;
    if (!Red::CallStatic("ScriptGameInstance", "GetDynamicEntitySystem", system) || !system)
    {
        return false;
    }

    bool deleted = false;
    return Red::CallVirtual(system.instance, "DeleteEntity", deleted, aEntityId) && deleted;
}
} // namespace

void WorldSystem::PumpRemotePlayers(uint64_t aNowMs)
{
    auto& net = Plugin::Net();

    // Events first: a body has to exist before a snapshot can move it.
    for (const auto& event : net.TakeEvents())
    {
        if (event.joined)
        {
            m_bodies.try_emplace(event.playerId, RemoteBody{});
        }
        else if (const auto it = m_bodies.find(event.playerId); it != m_bodies.end())
        {
            DestroyBodyEntity(it->second.entityId);
            m_bodies.erase(it);
        }
    }

    if (m_bodies.empty())
    {
        return;
    }

    net.CopyRemoteSnapshots(m_snapshotBuffer);

    Red::Handle<Red::IScriptable> entitySystem;
    const auto haveSystem = Red::CallStatic("ScriptGameInstance", "GetDynamicEntitySystem", entitySystem) &&
                            static_cast<bool>(entitySystem);

    for (const auto& [playerId, snapshot] : m_snapshotBuffer)
    {
        const auto it = m_bodies.find(playerId);
        if (it == m_bodies.end() || !haveSystem)
        {
            continue;
        }

        auto& body = it->second;

        // First snapshot is what tells us where to put the body, so spawning waits
        // for it rather than dropping an npc at the origin.
        if (!body.spawnRequested)
        {
            body.spawnRequested = true;

            auto spec = Red::MakeScriptedHandle<Red::IScriptable>("DynamicEntitySpec");
            if (!spec)
            {
                continue;
            }

            auto* position = Red::GetPropertyPtr<Red::Vector4>(spec.instance, "position");
            auto* persistState = Red::GetPropertyPtr<bool>(spec.instance, "persistState");
            auto* persistSpawn = Red::GetPropertyPtr<bool>(spec.instance, "persistSpawn");

            if (!position || !persistState || !persistSpawn)
            {
                continue;
            }

            if (m_bodyKind.load(std::memory_order_relaxed) == 1)
            {
                // A prop carries no ai, so nothing overwrites what we write.
                auto* templatePath = Red::GetPropertyPtr<Red::ResRef>(spec.instance, "templatePath");
                if (!templatePath)
                {
                    continue;
                }

                templatePath->resource =
                    Red::ResourcePath("base\\gameplay\\devices\\ladder\\appearances\\10m_gen_default.ent");
            }
            else
            {
                auto* recordID = Red::GetPropertyPtr<Red::TweakDBID>(spec.instance, "recordID");
                if (!recordID)
                {
                    continue;
                }

                *recordID = Red::TweakDBID("Character.spr_animals_bouncer1_ranged1_omaha_mb");
            }

            *position = Red::Vector4(snapshot.x, snapshot.y, snapshot.z, 1.0f);
            *persistState = false;
            *persistSpawn = false;

            Red::EntityID entityId;
            if (Red::CallVirtual(entitySystem.instance, "CreateEntity", entityId, spec) && entityId.IsDefined())
            {
                body.entityId = entityId;
                CYBERMP_INFO("body for player #%u requested, id %llu", playerId, entityId.hash);
            }

            continue; // spawning is async, so nothing to move yet
        }

        if (!body.entityId.IsDefined())
        {
            continue;
        }

        Red::Handle<Red::Entity> entity;
        if (!Red::CallVirtual(entitySystem.instance, "GetEntity", entity, body.entityId) || !entity)
        {
            continue; // still spawning
        }

        // A new snapshot joins the history, stamped with local arrival time.
        if (snapshot.tick > body.lastTick)
        {
            body.lastTick = snapshot.tick;
            body.samples.push_back({aNowMs, snapshot.x, snapshot.y, snapshot.z, snapshot.rotation});

            if (body.samples.size() > kMaxSamples)
            {
                body.samples.erase(body.samples.begin());
            }
        }

        if (body.samples.empty())
        {
            continue;
        }

        client::RemoteSnapshot target = snapshot;
        Red::Vector3 velocity{};

        if (m_interpolate.load(std::memory_order_relaxed))
        {
            const auto renderMs = aNowMs > kInterpolationDelayMs ? aNowMs - kInterpolationDelayMs : 0;

            const Sample* before = nullptr;
            const Sample* after = nullptr;

            for (const auto& sample : body.samples)
            {
                if (sample.localMs <= renderMs)
                {
                    before = &sample;
                }
                else
                {
                    after = &sample;
                    break;
                }
            }

            if (before && after && after->localMs > before->localMs)
            {
                const auto span = static_cast<float>(after->localMs - before->localMs);
                const auto t = static_cast<float>(renderMs - before->localMs) / span;

                target.x = before->x + (after->x - before->x) * t;
                target.y = before->y + (after->y - before->y) * t;
                target.z = before->z + (after->z - before->z) * t;
                target.rotation = LerpAngleDegrees(before->rotation, after->rotation, t);

                const auto seconds = span / 1000.0f;
                velocity.X = (after->x - before->x) / seconds;
                velocity.Y = (after->y - before->y) / seconds;
                velocity.Z = (after->z - before->z) / seconds;
            }
            else
            {
                // No later sample to aim at: hold the newest rather than extrapolate
                // into a position that never existed.
                const auto& newest = body.samples.back();
                target.x = newest.x;
                target.y = newest.y;
                target.z = newest.z;
                target.rotation = newest.rotation;

                m_starvedFrames.fetch_add(1, std::memory_order_relaxed);
            }
        }

        const auto& snapshotToApply = target;
        const auto mode = m_moveMode.load(std::memory_order_relaxed);

        if (mode == 0 || mode == 2)
        {
            if (WriteTransform(entity, snapshotToApply))
            {
                m_transformWrites.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (mode == 1 || mode == 2)
        {
            if (WriteMover(entity, snapshotToApply, velocity))
            {
                m_moverWrites.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                m_moverMissing.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (mode == 3)
        {
            if (WriteTeleport(entity, snapshotToApply))
            {
                m_transformWrites.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                m_moverMissing.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void WorldSystem::SendLocalState(uint64_t aNowMs)
{
    // Rate limited on purpose: 60 Hz of snapshots would be bandwidth spent on
    // detail no interpolation can use.
    if (aNowMs - m_lastStateSentMs < kStateSendIntervalMs)
    {
        return;
    }

    const auto position = GetPlayerPosition();
    if (position.X == 0.0f && position.Y == 0.0f && position.Z == 0.0f)
    {
        return; // no player yet, nothing worth sending
    }

    m_lastStateSentMs = aNowMs;

    // Rotation stays 0 until C3: a body that faces the wrong way is a smaller
    // problem than one that is not there.
    Plugin::Net().SendState(position.X, position.Y, position.Z, 0.0f);
}

// The engine has dedicated multiplayer tick groups left over from the cancelled
// online mode. Registering on both FrameBegin and one of them tells us whether the
// multiplayer ones still fire in retail -- if they do, they are semantically exactly
// what we want: capture our state late, apply received state early.
void WorldSystem::OnRegisterUpdates(Red::UpdateRegistrar* aRegistrar)
{
    IGameSystem::OnRegisterUpdates(aRegistrar);

    if (!aRegistrar)
    {
        return;
    }

    aRegistrar->RegisterUpdate(Red::UpdateTickGroup::FrameBegin, this, "cybermp/FrameBegin",
                               Red::GroupUpdateCallback(this, &WorldSystem::OnFrameBegin));

    aRegistrar->RegisterUpdate(Red::UpdateTickGroup::Multiplayer_UpdateStateSnapshots, this, "cybermp/MpUpdate",
                               Red::GroupUpdateCallback(this, &WorldSystem::OnMultiplayerUpdate));

    CYBERMP_INFO("registered frame updates");
}

void WorldSystem::OnFrameBegin(Red::FrameInfo& aFrame, Red::JobQueue&)
{
    RunTick(aFrame, m_frameBeginTicks);
}

// Remote bodies are driven from the engine's own multiplayer group, which is what
// CDPR meant it for: apply received state early in the frame.
void WorldSystem::OnMultiplayerUpdate(Red::FrameInfo& aFrame, Red::JobQueue&)
{
    RunTick(aFrame, m_multiplayerTicks);

    const auto nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());

    PumpRemotePlayers(nowMs);
    SendLocalState(nowMs);
}

// No logging and no allocation in here: this is the hottest path in the plugin.
void WorldSystem::RunTick(Red::FrameInfo& aFrame, std::atomic_uint64_t& aCounter)
{
    const auto started = std::chrono::steady_clock::now();

    const auto thisThread = CurrentThreadId();

    aCounter.fetch_add(1, std::memory_order_relaxed);
    m_lastDeltaTime.store(aFrame.deltaTime, std::memory_order_relaxed);

    // Counting the changes is how we show the pool rotates rather than guessing.
    if (m_tickThreadId.exchange(thisThread, std::memory_order_relaxed) != thisThread)
    {
        m_tickThreadChanges.fetch_add(1, std::memory_order_relaxed);
    }

    m_drainThreadId.store(thisThread, std::memory_order_relaxed);

    // Bounded on purpose: a burst off the network must not be allowed to stall a frame.
    const auto ran = Plugin::Tasks().Drain(64);
    if (ran > 0)
    {
        m_tasksRun.fetch_add(ran, std::memory_order_relaxed);
    }

    const auto micros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());

    // Plain max rather than an average: a single long frame is what players notice.
    auto previous = m_maxTickMicros.load(std::memory_order_relaxed);
    while (micros > previous && !m_maxTickMicros.compare_exchange_weak(previous, micros, std::memory_order_relaxed))
    {
    }
}

Red::CString WorldSystem::GetTickStats()
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return "no world system";
    }

    const auto text = std::format(
        "frameBegin={} mpUpdate={} tasksRun={} pending={} dt={:.4f} maxTick={}us "
        "tickThreadChanges={} producerThread={} tasksAlwaysOnDrainThread={}",
        self->m_frameBeginTicks.load(), self->m_multiplayerTicks.load(), self->m_tasksRun.load(),
        Plugin::Tasks().Pending(), self->m_lastDeltaTime.load(), self->m_maxTickMicros.load(),
        self->m_tickThreadChanges.load(), self->m_producerThreadId.load(),
        self->m_taskRanOffDrainThread.load() ? "no" : "yes");

    CYBERMP_INFO("%s", text.c_str());

    return Red::CString(text.c_str());
}

Red::CString WorldSystem::PostTasksFromThread(int32_t aCount)
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return "no world system";
    }

    if (aCount <= 0 || aCount > 10000)
    {
        return "count must be 1..10000";
    }

    // Replaces any previous producer, which joins here rather than leaking.
    self->m_producer = std::jthread([self, aCount] {
        self->m_producerThreadId.store(CurrentThreadId(), std::memory_order_relaxed);

        for (int32_t i = 0; i < aCount; ++i)
        {
            // The invariant that matters: a task body must run on the thread that is
            // draining it, never on the one that queued it.
            Plugin::Tasks().Push([self] {
                if (CurrentThreadId() != self->m_drainThreadId.load(std::memory_order_relaxed))
                {
                    self->m_taskRanOffDrainThread.store(true, std::memory_order_relaxed);
                }
            });
        }
    });

    return Red::CString(std::format("queued {} task(s) from a worker thread", aCount).c_str());
}

Red::CString WorldSystem::Connect(uint32_t aPort, const Red::CString& aUsername)
{
    if (aPort == 0 || aPort > 65535)
    {
        return "port must be 1..65535";
    }

    const std::string username = aUsername.Length() > 0 ? aUsername.c_str() : "player";

    if (!Plugin::Net().Connect("127.0.0.1", static_cast<uint16_t>(aPort), username))
    {
        return Red::CString(std::format("connect failed: {}", Plugin::Net().GetStats().lastError).c_str());
    }

    return Red::CString(std::format("connecting to 127.0.0.1:{} as '{}'", aPort, username).c_str());
}

Red::CString WorldSystem::Disconnect()
{
    Plugin::Net().Disconnect();

    return "disconnected";
}

Red::CString WorldSystem::GetNetStats()
{
    const auto stats = Plugin::Net().GetStats();

    const auto text = std::format("state={} sent={} received={} malformed={} helloAttempts={} rtt={:.3f}ms "
                                  "appliedOnGameThread={} error='{}'",
                                  client::ToString(stats.state), stats.sent, stats.received, stats.malformed,
                                  stats.helloAttempts, stats.lastRttMs, stats.appliedOnGameThread, stats.lastError);

    CYBERMP_INFO("%s", text.c_str());

    return Red::CString(text.c_str());
}

Red::CString WorldSystem::GetRemoteStats()
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return "no world system";
    }

    const auto stats = Plugin::Net().GetStats();

    size_t spawned = 0;
    for (const auto& [playerId, body] : self->m_bodies)
    {
        if (body.entityId.IsDefined())
        {
            ++spawned;
        }
    }

    size_t samples = 0;
    for (const auto& [playerId, body] : self->m_bodies)
    {
        samples += body.samples.size();
    }

    const auto text = std::format("bodies={} spawned={} snapshots={} statesSent={} outOfOrder={} tracked={} "
                                  "moveMode={} moverWrites={} transformWrites={} moverMissing={} "
                                  "interp={} samples={} starved={}",
                                  self->m_bodies.size(), spawned, stats.statesReceived, stats.statesSent,
                                  stats.statesOutOfOrder, stats.remoteCount, self->m_moveMode.load(),
                                  self->m_moverWrites.load(), self->m_transformWrites.load(),
                                  self->m_moverMissing.load(), self->m_interpolate.load() ? "on" : "off", samples,
                                  self->m_starvedFrames.load());

    CYBERMP_INFO("%s", text.c_str());

    return Red::CString(text.c_str());
}

Red::CString WorldSystem::DumpRemoteComponents()
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return "no world system";
    }

    Red::EntityID target;
    for (const auto& [playerId, body] : self->m_bodies)
    {
        if (body.entityId.IsDefined())
        {
            target = body.entityId;
            break;
        }
    }

    if (!target.IsDefined())
    {
        return "no remote body spawned";
    }

    Red::Handle<Red::IScriptable> entitySystem;
    if (!Red::CallStatic("ScriptGameInstance", "GetDynamicEntitySystem", entitySystem) || !entitySystem)
    {
        return "no DynamicEntitySystem";
    }

    Red::Handle<Red::Entity> entity;
    if (!Red::CallVirtual(entitySystem.instance, "GetEntity", entity, target) || !entity)
    {
        return "body not resolved yet";
    }

    // Codeware adds GetComponents to Entity.
    Red::DynArray<Red::Handle<Red::IComponent>> components;
    if (!Red::CallVirtual(entity.instance, "GetComponents", components))
    {
        return "GetComponents failed";
    }

    const auto count = static_cast<uint32_t>(components.size());
    CYBERMP_INFO("body %llu has %u component(s):", target.hash, count);

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& component = components[i];
        if (!component)
        {
            continue;
        }

        const auto* type = component.instance->GetType();
        CYBERMP_INFO("  [%u] %s", i, type ? type->GetName().ToString() : "<no type>");
    }

    return Red::CString(std::format("{} component(s), see the log", count).c_str());
}

Red::CString WorldSystem::SetMoveMode(uint32_t aMode)
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return "no world system";
    }

    if (aMode > 3)
    {
        return "mode must be 0 (transform), 1 (mover), 2 (both) or 3 (teleport)";
    }

    self->m_moveMode.store(aMode, std::memory_order_relaxed);
    self->m_moverWrites = 0;
    self->m_transformWrites = 0;
    self->m_moverMissing = 0;

    static constexpr const char* kNames[] = {"transform only", "mover only", "both", "teleport facility"};
    CYBERMP_INFO("move mode %u (%s)", aMode, kNames[aMode]);

    return Red::CString(std::format("move mode {} ({})", aMode, kNames[aMode]).c_str());
}

Red::CString WorldSystem::SetInterpolation(bool aEnabled)
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return "no world system";
    }

    self->m_interpolate.store(aEnabled, std::memory_order_relaxed);
    self->m_starvedFrames = 0;

    CYBERMP_INFO("interpolation %s", aEnabled ? "on" : "off");

    return aEnabled ? "interpolation on" : "interpolation off (raw snapshots, stepped)";
}

Red::CString WorldSystem::SetBodyKind(uint32_t aKind)
{
    auto* self = Red::GetGameSystem<WorldSystem>();
    if (!self)
    {
        return "no world system";
    }

    if (aKind > 1)
    {
        return "kind must be 0 (npc) or 1 (prop)";
    }

    self->m_bodyKind.store(aKind, std::memory_order_relaxed);

    // Reset the entries instead of erasing them: a body is only created from a join
    // event, and the player already joined, so erasing would mean it never comes back.
    uint32_t reset = 0;
    for (auto& [playerId, body] : self->m_bodies)
    {
        DestroyBodyEntity(body.entityId);

        body.entityId = {};
        body.spawnRequested = false;
        body.lastTick = 0;
        body.samples.clear();
        ++reset;
    }

    static constexpr const char* kNames[] = {"npc", "prop"};
    CYBERMP_INFO("body kind %u (%s), %u body(ies) reset", aKind, kNames[aKind], reset);

    return Red::CString(
        std::format("body kind {} ({}), {} reset, wait for respawn", aKind, kNames[aKind], reset).c_str());
}

RTTI_DEFINE_CLASS(WorldSystem, "Cybermp.WorldSystem", {
    RTTI_METHOD(IsWorldReady);
    RTTI_METHOD(GetPlayerPosition);
    RTTI_METHOD(GetPlayerPositionText);
    RTTI_METHOD(SpawnProp);
    RTTI_METHOD(SpawnNpc);
    RTTI_METHOD(GetSpawnedCount);
    RTTI_METHOD(DespawnAll);
    RTTI_METHOD(GetTickStats);
    RTTI_METHOD(PostTasksFromThread);
    RTTI_METHOD(Connect);
    RTTI_METHOD(Disconnect);
    RTTI_METHOD(GetNetStats);
    RTTI_METHOD(GetRemoteStats);
    RTTI_METHOD(DumpRemoteComponents);
    RTTI_METHOD(SetMoveMode);
    RTTI_METHOD(SetBodyKind);
    RTTI_METHOD(SetInterpolation);
});

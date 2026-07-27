#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "NetClient.hpp"

#include <RedLib.hpp>

// RedLib.hpp doesn't pull the generated headers we need in this interface.
#include <RED4ext/Scripting/Natives/Generated/ent/EntityID.hpp>
#include <RED4ext/SystemUpdate.hpp>

#include "TaskQueue.hpp"

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

    static Red::CString GetTickStats();

    // Debug: pushes tasks from a worker thread to prove the bridge the network uses.
    // Nothing else in the plugin may touch the game off the game thread.
    static Red::CString PostTasksFromThread(int32_t aCount);

    static Red::CString Connect(uint32_t aPort, const Red::CString& aUsername);
    static Red::CString Disconnect();
    static Red::CString GetNetStats();
    static Red::CString GetRemoteStats();

    // Diagnostic: writing the transform only shows up once the game wakes the npc
    // (aiming at it does). Lists what a body actually carries, so we drive the right
    // component instead of guessing its type.
    static Red::CString DumpRemoteComponents();

    // 0 = SetWorldTransform, 1 = write the moveComponent, 2 = both,
    // 3 = TeleportationFacility, the game's own way of moving something.
    // Switchable at runtime so one session answers which one the engine honours,
    // instead of one build per guess.
    static Red::CString SetMoveMode(uint32_t aMode);

    // 0 = npc from a Character record, 1 = a template.
    // A record spawns a full ai driven puppet whose motion planner overwrites us; a
    // template spawns the body alone, which is what CyberpunkMP does with its .ent
    // files rather than records.
    static Red::CString SetBodyKind(uint32_t aKind);

    // Template used by kind 1. Settable from the console so candidates can be tried
    // without a rebuild -- the game ships base_entities like man_base.ent.
    static Red::CString SetBodyTemplate(const Red::CString& aPath);

    // Off replays raw snapshots, which is visibly stepped at 15 Hz. Kept switchable
    // so the difference can be seen rather than asserted.
    static Red::CString SetInterpolation(bool aEnabled);

private:
    void OnWorldAttached(Red::world::RuntimeScene* aScene) override;
    void OnWorldDetached(Red::world::RuntimeScene* aScene) override;

    void OnRegisterUpdates(Red::UpdateRegistrar* aRegistrar) override;

    // Signature is fixed by GroupUpdateCallback.
    void OnFrameBegin(Red::FrameInfo& aFrame, Red::JobQueue& aJobs);
    void OnMultiplayerUpdate(Red::FrameInfo& aFrame, Red::JobQueue& aJobs);

    void RunTick(Red::FrameInfo& aFrame, std::atomic_uint64_t& aCounter);

    // All of these run on a tick thread, never on the network thread.
    void PumpRemotePlayers(uint64_t aNowMs);
    void SendLocalState(uint64_t aNowMs);

    // A remote player is drawn by an npc for now. The game has no third person body
    // for V, so a real player model needs a custom .ent -- deferred on purpose.
    // One received snapshot, stamped with when it arrived here rather than with the
    // sender's tick: interpolation is driven by our own clock, and the two clocks are
    // not synchronised.
    struct Sample
    {
        uint64_t localMs{};
        float x{};
        float y{};
        float z{};
        float rotation{};
    };

    struct RemoteBody
    {
        Red::EntityID entityId;
        bool spawnRequested{};

        uint64_t lastTick{};
        std::vector<Sample> samples;
    };

    std::unordered_map<uint32_t, RemoteBody> m_bodies;
    std::vector<std::pair<uint32_t, client::RemoteSnapshot>> m_snapshotBuffer;
    uint64_t m_lastStateSentMs{};
    std::atomic_uint32_t m_moveMode{1};
    std::atomic_uint32_t m_bodyKind{0};
    std::mutex m_templateMutex;
    std::string m_bodyTemplate{"base\\characters\\base_entities\\man_base\\man_base.ent"};
    std::atomic_uint64_t m_moverWrites{};
    std::atomic_uint64_t m_transformWrites{};
    std::atomic_uint64_t m_moverMissing{};
    std::atomic_bool m_interpolate{true};
    std::atomic_uint64_t m_starvedFrames{};

    // Every tick counter is written from the game thread and read from the script
    // thread, so they are atomic rather than plain.
    std::atomic_uint64_t m_frameBeginTicks{};
    std::atomic_uint64_t m_multiplayerTicks{};
    std::atomic_uint64_t m_tasksRun{};
    std::atomic_uint64_t m_maxTickMicros{};
    std::atomic<float> m_lastDeltaTime{};

    // The engine ticks us from a worker pool, so the tick thread changes between
    // frames. Comparing the last tick thread to the last task thread proves nothing;
    // what matters is that a task body ran on the thread that was draining it.
    std::atomic_size_t m_tickThreadId{};
    std::atomic_size_t m_drainThreadId{};
    std::atomic_size_t m_producerThreadId{};
    std::atomic_uint64_t m_tickThreadChanges{};
    std::atomic_bool m_taskRanOffDrainThread{};

    std::jthread m_producer; // joins on destruction, so no detached thread outlives us

    bool m_worldAttached{};

    // We own what we spawn rather than leaning on the game's tags: the same registry
    // becomes the serverId -> EntityID map once puppets are networked.
    // Guarded because engine callbacks reach us from several threads.
    std::vector<Red::EntityID> m_spawned;
    std::mutex m_spawnedMutex;

    RTTI_IMPL_TYPEINFO(WorldSystem);
    RTTI_IMPL_ALLOCATOR();
};

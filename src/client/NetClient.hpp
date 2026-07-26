#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "Net.hpp"
#include "TaskQueue.hpp"

namespace client
{
enum class NetState : uint8_t
{
    Disconnected,
    Connecting,
    Connected,
    Refused,
};

const char* ToString(NetState aState);

struct NetStats
{
    NetState state{NetState::Disconnected};
    uint64_t sent{};
    uint64_t received{};
    uint64_t malformed{};
    uint64_t helloAttempts{};
    double lastRttMs{};
    uint64_t appliedOnGameThread{};
    std::string lastError;
};

// Owns the socket and its thread. Nothing here ever touches the engine: anything
// that has to reach the game is pushed to the task queue and runs inside the tick.
class NetClient
{
public:
    explicit NetClient(core::TaskQueue& aTasks);
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Replaces any current connection. Returns false only if the socket itself
    // could not be opened -- being refused by the server happens later, on the thread.
    bool Connect(const std::string& aHost, uint16_t aPort, const std::string& aUsername);
    void Disconnect();

    NetStats GetStats() const;

private:
    void Run(std::stop_token aStop);
    bool SendHello();
    bool SendPing();
    void HandleDatagram(std::span<const uint8_t> aData);

    void SetError(std::string aMessage);

    core::TaskQueue& m_tasks;

    net::Startup m_startup;
    net::UdpSocket m_socket;
    net::Endpoint m_server;
    std::string m_username;

    std::jthread m_thread;

    std::atomic<NetState> m_state{NetState::Disconnected};
    std::atomic_uint64_t m_sent{};
    std::atomic_uint64_t m_received{};
    std::atomic_uint64_t m_malformed{};
    std::atomic_uint64_t m_helloAttempts{};
    std::atomic<double> m_lastRttMs{};
    std::atomic_uint64_t m_appliedOnGameThread{};

    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};
} // namespace client

#include "Plugin.hpp"

#include <RedLib.hpp>

#include <Version.hpp>

#include "Log.hpp"
#include "NetClient.hpp"

const RED4ext::v1::Sdk* Plugin::s_sdk = nullptr;
RED4ext::v1::PluginHandle Plugin::s_handle = {};

// Function-local statics: built on first use, which keeps them out of Main(Load)
// where the game's allocators are not up yet.
core::TaskQueue& Plugin::Tasks()
{
    static core::TaskQueue s_tasks;
    return s_tasks;
}

client::NetClient& Plugin::Net()
{
    static client::NetClient s_net(Tasks());
    return s_net;
}

void Plugin::OnLoad(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    s_handle = aHandle;
    s_sdk = aSdk;

    // Queues every RTTI_DEFINE_* in the binary. Actual registration happens when the
    // game builds its RTTI, not right now.
    Red::TypeInfoRegistrar::RegisterDiscovered();

    // Game allocators aren't up yet, so keep this cheap. Real init goes in a game state later.
    const auto& runtime = *aSdk->runtime;
    CYBERMP_INFO("loaded -- cybermp %s (%s) -- game %u.%u.%u", CYBERMP_VERSION, CYBERMP_GIT_HASH,
                 static_cast<unsigned>(runtime.major), static_cast<unsigned>(runtime.minor),
                 static_cast<unsigned>(runtime.patch));
}

void Plugin::OnUnload()
{
    // Joins the network thread before the sdk pointer goes away, otherwise it could
    // still try to log through it.
    Net().Disconnect();
    Tasks().Clear();

    CYBERMP_INFO("unloaded");

    s_sdk = nullptr;
    s_handle = {};
}

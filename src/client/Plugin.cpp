#include "Plugin.hpp"

#include <RedLib.hpp>

#include "Log.hpp"

const RED4ext::v1::Sdk* Plugin::s_sdk = nullptr;
RED4ext::v1::PluginHandle Plugin::s_handle = {};

void Plugin::OnLoad(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    s_handle = aHandle;
    s_sdk = aSdk;

    // Queues every RTTI_DEFINE_* in the binary. Actual registration happens when the
    // game builds its RTTI, not right now.
    Red::TypeInfoRegistrar::RegisterDiscovered();

    // Game allocators aren't up yet, so keep this cheap. Real init goes in a game state later.
    const auto& runtime = *aSdk->runtime;
    CYBERMP_INFO("loaded -- game %u.%u.%u", static_cast<unsigned>(runtime.major),
                 static_cast<unsigned>(runtime.minor), static_cast<unsigned>(runtime.patch));
}

void Plugin::OnUnload()
{
    CYBERMP_INFO("unloaded");

    s_sdk = nullptr;
    s_handle = {};
}

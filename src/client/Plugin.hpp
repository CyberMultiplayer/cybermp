#pragma once

#include <RED4ext/RED4ext.hpp>

#include "TaskQueue.hpp"

namespace client
{
class NetClient;
}

// Holds what the loader hands us. Everything else reaches the sdk through here.
//
// The network lives here rather than in WorldSystem: a game system only lasts as
// long as a session, and a connection has to survive world transitions.
class Plugin
{
public:
    static void OnLoad(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
    static void OnUnload();

    // One queue for the whole plugin: the network pushes, the frame tick drains.
    static core::TaskQueue& Tasks();
    static client::NetClient& Net();

    static const RED4ext::v1::Sdk* GetSdk()
    {
        return s_sdk;
    }

    static RED4ext::v1::PluginHandle GetHandle()
    {
        return s_handle;
    }

private:
    static const RED4ext::v1::Sdk* s_sdk;
    static RED4ext::v1::PluginHandle s_handle;
};

#pragma once

#include <RED4ext/RED4ext.hpp>

// Holds what the loader hands us. Everything else reaches the sdk through here.
class Plugin
{
public:
    static void OnLoad(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
    static void OnUnload();

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

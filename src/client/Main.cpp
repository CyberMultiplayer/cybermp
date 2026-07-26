#include <RED4ext/RED4ext.hpp>

#include "Plugin.hpp"

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}

// Metadata only. No hooks, no allocations -- the loader calls this before any version check.
RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"cybermp";
    aInfo->author = L"akiti";
    aInfo->version = RED4EXT_V1_SEMVER(0, 1, 0);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST; // 3.0.80.51928 -- game 2.31
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    switch (aReason)
    {
    case RED4ext::v1::EMainReason::Load:
        Plugin::OnLoad(aHandle, aSdk);
        break;

    case RED4ext::v1::EMainReason::Unload:
        Plugin::OnUnload();
        break;
    }

    return true;
}

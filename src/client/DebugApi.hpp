#pragma once

#include <RedLib.hpp>

// First bridge into the game's RTTI. Scripts see it as "Cybermp.Debug".
// Static methods become static script functions, so GetSingleton() can call them.
struct DebugApi : Red::IScriptable
{
    static Red::CString Ping();
    static int32_t Add(int32_t a, int32_t b);

    RTTI_IMPL_TYPEINFO(DebugApi);
    RTTI_IMPL_ALLOCATOR();
};

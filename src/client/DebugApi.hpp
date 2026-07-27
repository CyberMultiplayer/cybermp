#pragma once

#include <RedLib.hpp>

// First bridge into the game's RTTI. Scripts see it as "Cybermp.Debug".
// Static methods become static script functions, so GetSingleton() can call them.
struct DebugApi : Red::IScriptable
{
    static Red::CString Ping();
    static int32_t Add(int32_t a, int32_t b);

    // Rtti explorer. Guessing native names has cost us several game sessions, and
    // Codeware's reds dump only lists its own additions, not vanilla classes.
    // Output goes to our log, which is readable without quitting the game.
    static Red::CString DumpMethods(const Red::CString& aClassName, const Red::CString& aFilter);
    static Red::CString FindClasses(const Red::CString& aFilter);

    RTTI_IMPL_TYPEINFO(DebugApi);
    RTTI_IMPL_ALLOCATOR();
};

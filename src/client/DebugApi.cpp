#include "DebugApi.hpp"

#include "Log.hpp"

Red::CString DebugApi::Ping()
{
    CYBERMP_INFO("Debug.Ping called from script");
    return "cybermp 0.1.0";
}

// Two args + a return value, to prove marshalling both ways.
int32_t DebugApi::Add(int32_t a, int32_t b)
{
    CYBERMP_INFO("Debug.Add(%d, %d) called from script", a, b);
    return a + b;
}

RTTI_DEFINE_CLASS(DebugApi, "Cybermp.Debug", {
    RTTI_METHOD(Ping);
    RTTI_METHOD(Add);
});

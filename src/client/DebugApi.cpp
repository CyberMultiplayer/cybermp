#include "DebugApi.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

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

namespace
{
bool Matches(const char* aName, const std::string& aFilter)
{
    if (aFilter.empty())
    {
        return true;
    }

    std::string name(aName ? aName : "");
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });

    return name.find(aFilter) != std::string::npos;
}

std::string Lowered(const Red::CString& aValue)
{
    std::string value(aValue.Length() > 0 ? aValue.c_str() : "");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });

    return value;
}

const char* TypeName(const Red::CProperty* aProperty)
{
    return aProperty && aProperty->type ? aProperty->type->GetName().ToString() : "?";
}

// Param types are what we actually need: a name alone still leaves the call to guess.
std::string Signature(const Red::CBaseFunction* aFunction)
{
    std::string text(aFunction->shortName.ToString());
    text += '(';

    for (uint32_t i = 0; i < static_cast<uint32_t>(aFunction->params.size()); ++i)
    {
        if (i > 0)
        {
            text += ", ";
        }

        const auto* param = aFunction->params[i];
        text += TypeName(param);

        if (param && param->name)
        {
            text += ' ';
            text += param->name.ToString();
        }
    }

    text += ") -> ";
    text += aFunction->returnType ? TypeName(aFunction->returnType) : "Void";

    return text;
}
} // namespace

Red::CString DebugApi::DumpMethods(const Red::CString& aClassName, const Red::CString& aFilter)
{
    if (aClassName.Length() == 0)
    {
        return "usage: DumpMethods('ClassName', 'filter or empty')";
    }

    auto* type = Red::GetClass(Red::CName(aClassName.c_str()));
    if (!type)
    {
        return Red::CString(std::format("no class named '{}'", aClassName.c_str()).c_str());
    }

    const auto filter = Lowered(aFilter);
    uint32_t shown = 0;

    // Walks the parents too: what we want is often inherited.
    for (const auto* current = type; current; current = current->parent)
    {
        CYBERMP_INFO("--- %s ---", current->name.ToString());

        for (auto* function : current->funcs)
        {
            if (function && Matches(function->shortName.ToString(), filter))
            {
                CYBERMP_INFO("  %s", Signature(function).c_str());
                ++shown;
            }
        }

        for (auto* function : current->staticFuncs)
        {
            if (function && Matches(function->shortName.ToString(), filter))
            {
                CYBERMP_INFO("  static %s", Signature(function).c_str());
                ++shown;
            }
        }
    }

    return Red::CString(std::format("{} method(s) logged", shown).c_str());
}

Red::CString DebugApi::FindClasses(const Red::CString& aFilter)
{
    const auto filter = Lowered(aFilter);
    if (filter.empty())
    {
        return "usage: FindClasses('substring') -- an empty filter would log everything";
    }

    auto* rtti = Red::CRTTISystem::Get();
    if (!rtti)
    {
        return "no rtti system";
    }

    uint32_t shown = 0;
    rtti->types.for_each([&](const Red::CName& aName, Red::CBaseRTTIType* const&) {
        // Capped: an unfiltered walk would log thousands of lines.
        if (shown < 200 && Matches(aName.ToString(), filter))
        {
            CYBERMP_INFO("  %s", aName.ToString());
            ++shown;
        }
    });

    return Red::CString(std::format("{} type(s) logged", shown).c_str());
}

RTTI_DEFINE_CLASS(DebugApi, "Cybermp.Debug", {
    RTTI_METHOD(Ping);
    RTTI_METHOD(Add);
    RTTI_METHOD(DumpMethods);
    RTTI_METHOD(FindClasses);
});

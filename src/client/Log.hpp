#pragma once

#include "Plugin.hpp"

// Output goes to red4ext/logs/cybermp-<timestamp>.log
#define CYBERMP_INFO(...) Plugin::GetSdk()->logger->InfoF(Plugin::GetHandle(), __VA_ARGS__)
#define CYBERMP_WARN(...) Plugin::GetSdk()->logger->WarnF(Plugin::GetHandle(), __VA_ARGS__)
#define CYBERMP_ERROR(...) Plugin::GetSdk()->logger->ErrorF(Plugin::GetHandle(), __VA_ARGS__)

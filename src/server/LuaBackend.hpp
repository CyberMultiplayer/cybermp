#pragma once

#include <filesystem>
#include <memory>

#include "Script.hpp"

namespace server
{
// Loads every .lua in a directory and dispatches server events to it.
// Missing directory is not an error: a server without scripts is a valid server.
std::unique_ptr<script::IBackend> MakeLuaBackend(std::filesystem::path aScriptDir);
} // namespace server

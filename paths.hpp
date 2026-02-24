#pragma once

#include <filesystem>

extern const char* ERRLOG_PERSIST_PATH;

namespace phosphor::logging::paths
{

auto error() -> std::filesystem::path;
auto extension() -> std::filesystem::path;

} // namespace phosphor::logging::paths

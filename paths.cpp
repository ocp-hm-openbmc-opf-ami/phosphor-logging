#include "config.h"

#include "paths.hpp"

namespace phosphor::logging::paths
{
auto error() -> std::filesystem::path
{
    return std::filesystem::path(ERRLOG_PERSIST_PATH) / "errors";
}
auto extension() -> std::filesystem::path
{
    return std::filesystem::path(ERRLOG_PERSIST_PATH) / "extensions";
}
} // namespace phosphor::logging::paths

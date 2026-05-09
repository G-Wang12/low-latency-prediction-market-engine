#pragma once

#include <cstddef>

namespace engine_config
{
    // Polymarket snapshots can contain hundreds of levels; a small queue can fill
    // and cause important updates (e.g., asks) to be dropped.
    static constexpr std::size_t kTickQueueSize = 65536;
} // namespace engine_config

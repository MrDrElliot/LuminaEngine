#pragma once

// Cross-DLL plumbing for entt's meta system: each DLL has its own locator,
// so non-runtime modules must seed via GetEngineMetaService() to share the registry.

#include <entt/entt.hpp>

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    /** Runtime-DLL meta context used for component/system/event registration. */
    RUNTIME_API entt::meta_ctx& GetEngineMetaContext();

    /** Pass to entt::locator<>::reset() in non-runtime DLLs to share the runtime registry. */
    RUNTIME_API entt::locator<entt::meta_ctx>::node_type GetEngineMetaService();
}

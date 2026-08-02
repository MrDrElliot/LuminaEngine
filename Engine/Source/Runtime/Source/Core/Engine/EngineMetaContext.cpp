#include "RuntimePCH.h"
#include "EngineMetaContext.h"

namespace Lumina
{
    entt::meta_ctx& GetEngineMetaContext()
    {
        return entt::locator<entt::meta_ctx>::value_or();
    }

    entt::locator<entt::meta_ctx>::node_type GetEngineMetaService()
    {
        return entt::locator<entt::meta_ctx>::handle();
    }
}

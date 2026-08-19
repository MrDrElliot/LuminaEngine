#pragma once

#include "entt/entt.hpp"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineMetaContext.h"
#include "World/Entity/Traits.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "World/Entity/Components/Component.h"


namespace Lumina::Meta
{
    template<typename TEvent>
    void RegisterECSEvent()
    {
        using namespace entt::literals;
        entt::meta_factory<TEvent>(GetEngineMetaContext())
            .type(TEvent::StaticStruct()->GetName().c_str())
            .traits(ECS::ETraits::Event);
    }
}

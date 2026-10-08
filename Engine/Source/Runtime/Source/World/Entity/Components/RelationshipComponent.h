#pragma once

#include "World/ECS/Entity.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    // The per-entity links worlds saved before ELuminaEngineVersion::REGISTRY_PARENT_LINKS, read only to load those files.
    struct LUM_DEPRECATED(0.1.41, "Parent links live in ECS::FHierarchy. Only old saves are read through this.") FRelationshipComponent
    {
        size_t       Children{};
        ECS::FEntity First{ ECS::NullEntity };
        ECS::FEntity Prev{ ECS::NullEntity };
        ECS::FEntity Next{ ECS::NullEntity };
        ECS::FEntity Parent{ ECS::NullEntity };
    };
}

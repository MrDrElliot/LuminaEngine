#pragma once

#include <entt/entt.hpp>

#include "Core/Serialization/Archiver.h"


namespace Lumina
{
    struct RUNTIME_API FRelationshipComponent
    {
        size_t          Children{};
        entt::entity    First{entt::null};
        entt::entity    Prev{entt::null};
        entt::entity    Next{entt::null};
        entt::entity    Parent{entt::null};
        
        friend FArchive& operator << (FArchive& Ar, FRelationshipComponent& Data)
        {
            Ar << Data.Children;
            Ar << Data.First;
            Ar << Data.Prev;
            Ar << Data.Next;
            Ar << Data.Parent;
            
            return Ar;
        }
    };

    struct FParentEntityTag { };
    struct FChildEntityTag { };
}

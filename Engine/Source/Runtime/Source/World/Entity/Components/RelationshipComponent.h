#pragma once

#include "World/ECS/Registry.h"



#include "Core/Serialization/Archiver.h"


namespace Lumina
{
    struct RUNTIME_API FRelationshipComponent
    {
        size_t          Children{};
        ECS::FEntity    First{ECS::NullEntity};
        ECS::FEntity    Prev{ECS::NullEntity};
        ECS::FEntity    Next{ECS::NullEntity};
        ECS::FEntity    Parent{ECS::NullEntity};
        
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

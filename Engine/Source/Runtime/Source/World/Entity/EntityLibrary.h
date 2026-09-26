#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Containers/Vector.h"
#include "World/ECS/Entity.h"
#include "EntityLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    // Hierarchy walks over the relationship component, which the world stores but does not otherwise own.
    REFLECT()
    class RUNTIME_API CEntityLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** Tags the entity so CWorld::FindByTag finds it. An empty tag clears whatever it had. */
        FUNCTION()
        static void SetTag(CWorld* World, ECS::FEntity Entity, const FName& Tag);

        /** The tag the entity answers to, or None. */
        FUNCTION()
        static FName GetTag(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static ECS::FEntity GetFirstChild(CWorld* World, ECS::FEntity Entity);

        FUNCTION()
        static ECS::FEntity GetNextSibling(CWorld* World, ECS::FEntity Entity);

        /** Entity first, then each parent up to the root. One crossing rather than a call per hop. */
        FUNCTION()
        static void GetAncestorChain(CWorld* World, ECS::FEntity Entity, TVector<ECS::FEntity>& Out);

        /** Entity and every descendant, in an unspecified order. */
        FUNCTION()
        static void GetSubtree(CWorld* World, ECS::FEntity Entity, TVector<ECS::FEntity>& Out);
    };
}
